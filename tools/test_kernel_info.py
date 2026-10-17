"""Run the production GetInfo branch against the declared host address layout."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
kernel=(ROOT/'runtime/kernel.c').read_text(encoding='utf8')
constants=kernel[kernel.index('#undef R_OK'):kernel.index('#define WAITERS_BIT')]
handler=kernel[kernel.index('    case 0x29:'):kernel.index('    case 0x2C:')]
source=r'''
#include <assert.h>
#include "kernel.h"
#include "research.h"
static uint64_t heap_size=0x12340000;
static TitleProfile profile={.program_id=UINT64_C(0x0100f43008c44000)};
const TitleProfile *title_profile_current(void) { return &profile; }
uint32_t kernel_current_thread_handle(void) { return 1; }
void research_external(int a,uint64_t b,uint32_t c,uint64_t d,uint64_t e) {(void)a;(void)b;(void)c;(void)d;(void)e;}
''' + constants + '\nstatic void info(uint64_t *x) { uint64_t pc=0; switch(0x29) {\n'+handler+'\n} }\n'+r'''
static uint64_t query(uint64_t id,uint64_t handle,uint64_t sub,uint32_t result) {
    uint64_t x[4]={0,id,handle,sub}; info(x);assert(x[0]==result);return x[1];
}
int main(void) {
    assert(query(2,PSEUDO_PROCESS,0,R_OK)==ALIAS_BASE);
    assert(query(3,PSEUDO_PROCESS,0,R_OK)==ALIAS_SIZE);
    assert(query(4,PSEUDO_PROCESS,0,R_OK)==HEAP_BASE);
    assert(query(5,PSEUDO_PROCESS,0,R_OK)==HEAP_MAX);
    assert(query(7,PSEUDO_PROCESS,0,R_OK)==heap_size+0x4000000);
    assert(query(16,PSEUDO_PROCESS,0,R_OK)==0); // do not advertise virtual allocator support
    assert(query(17,PSEUDO_PROCESS,0,R_OK)==0);
    assert(query(18,PSEUDO_PROCESS,0,R_OK)==profile.program_id);
    assert(query(28,PSEUDO_PROCESS,0,R_OK)==0);
    assert(query(28,0,0,R_INVALID_HANDLE)==0);
    assert(query(28,PSEUDO_THREAD,0,R_INVALID_HANDLE)==0);
    assert(query(28,PSEUDO_PROCESS,1,R_INVALID_COMBINATION)==0);
    assert(query(0x1234,PSEUDO_PROCESS,0,R_INVALID_ENUM)==0);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root=Path(temporary);file=root/'info.c';exe=root/('info.exe' if os.name=='nt' else 'info')
    file.write_text(source,encoding='utf8')
    subprocess.run(shlex.split(os.environ.get('CC','clang'))+['-std=gnu11','-O2','-I'+str(ROOT/'runtime'),str(file),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: host memory information, new alias-extra query and invalid handle/subtype rejection')
