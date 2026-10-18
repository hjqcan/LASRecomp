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
#include <stdlib.h>
#include "kernel.h"
#include "kernel_memory.h"
#include "research.h"
static const uint64_t initial_heap=0x12340000;
static TitleProfile profile={.program_id=UINT64_C(0x0100f43008c44000)};
const TitleProfile *title_profile_current(void) { return &profile; }
uint32_t kernel_current_thread_handle(void) { return 1; }
void research_external(int a,uint64_t b,uint32_t c,uint64_t d,uint64_t e) {(void)a;(void)b;(void)c;(void)d;(void)e;}
void plat_commit(uint64_t a,uint64_t b) {(void)a;(void)b;}
bool plat_reserve_guest_extra(uint64_t a,uint64_t b) {(void)a;(void)b;return true;}
bool plat_reset_guest(uint64_t a,uint64_t b) {(void)a;(void)b;return true;}
bool plat_commit_physical(uint64_t a,uint64_t b) {(void)a;(void)b;return true;}
bool plat_discard_physical(uint64_t a,uint64_t b) {(void)a;(void)b;return true;}
void rt_fatal_msg(const char *s,...) {(void)s;abort();}
void rt_fatal_trace(void) {abort();}
''' + constants + '\nstatic void info(uint64_t *x) { uint64_t pc=0; switch(0x29) {\n'+handler+'\n} }\n'+r'''
static uint64_t query(uint64_t id,uint64_t handle,uint64_t sub,uint32_t result) {
    uint64_t x[4]={0,id,handle,sub}; info(x);assert(x[0]==result);return x[1];
}
int main(void) {
    assert(kernel_memory_configure(0) && kernel_set_heap_size(initial_heap)==KM_OK);
    assert(query(2,PSEUDO_PROCESS,0,R_OK)==ALIAS_BASE);
    assert(query(3,PSEUDO_PROCESS,0,R_OK)==ALIAS_SIZE);
    assert(query(4,PSEUDO_PROCESS,0,R_OK)==HEAP_BASE);
    assert(query(5,PSEUDO_PROCESS,0,R_OK)==HEAP_MAX);
    assert(query(7,PSEUDO_PROCESS,0,R_OK)==initial_heap+0x4000000);
    assert(query(16,PSEUDO_PROCESS,0,R_OK)==0); // do not advertise virtual allocator support
    assert(query(17,PSEUDO_PROCESS,0,R_OK)==0);
    assert(query(16,0,0,R_INVALID_HANDLE)==0);
    assert(query(17,PSEUDO_PROCESS,1,R_INVALID_COMBINATION)==0);
    assert(query(18,PSEUDO_PROCESS,0,R_OK)==profile.program_id);
    assert(query(28,PSEUDO_PROCESS,0,R_OK)==0);
    assert(query(28,0,0,R_INVALID_HANDLE)==0);
    assert(query(28,PSEUDO_THREAD,0,R_INVALID_HANDLE)==0);
    assert(query(28,PSEUDO_PROCESS,1,R_INVALID_COMBINATION)==0);
    assert(query(0x1234,PSEUDO_PROCESS,0,R_INVALID_ENUM)==0);
    kernel_memory_reset(); assert(kernel_memory_configure(0x1000000));
    assert(query(2,PSEUDO_PROCESS,0,R_OK)==VIRTUAL_ALIAS_BASE);
    assert(query(3,PSEUDO_PROCESS,0,R_OK)==VIRTUAL_ALIAS_SIZE);
    assert(query(13,PSEUDO_PROCESS,0,R_OK)==VIRTUAL_GUEST_HI-GUEST_LO);
    assert(query(16,PSEUDO_PROCESS,0,R_OK)==0x1000000);
    uint64_t resources=query(17,PSEUDO_PROCESS,0,R_OK),used=query(7,PSEUDO_PROCESS,0,R_OK);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE,0x1000,true)==KM_OK);
    assert(query(17,PSEUDO_PROCESS,0,R_OK)==resources+0x1000);
    assert(query(7,PSEUDO_PROCESS,0,R_OK)==used+0x1000);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root=Path(temporary);file=root/'info.c';exe=root/('info.exe' if os.name=='nt' else 'info')
    file.write_text(source,encoding='utf8')
    subprocess.run(shlex.split(os.environ.get('CC','clang'))+['-std=gnu11','-O2','-pthread','-I'+str(ROOT/'runtime'),str(file),
                   str(ROOT/'runtime/kernel_memory.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: host memory information, new alias-extra query and invalid handle/subtype rejection')
