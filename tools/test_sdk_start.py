"""Exercise production SDK entry selection, module lifecycle and initial registers."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
main=(ROOT/'runtime/main.c').read_text(encoding='utf8')
callbacks=main[main.index('static void run_array('):main.index('// Claim the guest address range')]
kernel=(ROOT/'runtime/kernel.c').read_text(encoding='utf8')
launch=kernel[kernel.index('void kernel_run_main('):kernel.index('uint32_t kernel_current_thread_handle(')]
source=r'''
#include <assert.h>
#include "rt.h"
#include "kernel.h"
Module g_modules[TITLE_MAX_MODULES];
static TitleProfile profile={.module_count=3,.init_order={1,2,0}};
static unsigned calls; static uint64_t seen[16]; static unsigned symbols;
const TitleProfile *title_profile_current(void) { return &profile; }
void guest_call(Cpu *c,uint64_t fn) { (void)c;seen[calls++]=fn; }
uint64_t loader_find_symbol(const char *name) {
    if ((symbols&1) && !strcmp(name,"_ZN2nn4init5StartEmmPFvvES2_")) return 0x100;
    if ((symbols&2) && !strcmp(name,"_ZN2nn4init5StartEmmPFvvES2_S2_")) return 0x200;
    return 0;
}
static void thread_start(KThread *t) {
    for (int i=0;i<5;i++) assert(t->cpu.x[i]==(uint64_t)(10+i));
    t->started=true;
}
''' + callbacks + launch + r'''
int main(void) {
    bool fini;
    symbols=0;assert(sdk_start_entry(&fini)==0 && !fini);
    symbols=1;assert(sdk_start_entry(&fini)==0x100 && !fini);
    symbols=2;assert(sdk_start_entry(&fini)==0x200 && fini);
    symbols=3;assert(sdk_start_entry(&fini)==0x100 && !fini);
    Cpu c={0}; uint64_t init[]={21,0,23},done[]={211,0,213};
    g_modules[0]=(Module){.name="main",.init=1,.fini=101};
    g_modules[1]=(Module){.name="sdk",.init=11,.fini=111};
    g_modules[2]=(Module){.name="plugin",.init_array=(uintptr_t)init,.init_array_sz=sizeof init,
                         .fini_array=(uintptr_t)done,.fini_array_sz=sizeof done};
    rtld_init_sdk(&c); rtld_init_main(&c); rtld_fini_modules(&c);
    uint64_t expected[]={11,21,23,1,101,213,211,111};
    assert(calls==sizeof expected/sizeof expected[0] && !memcmp(seen,expected,sizeof expected));
    KThread thread={0};kernel_run_main(&thread,10,11,12,13,14);assert(thread.started);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root=Path(temporary);file=root/'start.c';exe=root/('start.exe' if os.name=='nt' else 'start')
    file.write_text(source,encoding='utf8')
    cc=shlex.split(os.environ.get('CC','clang'))
    subprocess.run(cc+['-std=gnu11','-O2','-I'+str(ROOT/'runtime'),str(file),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: legacy/new SDK entry selection, constructor/destructor order and five initial arguments')
