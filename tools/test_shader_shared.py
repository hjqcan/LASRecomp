"""Execute production LDS/STS translations with concurrent workgroup lanes.

The emitted GLSL bodies run as C++20 with atomic operations and a workgroup
barrier. Optional GLSLC also validates the complete generated compute shaders.
No game code or shader data is needed.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "shader.h"
static uint64_t blob[128];
static int at;
static void emit(uint64_t w) { if (!(at % 4)) at++; blob[at++] = w | 0x70000; }
static uint64_t shl(int d, int a, int n) { return 0x3848000000000000ull | d | (a << 8) | ((uint64_t)n << 20); }
static uint64_t add(int d, int a, uint32_t n) { return 0x1c00000000000000ull | d | (a << 8) | ((uint64_t)n << 20); }
static uint64_t ls(int store, int size, int d, int a, int off) {
    return (store ? 0xef58000000000000ull : 0xef48000000000000ull) |
        ((uint64_t)size << 48) | d | (a << 8) | ((uint64_t)(off & 0xffffff) << 20);
}
int main(int argc, char **argv) {
    if (argc != 5) return 2;
    int st = atoi(argv[1]), ld = atoi(argv[2]), dialect = atoi(argv[3]), mode = atoi(argv[4]);
    int shift = st < 2 ? 0 : st < 4 ? 1 : st - 2;
    emit(0xf0c8000002100000ull); // S2R R0, tid.x
    emit(0xf0c8000002500001ull); // S2R R1, ctaid.x
    emit(add(16, 0, 0x80808080u));
    emit(0x5c10000000101010ull); // IADD R16, R16, R1
    for (int k = 1; k < 4; k++) emit(add(16+k, 16, (uint32_t)k * 0x10000u));
    emit(shl(8, 0, shift));
    emit(ls(1, st, 16, 8, 0));
    emit(0xf0a8000000000000ull); // BAR.SYNC 0
    // Align the address to the load width and use a signed displacement.
    int align = ld < 2 ? 0 : ld < 4 ? 1 : 2;
    emit(0x382800000000000aull | (8 << 8) | ((uint64_t)align << 20)); // SHR R10, R8, align
    emit(shl(10, 10, align));
    emit(add(10, 10, 16));
    emit(ls(0, ld, 10, 10, -16)); // overlapping address/destination, vector loads included
    emit(shl(9, 1, 9)); // group * (32 lanes * 16 bytes)
    emit(shl(8, 0, 4));
    emit(0x5c10000000800909ull); // IADD R9, R9, R8
    emit(0xeede00000000090aull); // STG.128 [R9], R10..R13
    emit(0xe30000000000000full); // EXIT
    ShaderKey key = {.glsl = dialect, .shared_memory = mode < 0 ? 0 : mode ? (uint32_t)mode : (32u << shift)};
    maxwell_gmem_base = 0; maxwell_gmem_size = 4096;
    ShaderInfo *s = maxwell_translate((const uint8_t *)blob, sizeof blob, STAGE_COMPUTE, &key);
    if (!s->msl) { fprintf(stderr, "%s\n", s->error); return 1; }
    puts(s->msl); maxwell_free(s); return 0;
}
'''

EXECUTOR = r'''
#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>
using uint = uint32_t;
using uint64_t = std::uint64_t;
struct Vec { uint x, y, z; };
thread_local Vec gl_LocalInvocationID, gl_WorkGroupID;
struct Group { uint shared[512]{}; std::barrier<> sync{32}; };
thread_local Group *group;
#define smem group->shared
struct Memory { uint w[1024]{}; } gmem_, gfast_;
static uint gfind(uint) { return 0xffffffffu; }
static uint gfindw(uint g) { return gfind(g); }
static int sx8(uint v) { return int(int8_t(v)); }
static int sx16(uint v) { return int(int16_t(v)); }
static uint atomicAdd(uint &v, uint x) { return std::atomic_ref<uint>(v).fetch_add(x); }
static uint atomicExchange(uint &v, uint x) { return std::atomic_ref<uint>(v).exchange(x); }
static uint atomicCompSwap(uint &v, uint old, uint next) {
    std::atomic_ref<uint>(v).compare_exchange_strong(old, next); return old;
}
static void barrier() { group->sync.arrive_and_wait(); }
SHADER_BODY
int main() {
    constexpr int store_size = STORE_SIZE, load_size = LOAD_SIZE;
    constexpr int stride = store_size < 2 ? 1 : store_size < 4 ? 2 : 1 << (store_size-2);
    constexpr int count = load_size == 5 ? 2 : load_size == 6 ? 4 : 1;
    for (int repeat = 0; repeat < 12; repeat++) {
        Group groups[4];
        std::vector<std::thread> threads;
        for (uint g = 0; g < 4; g++) for (uint lane = 0; lane < 32; lane++)
            threads.emplace_back([&, g, lane] {
                group = &groups[g]; gl_WorkGroupID = {g,0,0}; gl_LocalInvocationID = {lane,0,0}; shader_main();
            });
        for (auto &t : threads) t.join();
        for (uint g = 0; g < 4; g++) {
            std::array<uint8_t, 528> reference{};
            for (uint lane = 0; lane < 32; lane++) for (int byte = 0; byte < stride; byte++) {
                uint word = 0x80808080u + lane + g + uint(byte/4) * 0x10000u;
                reference[lane*stride+byte] = uint8_t(word >> ((byte%4)*8));
            }
            for (uint lane = 0; lane < 32; lane++) for (int word = 0; word < count; word++) {
                uint address = lane*stride;
                address &= load_size < 2 ? ~0u : load_size < 4 ? ~1u : ~3u;
                address += word*4;
                uint value = 0;
                int bytes = load_size < 2 ? 1 : load_size < 4 ? 2 : 4;
                if (address < 32*stride) std::memcpy(&value, reference.data()+address, bytes);
                if (load_size == 1) value = uint(sx8(value));
                if (load_size == 3) value = uint(sx16(value));
                assert(gmem_.w[(g*32+lane)*4+word] == value);
            }
        }
    }
}
'''

def main():
    cc = shlex.split(os.environ.get('CC', 'clang'))
    cxx = shlex.split(os.environ.get('CXX', 'clang++'))
    glslc = os.environ.get('GLSLC')
    cases = [(0, n) for n in range(5)] + [(2, n) for n in (2, 3, 4)] + [(4, n) for n in (4, 5, 6)] + [(5, 5), (6, 6)]
    with tempfile.TemporaryDirectory() as tmp:
        temp = Path(tmp); source = temp/'generate.c'; source.write_text(GENERATOR)
        gen = temp/('generate.exe' if os.name == 'nt' else 'generate')
        subprocess.run(cc + ['-std=gnu11', '-Iruntime/nvn/maxwell', str(source), 'runtime/nvn/maxwell/translate.c',
                            *([] if os.name == 'nt' else ['-lm']), '-o', str(gen)], cwd=ROOT, check=True)
        for st, ld, size in [(4, 4, -1), (4, 4, 98305), (7, 4, 0), (4, 7, 0)]:
            rejected = subprocess.run([str(gen), str(st), str(ld), '1', str(size)], text=True, capture_output=True)
            assert rejected.returncode == 1 and 'shared memory' in rejected.stderr
        for st, ld in cases:
            shader = subprocess.check_output([str(gen), str(st), str(ld), '1', '0'], text=True)
            body = shader[shader.index('void main()'):].replace('void main()', 'void shader_main()', 1)
            body = '\n'.join(line for line in shader.splitlines()
                             if line.startswith(('#define GMR(', '#define GMW(', '#define GMAND(', '#define GMOR('))) + '\n' + body
            cpp = temp/'execute.cpp'
            cpp.write_text(EXECUTOR.replace('SHADER_BODY', body).replace('STORE_SIZE', str(st)).replace('LOAD_SIZE', str(ld)))
            exe = temp/('execute.exe' if os.name == 'nt' else 'execute')
            subprocess.run(cxx + ['-std=c++20', '-O1', '-pthread', str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
            if glslc:
                comp = temp/'test.comp'; comp.write_text(shader)
                subprocess.run([glslc, str(comp), '-o', str(temp/'test.spv')], check=True)
            metal = subprocess.check_output([str(gen), str(st), str(ld), '0', '0'], text=True)
            assert 'threadgroup atomic_uint smem[' in metal and 'threadgroup_barrier' in metal
    print('PASS: LDS/STS signed and vector accesses, overlapping registers, bounds, concurrent byte/halfword stores, workgroup isolation')


if __name__ == '__main__':
    main()
