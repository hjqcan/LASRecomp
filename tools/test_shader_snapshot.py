"""Compile the production snapshot helper; release/mutate inputs before translation."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'runtime/nvn/vulkan/exec.c').read_text()
start=source.index('static void *shader_snapshot(')
helper=source[start:source.index('\n}\n',start)+3]
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "shader.h"
#define FATAL(...) abort()
static bool shader_debug(void) { return false; }
'''+helper+r'''
int main(void) {
    uint64_t blob[24]={0}, *code=blob+16;
    code[1]=0x0100000000070002ull | ((uint64_t)0x3fc00000 << 20);
    code[2]=0xeff000000707ff02ull;
    code[3]=0xe30000000007000full;
    code[5]=0xe24000000007000full | ((uint64_t)0xfffff8 << 20);
    uint64_t size=0x10000;
    void *copy=shader_snapshot((uintptr_t)blob,&size,STAGE_VERTEX);
    assert(size==0xa0 && !memcmp(copy,blob,size));
    ShaderKey key={.glsl=1};
    ShaderInfo *before=maxwell_translate((uint8_t *)blob,sizeof blob,STAGE_VERTEX,&key);
    memset(blob,0xff,sizeof blob);
    ShaderInfo *after=maxwell_translate(copy,size,STAGE_VERTEX,&key);
    assert(before->msl && after->msl && !strcmp(before->msl,after->msl));
    maxwell_free(before); maxwell_free(after); free(copy);
    assert(maxwell_code_size((uint8_t *)blob,0x20,STAGE_VERTEX)==0);
    size=0x20; assert(!shader_snapshot((uintptr_t)blob,&size,STAGE_VERTEX) && size==0);
    size=0x10000; assert(!shader_snapshot(0,&size,STAGE_VERTEX) && size==0);
    ShaderInfo *bad=maxwell_translate((uint8_t *)blob,0x20,STAGE_FRAGMENT,&key);
    assert(!bad->msl && strstr(bad->error,"truncated")); maxwell_free(bad);
    puts("PASS: shader snapshots stop at padding, survive source reuse and reject truncated headers");
}
'''
with tempfile.TemporaryDirectory(prefix='las-shader-snapshot-') as folder:
    folder=Path(folder); src=folder/'snapshot.c'; src.write_text(harness)
    compiler=shlex.split(os.environ.get('CC','cc'))
    for name,inputs in [('snapshot',[src,root/'runtime/nvn/maxwell/translate.c']),
                        ('prefetch',[root/'tools/test_shader_prefetch.c'])]:
        exe=folder/(name+('.exe' if os.name=='nt' else ''))
        subprocess.run([*compiler,'-std=gnu11','-O2','-I',str(root/'runtime/nvn/maxwell'),
                        *map(str,inputs),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
