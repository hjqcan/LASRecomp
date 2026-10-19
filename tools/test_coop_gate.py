"""Exercise co-op opt-in, isolation/version guards and reboot reset without game assets."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include "hle.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "hle/coop.c"
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
static void env(const char *k,const char *v){_putenv_s(k,v);}
#else
#include <sys/mman.h>
static void env(const char *k,const char *v){setenv(k,v,1);}
#endif
static GameVersion version=GAME_100;
static bool las=true;
bool title_is_las(void){return las;}
static const char *save,*seed;
GameVersion game_version_current(void){return version;}
const char *workshop_save_dir(void){return save;}
const char *rando_seed_dir(void){return seed;}
uint64_t game_main_address(uint64_t a){return a;}
uint64_t kernel_heap_size(void){return 0;}
uint64_t renderer_present_count(void){return 0;}
int game_in_play(void){return 0;}
bool menu_input_captured(void){return false;}
void guest_call(Cpu *c,uint64_t a){assert(!"guest code must not run in boot checks");}
void pets_call(int n){assert(!"companion must not spawn in boot checks");}
static void dummy(Cpu *c){}
GuestFn dispatch_lookup(uint64_t a){return dummy;}
int main(void){
    char why[256];env("LARECOMP_COOP","0");coop_request(false);
    assert(coop_boot(why,sizeof why)&&!coop_active()&&!coop_dispatch_override(IDLE_TICK));
    env("LARECOMP_COOP","1");assert(!coop_boot(why,sizeof why)); // no isolated save
    save="isolated-test-save";version=GAME_110;assert(!coop_boot(why,sizeof why));
    version=GAME_100;las=false;assert(!coop_boot(why,sizeof why));las=true;
    version=GAME_100;seed="seed";assert(!coop_boot(why,sizeof why));seed=NULL;
    // Wrong instructions at a valid mapped address must reject the whole boot.
    void *address=(void *)(uintptr_t)UINT64_C(0x7000900000);
#ifdef _WIN32
    assert(VirtualAlloc(address,0x50000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)==address);
#else
    assert(mmap(address,0x50000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0)==address);
#endif
    assert(!coop_boot(why,sizeof why)&&strstr(why,"guard failed")&&!coop_active());
    coop_host_input(99999,-99999,true);uint64_t value=host_input;
    assert((int16_t)value==32767&&(int16_t)(value>>16)==-32767&&(value>>32)==1);
    enabled=true;companion=123;controlled_steps=42;called=true;coop_reset();
    assert(!coop_active()&&!companion&&!controlled_steps&&!host_input&&!called);
    env("LARECOMP_COOP_INPUT","0:RIGHT:30,20:ATTACK:5");assert(read_script()&&script_count==2);
    clear();env("LARECOMP_COOP_INPUT","0:UNKNOWN:5");assert(!read_script());
    clear();env("LARECOMP_COOP_INPUT","0:RIGHT:999999999");assert(!read_script());
    clear();env("LARECOMP_COOP_INPUT","0:WAIT:1000");assert(read_script()&&scripted&&script_count==1);
#ifdef _WIN32
    VirtualFree(address,0,MEM_RELEASE);
#else
    munmap(address,0x50000);
#endif
    puts("Co-op default-off, isolated-save/version/seed/instruction guards and reboot reset passed");
}
'''
with tempfile.TemporaryDirectory() as temporary:
    folder=Path(temporary)
    source=folder/'gate.c'
    source.write_text(HARNESS)
    exe=folder/('gate.exe' if os.name=='nt' else 'gate')
    subprocess.run(shlex.split(os.environ.get('CC','clang'))+['-std=gnu11','-O1','-I'+str(ROOT/'runtime'),
                   str(source),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
