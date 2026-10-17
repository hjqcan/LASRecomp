"""Exercise real host reserve/extend/commit/discard/reset functions without a GPU."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
platform = 'windows' if os.name == 'nt' else 'macos' if sys.platform == 'darwin' else 'linux'
source = (ROOT / 'runtime/platform' / (platform + '.c')).read_text(encoding='utf8')

def function(name):
    start = re.search(r'bool ' + name + r'\(', source).start()
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
'''
if platform == 'windows':
    preamble += r'''
#include <windows.h>
static uint64_t g_lo, g_hi;
static bool g_write_watch;
static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) { (void)ep;return EXCEPTION_CONTINUE_SEARCH; }
'''
else:
    preamble += r'''
#include <unistd.h>
#include <sys/mman.h>
static uint64_t g_guest_lo, g_guest_hi;
'''
    if platform == 'macos':
        preamble += '#include <mach/mach.h>\n#include <mach/mach_vm.h>\n'
    else:
        # The production memory operations run unchanged; write-tracking setup
        # is independent and requires kernel facilities unavailable in many CI jobs.
        preamble += r'''
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
static int g_uffd = -1;
static uint64_t g_wt_hi;
static void wt_init(uint64_t a,uint64_t b) { (void)a;(void)b; }
static bool wt_register(uint64_t a,uint64_t b) { (void)a;(void)b;return true; }
static void wt_off(const char *s) { (void)s; }
'''
harness = preamble + ''.join(function(n) for n in (
    'plat_reserve_guest', 'plat_reserve_guest_extra', 'plat_commit_physical',
    'plat_discard_physical', 'plat_reset_guest')) + r'''
int main(void) {
    const uint64_t lo = UINT64_C(0x7000000000), hi = lo + 0x200000, ext = hi + 0x2000000;
    assert(plat_reserve_guest(lo,hi));
    assert(!plat_reserve_guest_extra(lo,ext));
    assert(plat_reserve_guest_extra(hi,ext));
    assert(!plat_reserve_guest_extra(hi,ext+0x10000));
    assert(!plat_commit_physical(ext,0x1000) && !plat_commit_physical(ext-0x1000,0x2000));
    assert(!plat_discard_physical(lo-0x1000,0x1000));
    const uint64_t a = hi + 0x10000;
    unsigned char *data = (unsigned char *)(uintptr_t)a;
    assert(plat_commit_physical(a,0x10000));
    for (unsigned i=0;i<0x10000;i++) assert(data[i]==0);
    memset(data,0xa5,0x10000);
    assert(plat_discard_physical(a+0x1000,0x1000));
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info;
    assert(VirtualQuery(data+0x1000,&info,sizeof info) && info.State==MEM_RESERVE);
    assert(VirtualQuery(data,&info,sizeof info) && info.State==MEM_COMMIT);
#endif
    assert(plat_commit_physical(a+0x1000,0x1000));
    for (unsigned i=0;i<0x10000;i++) assert(data[i]==(i>=0x1000 && i<0x2000 ? 0 : 0xa5));
    assert(plat_discard_physical(a+0x3000,0x6000));
    assert(plat_commit_physical(a+0x3000,0x6000));
    for (unsigned i=0;i<0x10000;i++) assert(data[i]==((i>=0x1000 && i<0x2000)||(i>=0x3000 && i<0x9000) ? 0 : 0xa5));
    assert(plat_commit_physical(lo+0x10000,0x1000));
    *(uint64_t *)(uintptr_t)(lo+0x10000)=123;
    assert(plat_reset_guest(hi,ext));
    assert(*(uint64_t *)(uintptr_t)(lo+0x10000)==123);
    assert(plat_commit_physical(a,0x10000));
    for (unsigned i=0;i<0x10000;i++) assert(data[i]==0);
    assert(plat_reset_guest(lo,hi));
    assert(plat_commit_physical(lo+0x10000,0x1000));
    assert(*(uint64_t *)(uintptr_t)(lo+0x10000)==0);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary); path = root / 'memory.c'; exe = root / ('memory.exe' if os.name == 'nt' else 'memory')
    path.write_text(harness, encoding='utf8')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + ['-std=gnu11', '-O2', str(path), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=20)
print('PASS: real host extension, page backing, neighboring-page preservation, zero reuse and separate reservation reset')
