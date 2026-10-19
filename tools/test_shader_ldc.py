"""Execute production-translated LDC shaders against known constant buffers.

The synthetic shader bodies use scalar C++-compatible GLSL. Running those
bodies natively tests address arithmetic and overlapping destinations without
requiring a GPU or distributing game shader binaries.
"""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "shader.h"
int main(int argc, char **argv) {
    if (argc != 4 && argc != 5) return 2;
    uint64_t blob[24] = {0};
    uint64_t *code = blob + 16;
    uint32_t base = (uint32_t)strtoul(argv[1], NULL, 0);
    uint16_t offset = (uint16_t)strtol(argv[2], NULL, 0);
    int count = atoi(argv[3]);
    code[1] = 0x0100000000070002ull | ((uint64_t)base << 20); // MOV32I R2, base
    code[2] = 0xef90006000070202ull | ((uint64_t)offset << 20) |
              ((uint64_t)(count == 2 ? 5 : 4) << 48); // LDC R2, c6[R2 + offset]
    if (argc == 5) code[2] = (code[2] & ~0xff00ull) | 0xff00ull; // RZ: constant address
    code[3] = 0xeff000000707ff02ull | ((uint64_t)(count - 1) << 47); // ST position, R2
    code[5] = 0xe30000000007000full; // EXIT
    ShaderKey key = {.glsl = 1};
    ShaderInfo *s = maxwell_translate((const uint8_t *)blob, sizeof blob, STAGE_VERTEX, &key);
    if (!s->msl) { fprintf(stderr, "%s\n", s->error); return 1; }
    puts(s->msl);
    maxwell_free(s);
    return 0;
}
'''
EXECUTOR = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
using uint = uint32_t;
struct vec4 {
    float v[4];
    vec4(float a, float b, float c, float d) : v{a,b,c,d} {}
    float &operator[](uint i) { assert(i < 4); return v[i]; }
};
static vec4 gl_Position(0,0,0,1);
static uint cb6[16384];
static float as_f(uint u) { float v; std::memcpy(&v, &u, 4); return v; }
static uint cbr(const uint *buffer, uint byte) {
    assert(byte %% 4 == 0 && byte < sizeof cb6);
    return buffer[byte / 4];
}
%s
int main() {
    for (uint i = 0; i < 16384; i++) {
        float value = float(i) + 0.25f;
        std::memcpy(cb6 + i, &value, 4);
    }
    shader_main();
    assert(gl_Position[0] == %sf);
    %s
}
'''

cc = shlex.split(os.environ.get('CC', 'clang'))
cxx = shlex.split(os.environ.get('CXX', 'clang++'))
assert shutil.which(cc[0]) and shutil.which(cxx[0]), 'C and C++ compilers are required'
cases = [(32, -16, 2), (32784, -32768, 2), (0, 32764, 1), (0, 16, 2), (16, 0, 2)]
with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    generator = root / 'generate.c'
    generator.write_text(GENERATOR)
    executable = root / ('generate.exe' if os.name == 'nt' else 'generate')
    subprocess.run(cc + ['-std=gnu11', '-Iruntime/nvn/maxwell', str(generator),
                        'runtime/nvn/maxwell/translate.c', *([] if os.name == 'nt' else ['-lm']), '-o', str(executable)],
                   cwd=ROOT, check=True)
    for displacement in (-4, -16, -32768):
        rejected = subprocess.run([str(executable), '0', str(displacement), '1', 'static'], capture_output=True, text=True)
        assert rejected.returncode == 1 and 'negative static cbuf address' in rejected.stderr
    constant = subprocess.check_output([str(executable), '0', '16', '1', 'static'], text=True)
    assert 'cb6[1][0]' in constant
    for index, (base, displacement, count) in enumerate(cases):
        shader = subprocess.check_output([str(executable), str(base), str(displacement), str(count)], text=True)
        body = shader[shader.index('void main()'):].replace('void main()', 'void shader_main()', 1)
        expected = (base + displacement) // 4 + 0.25
        second = f'assert(gl_Position[1] == {expected + 1}f);' if count == 2 else ''
        source = root / f'case{index}.cpp'
        source.write_text(EXECUTOR % (body, expected, second))
        test = root / ('test.exe' if os.name == 'nt' else 'test')
        subprocess.run(cxx + ['-std=c++17', str(source), '-o', str(test)], check=True)
        subprocess.run([str(test)], check=True)
print('PASS: signed LDC displacements, boundary offsets and overlapping 64-bit destinations')
