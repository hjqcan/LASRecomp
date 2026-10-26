"""Exercise production heap segmentation and emitted LDG/STG across segment boundaries."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "shader.h"
#include "../vulkan/gmem_segments.h"
int main(int argc, char **argv) {
    uint64_t off, range;
    uint32_t words = gmem_segment_words(0xd0000000ull,1073741820,64);
    assert(words == 268435440);
    uint64_t total=0;
    for (uint32_t i=0;i<4;i++) {
        assert(gmem_segment_range(0xd0000000ull,words,i,&off,&range));
        assert(off%64==0 && range<=1073741820 && off==total); total+=range;
    }
    assert(total==0xd0000000ull);
    assert(!gmem_segment_range(total,words,4,&off,&range));
    assert(gmem_segment_words(0x10000000,0x40000000,256)==0);
    assert(gmem_segment_words(0xd0000000ull,0x8000000,256)==0x2000000);
    assert(gmem_segment_words(40,2,64)==UINT32_MAX);
    if(argc!=5)return 2;
    int stage=atoi(argv[2]); uint64_t blob[20]={0};
    int pos=stage==STAGE_COMPUTE?1:17;
    blob[pos]=strtoull(argv[1],NULL,16); blob[pos+1]=0xe30000000007000full;
    ShaderKey key={.glsl=1,.gmem_chunk_words=(uint32_t)atoi(argv[3])};
    maxwell_gmem_base=0x1000; maxwell_gmem_size=strtoull(argv[4],NULL,0);
    ShaderInfo *s=maxwell_translate((const uint8_t*)blob,(pos+2)*8,stage,&key);
    if(!s->msl){fprintf(stderr,"%s\n",s->error);maxwell_free(s);return 1;}
    assert(s->gmem_chunk_words==key.gmem_chunk_words);
    assert(s->uses_gmem_store==!!(blob[pos]&(1ull<<51)));
    puts(s->msl);maxwell_free(s);return 0;
}
'''
PRELUDE = r'''
#include <cassert>
#include <cstdint>
#include <vector>
using uint=uint32_t;
struct Words : std::vector<uint> { using std::vector<uint>::vector; size_t length()const{return size();} };
struct Buf { Words w; };
static void atomicAnd(uint &x,uint v){x&=v;}
static void atomicOr(uint &x,uint v){x|=v;}
static int sx8(uint x){return int8_t(x);}
static int sx16(uint x){return int16_t(x);}
'''
cc=shlex.split(os.environ.get('CC','clang'))
cxx=shlex.split(os.environ.get('CXX','clang++'))
with tempfile.TemporaryDirectory(prefix='gmem-') as tmp:
    temp=Path(tmp); src=temp/'generate.c'; src.write_text(GENERATOR)
    gen=temp/('generate.exe' if os.name=='nt' else 'generate')
    subprocess.run(cc+['-std=gnu11','-Iruntime/nvn/maxwell',str(src),'runtime/nvn/maxwell/translate.c',
                      *([] if os.name=='nt' else ['-lm']),'-o',str(gen)],cwd=ROOT,check=True)
    cases=[]
    for stage in (0,5):
        for store in (False,True):
            for size in range(7):
                word=0xeed0000000070004 | int(store)<<51 | size<<48 | 1<<45
                shader=subprocess.check_output([str(gen),f'{word:x}',str(stage),'4','40'],text=True)
                assert 'binding = 17)' in shader and 'binding = 96)' in shader and 'binding = 97)' in shader
                helpers=shader[shader.index('uint gmem_load('):]
                helpers=helpers[:min(helpers.index(s) for s in ('layout(', 'invariant ', 'void main()') if s in helpers)]
                body=shader[shader.index('  do {')+len('  do {'):shader.index('  } while (false);')]
                ns=f'case_{len(cases)}'
                cases.append(ns)
                code=f'namespace {ns} {{\nBuf gmem0_,gmem1_,gmem2_,gfast_;\nbool fast=false;\n'
                code+='uint gfind(uint g){return fast?g/4:0xffffffffu;}\nuint gfindw(uint g){return gfind(g);}\n'+helpers
                code+='void run(){for(bool use_fast : {false,true}) for(uint ga : {14u,15u,12u,36u,40u}){\n'
                code+=f'fast=use_fast && {str(stage==5).lower()};\n'
                code+='gmem0_.w={0x98765432u,0x98765433u,0x98765434u,0x98765435u};\n'
                code+='gmem1_.w={0x98765436u,0x98765437u,0x98765438u,0x98765439u};gmem2_.w={0x9876543au,0x9876543bu};\n'
                code+='gfast_.w=Words(16,0);for(uint i=0;i<10;i++)gfast_.w[i]=gmem_load(i);\n'
                # Fast-copy path has padded allocation, and this test concerns segment routing.
                code+='if(fast && ga>=36)continue;\n'
                code+='uint r0=0x1000+ga,r1=0,r4=0x89abcdefu,r5=0x76543210u,r6=0x11223344u,r7=0x55667788u;\n'
                code+='uint input[4]={r4,r5,r6,r7};std::vector<uint> expected;for(uint i=0;i<10;i++)expected.push_back(gmem_load(i));\n'
                if store:
                    if size<4:
                        mask='0xffu' if size<2 else '0xffffu'; shift='(ga&3)*8' if size<2 else '(ga&2)*8'
                        code+=f'if(ga<40){{uint sh={shift};expected[ga/4]=(expected[ga/4]&~({mask}<<sh))|((input[0]&{mask})<<sh);}}\n'
                    else:
                        count=1 if size==4 else 2 if size==5 else 4
                        code+=f'if(ga<40)for(uint i=0;i<{count} && ga/4+i<10;i++)expected[ga/4+i]=input[i];\n'
                code+=body
                if store:
                    code+='for(uint i=0;i<10;i++)assert((fast?gfast_.w[i]:gmem_load(i))==expected[i]);\n'
                else:
                    if size<4:
                        shift='(ga&3)*8' if size<2 else '(ga&2)*8'
                        expression={0:'v&0xffu',1:'uint(int32_t(int8_t(v)))',2:'v&0xffffu',3:'uint(int32_t(int16_t(v)))'}[size]
                        code+=f'uint v=ga<40?expected[ga/4]>>({shift}):0;assert(r4==({expression}));\n'
                    else:
                        count=1 if size==4 else 2 if size==5 else 4
                        code+='uint result[4]={r4,r5,r6,r7};'
                        code+=f'for(uint i=0;i<{count};i++)assert(result[i]==(ga<40 && ga/4+i<10?expected[ga/4+i]:0u));\n'
                code+='assert(gmem_load(10)==0 && gmem_load(12)==0);\n} } }\n'
                (temp/f'{ns}.inc').write_text(code)
                if os.environ.get('GLSLC'):
                    gl=temp/('shader.comp' if stage==5 else 'shader.vert');gl.write_text(shader)
                    subprocess.run([os.environ['GLSLC'],str(gl),'-o',str(temp/'shader.spv')],check=True)
    cpp=temp/'execute.cpp'
    cpp.write_text(PRELUDE+''.join((temp/f'{ns}.inc').read_text() for ns in cases)+
                   'int main(){'+''.join(f'{ns}::run();' for ns in cases)+'}')
    exe=temp/('execute.exe' if os.name=='nt' else 'execute')
    subprocess.run(cxx+['-std=c++17',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    legacy=subprocess.check_output([str(gen),'eed4200000070004','0','0','40'],text=True)
    assert 'gmem_.w[' in legacy and 'gmem_load(' not in legacy
    rejected=subprocess.run([str(gen),'eed4200000070004','0','1','132'],text=True,capture_output=True)
    assert rejected.returncode==1 and 'too many' in rejected.stderr
print('PASS: device-aligned 3.25 GB segmentation; LDG/STG widths, signed loads, cross-segment vectors, tails and compute fast copy')
