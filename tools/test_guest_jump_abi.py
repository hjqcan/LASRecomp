"""Windows: Zig-built guest checkpoint resumes through a clang-built host runtime.

Usage: python tools/test_guest_jump_abi.py <packaged zig directory>
Uses the release's prebuilt runtime libraries and slim headers, with no game assets.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

if os.name != 'nt':
    raise SystemExit('This interop fixture targets the Windows release toolchain')
root = Path(__file__).resolve().parent.parent
zig = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as td:
    p = Path(td)
    (p/'host.c').write_text(r'''
#include <windows.h>
#include <assert.h>
#include <stdlib.h>
#include "guest_jump.h"
void rt_fatal_msg(const char *f, ...) { (void)f; abort(); }
void rt_fatal_trace(void) { abort(); }
int main(int argc, char **argv) {
    assert(argc == 2);
    HMODULE m=LoadLibraryA(argv[1]); assert(m);
    int (*run)(void)=(void *)GetProcAddress(m,"jump_test"); assert(run);
    for (int i=0; i<1000; i++) { assert(run()==42); assert(!rt_guest_jump_head); }
    assert(FreeLibrary(m)); return 0;
}
''')
    (p/'guest.c').write_text(r'''
#include "guest_jump.h"
__declspec(dllexport) int jump_test(void) {
    static Cpu c; // guest CPU storage outlives the function containing setjmp
    RtGuestJumpScope *const s=rt_guest_jump_enter(&c);
    if (!setjmp(s->host)) {
        rt_guest_jump_mark(s, 100, 200, 300);
        c.sp=200; c.pc=300; c.x[0]=42;
        rt_guest_jump_resume(&c);
        return 0;
    }
    rt_guest_jump_leave(s);
    return (int)c.x[0];
}
''')
    cc = shlex.split(os.environ.get('CC', 'clang'))
    subprocess.run(cc+['-std=gnu11', '-O2', '-I'+str(root/'runtime'), str(p/'host.c'),
                      str(root/'runtime/guest_jump.c'), '-Wl,--export-all-symbols',
                      '-Wl,--out-implib,'+str(p/'host.a'), '-o', str(p/'host.exe')], check=True)
    subprocess.run([str(zig/'zig.exe'), 'cc', '-O1', '-mcpu=x86_64_v3', '-I'+str(root/'runtime'),
                    '-c', str(p/'guest.c'), '-o', str(p/'guest.o')], check=True)
    subprocess.run([str(zig/'zig.exe'), 'lld-link', '@'+str(zig/'rt/link.rsp'),
                    '-LIBPATH:'+str(zig/'rt'), '-OUT:'+str(p/'guest.dll'),
                    str(p/'guest.o'), str(p/'host.a')], check=True)
    subprocess.run([str(p/'host.exe'), str(p/'guest.dll')], check=True, timeout=20)
print('PASS: 1000 nonlocal returns from the packaged Zig guest DLL into the native Windows host')
