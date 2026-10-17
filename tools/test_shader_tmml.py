"""Validate TMML decoding, coordinate snapshots and signed fixed-point LOD results.

Semantics: envytools/envydis/gm107.c's TMML fields and Ryujinx's EmitTmml
(texture component order is reversed from GLSL, with 8 fractional bits).
Synthetic shaders contain no game assets. GLSLC optionally checks full shaders.
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
int main(int argc, char **argv) {
    if (argc != 6) return 2;
    uint64_t blob[19] = {0};
    blob[17] = strtoull(argv[1],NULL,16);
    blob[18] = 0xe30000000007000full;
    ShaderKey key = {.glsl=atoi(argv[2])};
    ShaderInfo *s = maxwell_translate((const uint8_t *)blob,sizeof blob,atoi(argv[3]),&key);
    if (!s->msl) { fprintf(stderr,"%s\n",s->error); maxwell_free(s); return 1; }
    if (s->ntex && (s->tex[0].type != atoi(argv[4]) || s->tex[0].handle_offset != (uint32_t)atoi(argv[5]))) return 3;
    puts(s->msl); maxwell_free(s); return 0;
}
'''
EXECUTOR = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
using uint=uint32_t;
struct vec2 { float x,y; vec2(float a,float b):x(a),y(b){} };
struct vec3 { float x,y,z; vec3(float a,float b,float c):x(a),y(b),z(c){} };
static float expected[3];
static unsigned queries;
static int t0;
static float as_f(uint x) { float f; std::memcpy(&f,&x,4); return f; }
static uint bits(float f) { uint x; std::memcpy(&x,&f,4); return x; }
static vec2 query() { ++queries; return vec2(3.751f,-1.126f); }
static vec2 textureQueryLod(int,float p) { assert(p==expected[0]); return query(); }
static vec2 textureQueryLod(int,vec2 p) { assert(p.x==expected[0] && p.y==expected[1]); return query(); }
static vec2 textureQueryLod(int,vec3 p) { assert(p.x==expected[0] && p.y==expected[1] && p.z==expected[2]); return query(); }
int main() {
    DECLARATIONS
    INITIALIZE
    BODY
    CHECKS
}
'''

def instruction(dim, array, mask, dest=2, source=2, index=8):
    return 0xdf58000000070000 | dest | source << 8 | array << 28 | dim << 29 | mask << 31 | index << 36

cc = shlex.split(os.environ.get('CC', 'clang'))
cxx = shlex.split(os.environ.get('CXX', 'clang++'))
with tempfile.TemporaryDirectory(prefix='tmml-') as tmp:
    temp = Path(tmp)
    source = temp/'generate.c'; source.write_text(GENERATOR)
    gen = temp/('generate.exe' if os.name == 'nt' else 'generate')
    subprocess.run(cc + ['-std=gnu11', '-Iruntime/nvn/maxwell', str(source), 'runtime/nvn/maxwell/translate.c',
                        *([] if os.name == 'nt' else ['-lm']), '-o', str(gen)], cwd=ROOT, check=True)
    types = [(0,0,0), (0,1,6), (1,0,1), (1,1,4), (2,0,2), (3,0,3), (3,1,5)]
    cases = [(dim,array,kind,mask,2,2) for dim,array,kind in types for mask in (1,2,3)]
    cases += [(1,0,1,3,254,254), (1,0,1,2,1,2), (1,0,1,2,8,0)]
    for dim,array,kind,mask,dest,src in cases:
        word=instruction(dim,array,mask,dest,src)
        shader=subprocess.check_output([str(gen),f'{word:x}','1','1',str(kind),'32'],text=True)
        snippet=shader[shader.index('    { // TMML'):shader.index('    } // TMML end')+len('    } // TMML end')]
        coord_count=1 if dim==0 else 2 if dim==1 else 3
        first=src+array
        init=[]
        for i in range(coord_count):
            reg=first+i
            value=(i+1)*0.25 if reg<255 else 0.0
            init.append(f'expected[{i}]={value}f;')
            if reg<255:init.append(f'r{reg}=bits({value}f);')
        checks=['assert(queries==1);']
        output=dest
        for component,value in enumerate((-288,960)):
            if mask & (1<<component):
                if output<255:checks.append(f'assert(r{output}==uint(int32_t({value})));')
                output+=1
        cpp=temp/'execute.cpp'
        cpp.write_text(EXECUTOR.replace('DECLARATIONS',''.join(f'uint r{i}=0;' for i in range(255)))
                       .replace('INITIALIZE',''.join(init)).replace('BODY',snippet).replace('CHECKS',''.join(checks)))
        exe=temp/('execute.exe' if os.name=='nt' else 'execute')
        subprocess.run(cxx+['-std=c++17',str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
        if os.environ.get('GLSLC'):
            frag=temp/'shader.frag'; frag.write_text(shader)
            subprocess.run([os.environ['GLSLC'],str(frag),'-o',str(temp/'shader.spv')],check=True)
    for word,dialect,stage,message in [(instruction(2,1,3),1,1,'3d array'),
                                       (instruction(1,0,4),1,1,'non-LOD'),
                                       (instruction(1,0,3),0,1,'GLSL fragment'),
                                       (instruction(1,0,3),1,0,'GLSL fragment')]:
        run=subprocess.run([str(gen),f'{word:x}',str(dialect),str(stage),'1','32'],text=True,capture_output=True)
        assert run.returncode==1 and message in run.stderr,run
    for word in (instruction(1,0,3,dest=255), instruction(1,0,0)):
        shader=subprocess.check_output([str(gen),f'{word:x}','1','1','1','32'],text=True)
        assert 'textureQueryLod(' not in shader
print('PASS: TMML masks/types, signed fixed-point LODs, array coordinates, overlapping/RZ registers and rejected variants')
