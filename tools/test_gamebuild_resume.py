"""Exercise the production C build scheduler with tiny generated programs and a real compiler.

Only process launching/resource lookup are bridged to Python. Generation, cancellation,
receipts, repair, linking and publication run through runtime/gamebuild.c and the real cache.
No game assets or downloaded toolchains are needed (CC/CXX default to clang/clang++).
"""
import ctypes as ct
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
from game_catalog import get_game


ROOT = Path(__file__).resolve().parent.parent
BRIDGE = r'''
#include "platform.h"
#include <chrono>
#include <thread>
#include <filesystem>
#include <string>
#include <cstdio>
static std::string root;
static intptr_t (*start_cb)(const char *const *, const char *, bool);
static int (*wait_cb)(intptr_t, bool);
static void (*kill_cb)(intptr_t);
extern "C" void test_init(const char *r, intptr_t (*s)(const char *const *,const char *,bool),
                          int (*w)(intptr_t,bool), void (*k)(intptr_t)) { root=r;start_cb=s;wait_cb=w;kill_cb=k; }
extern "C" bool rt_resource(const char *,const char *dev,char *out,size_t n) {
    if(!dev)return false;
    std::string p=root+"/"+dev;
    return std::filesystem::exists(std::filesystem::u8path(p)) && snprintf(out,n,"%s",p.c_str())<(int)n;
}
extern "C" bool rt_dev_mode() { return false; }
extern "C" uint64_t plat_now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
extern "C" void plat_sleep_until_ns(uint64_t t) { auto now=plat_now_ns(); if(t>now)std::this_thread::sleep_for(std::chrono::nanoseconds(t-now)); }
extern "C" intptr_t plat_spawn(const char *const *a,const char *l,bool append) { return start_cb(a,l,append); }
extern "C" int plat_proc_wait(intptr_t p,bool block) { return wait_cb(p,block); }
extern "C" void plat_proc_kill(intptr_t p) { kill_cb(p); }
'''
GENERATOR = r'''
import pathlib,sys,time
name,source,base,out=sys.argv[1:5]
out=pathlib.Path(out);out.mkdir(parents=True,exist_ok=True)
for i in range(3):
    (out/f'{name}_{i:03}.c').write_text(f'#include "cpu.h"\nvoid {name}_{i:03}(Cpu *c) {{ c->x[0] = {i}; }}\n')
    print(f'{name}: progress {i+1}/3 files {i+1}',file=sys.stderr,flush=True)
    time.sleep(.03)
build_id=pathlib.Path(source).read_bytes()[0x40:0x60].hex()
text='#include "cpu.h"\n'
text+=''.join(f'extern void {name}_{i:03}(Cpu *c);\n' for i in range(3))
text+=f'const uint64_t {name}_fn_addrs[] = {{'+','.join(hex(int(base,0)+i*4) for i in range(3))+'};\n'
text+=f'const GuestFn {name}_fn_ptrs[] = {{'+','.join(f'{name}_{i:03}' for i in range(3))+'};\n'
text+=f'const uint64_t {name}_fn_count = 3;\nconst char {name}_build_id_full[] = "{build_id}";\n'
(out/f'{name}_table.c').write_text(text)
'''

with tempfile.TemporaryDirectory() as temporary:
    tmp = Path(temporary).resolve()
    resources, game = tmp/'resources', tmp/'game'
    (resources/'runtime/hle').mkdir(parents=True)
    (resources/'tools').mkdir()
    (resources/'build').mkdir()
    (game/'exefs').mkdir(parents=True)
    for name in ('cpu.h','simd.h','simd_fp.h','gamelib.h','guest_jump.h'):
        shutil.copy2(ROOT/'runtime'/name, resources/'runtime'/name)
    shutil.copy2(ROOT/'tools/gamelib_entry.c', resources/'tools/gamelib_entry.c')
    (resources/'runtime/hle/hle_symbols.txt').write_text('fixture\n')
    main = bytearray(0x60); main[:4] = b'NSO0'; main[0x40:0x54] = bytes.fromhex('ae16f71e002af8cb059a9a74c4d90f34ba984892')
    (game/'exefs/main').write_bytes(main)
    sdk=bytearray(0x60);sdk[:4]=b'NSO0';sdk[0x40:0x60]=bytes.fromhex(get_game().releases[0].modules[1].build_id)
    (game/'exefs/sdk').write_bytes(sdk)
    generator = tmp/'generator.py'; generator.write_text(GENERATOR)
    cc = shlex.split(os.environ.get('CC','clang'))
    cxx = shlex.split(os.environ.get('CXX','clang++'))
    compiler = shutil.which(cc[0]); assert compiler
    # The native helper receives its normal argv; the process bridge invokes the fixture script.
    os.environ['LARECOMP_GEN'] = str(generator)
    os.environ['LARECOMP_CC'] = compiler
    if os.name == 'nt':
        stub=tmp/'stub.c'; stub.write_text('int fixture_export;\n')
        subprocess.run(cc+[str(stub),'-shared','-Wl,--out-implib,'+str(resources/'build/lasrecomp.dll.a'),
                           '-o',str(tmp/'stub.dll')],check=True)
    objects=[]
    for source in ('runtime/gamebuild.c','runtime/gameversion.c','runtime/title_profile.c','runtime/gamelib.c'):
        obj=tmp/(Path(source).stem+'.o'); objects.append(str(obj))
        subprocess.run(cc+['-std=gnu11','-fPIC','-Iruntime','-DGAMELIB_ID="cache-fixture"',
                           '-DGAMELIB_CFLAGS="-O0 -fPIC"','-DGAMELIB_SHARED="-shared"',
                           '-c',source,'-o',str(obj)],cwd=ROOT,check=True)
    bridge=tmp/'bridge.cpp'; bridge.write_text(BRIDGE)
    libfile=tmp/('scheduler.dll' if os.name=='nt' else 'scheduler.so')
    subprocess.run(cxx+['-shared','-fPIC','-pthread','-std=c++17','-Iruntime','-x','c++',str(bridge),
                        'runtime/ui/build_cache.mm','runtime/ui/game_install.mm','-x','none',*objects,'-o',str(libfile)]+
                   (['-Wl,--export-all-symbols'] if os.name=='nt' else []),cwd=ROOT,check=True)
    dll_search = os.add_dll_directory(str(Path(compiler).parent)) if os.name == 'nt' else None
    library=ct.CDLL(str(libfile))
    SPAWN=ct.CFUNCTYPE(ct.c_ssize_t,ct.POINTER(ct.c_char_p),ct.c_char_p,ct.c_bool)
    WAIT=ct.CFUNCTYPE(ct.c_int,ct.c_ssize_t,ct.c_bool)
    KILL=ct.CFUNCTYPE(None,ct.c_ssize_t)
    PROGRESS=ct.CFUNCTYPE(None,ct.c_double,ct.c_char_p)
    CANCEL=ct.CFUNCTYPE(ct.c_bool)
    class Options(ct.Structure):
        _fields_=[('progress',PROGRESS),('jobs',ct.c_int),('cancelled',CANCEL)]
    processes={}; launched=[]; next_id=0; fail_compile=False; fail_link=False
    @SPAWN
    def spawn(args, log, append):
        global next_id, fail_compile, fail_link
        command=[];i=0
        while args[i]: command.append(args[i].decode()); i+=1
        launched.append(command.copy())
        if command[0]==str(generator): command=[sys.executable,*command]
        elif (fail_compile and '-c' in command) or (fail_link and '-c' not in command):
            fail_compile=fail_link=False
            command=[sys.executable,'-c','import sys;print("synthetic failure");sys.exit(7)']
        stream=open(log.decode(),'ab' if append else 'wb')
        try:
            p=subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0,
                start_new_session=os.name!='nt')
        finally: stream.close()
        next_id+=1; processes[next_id]=p; return next_id
    @WAIT
    def wait(pid,block):
        p=processes[pid]; result=p.wait() if block else p.poll()
        if result is None:return -1
        del processes[pid];return result
    @KILL
    def kill(pid):
        p=processes.pop(pid)
        if p.poll() is None:
            if os.name=='nt': subprocess.run(['taskkill','/PID',str(p.pid),'/T','/F'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            else: os.killpg(p.pid,signal.SIGKILL)
        p.wait()
    @PROGRESS
    def progress(fraction,step): pass
    cancel_enabled=True
    work=game/'.gamebuild'
    @CANCEL
    def cancelled():
        return (cancel_enabled and (work/'main.generation').exists() and (work/'sdk.generation').exists()
                and len(list(work.glob('*.receipt')))>=2)
    options=Options(progress,1,cancelled)
    library.test_init.argtypes=[ct.c_char_p,SPAWN,WAIT,KILL]
    library.test_init(str(resources).encode(),spawn,wait,kill)
    library.gamebuild_run.argtypes=[ct.c_char_p,ct.POINTER(Options),ct.c_void_p,ct.c_size_t]
    library.gamebuild_run.restype=ct.c_bool
    why=ct.create_string_buffer(1024)
    library.gamelib_verify.argtypes=[ct.c_char_p,ct.c_void_p,ct.c_size_t]
    library.gamelib_verify.restype=ct.c_bool
    library.game_install_begin.argtypes=[ct.c_char_p,ct.c_bool,ct.c_void_p,ct.c_size_t]
    library.game_install_begin.restype=ct.c_void_p
    library.game_install_candidate.argtypes=[ct.c_void_p]
    library.game_install_candidate.restype=ct.c_char_p
    library.game_install_clone.argtypes=[ct.c_void_p,ct.c_char_p,ct.c_void_p,ct.c_size_t]
    library.game_install_clone.restype=ct.c_bool
    VALIDATE=ct.CFUNCTYPE(ct.c_bool,ct.c_char_p,ct.c_void_p,ct.c_size_t)
    library.game_install_publish.argtypes=[ct.c_void_p,ct.c_char_p,VALIDATE,ct.c_void_p,ct.c_size_t]
    library.game_install_publish.restype=ct.c_bool
    library.game_install_close.argtypes=[ct.c_void_p]
    library.game_install_resolve.argtypes=[ct.c_char_p,ct.c_void_p,ct.c_size_t,ct.c_void_p,ct.c_size_t]
    library.game_install_resolve.restype=ct.c_bool
    @VALIDATE
    def verify(path, error, cap): return library.gamelib_verify(path,error,cap)
    def run():
        launched.clear();why.value=b''
        return library.gamebuild_run(str(game).encode(),ct.byref(options),why,len(why))
    def compiles(): return [a for a in launched if '-c' in a]
    def generators(): return [a for a in launched if a[0]==str(generator)]
    try:
        assert not run() and b'cancelled' in why.value,why.value
        saved=len(list(work.glob('*.receipt'))); assert 0<saved<9
        cancel_enabled=False
        assert run(),why.value
        assert not generators() and len(compiles())==10-saved
        output=next(p for p in game.glob('lasgame.*') if p.suffix!='.id')
        assert library.gamelib_verify(str(game).encode(),why,len(why)),why.value
        receipt=output.with_name(output.name+'.id')
        original_id=receipt.read_bytes()
        receipt.write_bytes(original_id.replace(get_game().releases[0].main_build_id.encode(),b'00'*32))
        assert not library.gamelib_verify(str(game).encode(),why,len(why))
        receipt.write_bytes(original_id)
        before=hashlib.sha256(output.read_bytes()).digest()
        (work/'main_000.o').write_bytes(b'corrupt')
        assert run(),why.value
        assert len(compiles())==2 and not generators()
        (work/'main/main_000.c').write_text('corrupt source\n')
        assert run(),why.value
        assert len(generators())==1 and len(compiles())==1
        before=hashlib.sha256(output.read_bytes()).digest() # PE link timestamps may differ after a successful relink
        fail_link=True
        assert not run() and b'link' in why.value,why.value
        assert hashlib.sha256(output.read_bytes()).digest()==before
        (work/'sdk_000.o').write_bytes(b'corrupt')
        fail_compile=True
        assert not run(),why.value
        assert hashlib.sha256(output.read_bytes()).digest()==before
        assert run(),why.value
        assert len(compiles())==2 and not generators()
        (game/'exefs/main').write_bytes(main+b'changed input')
        assert run(),why.value
        assert len(generators())==2 and len(compiles())==1
        (resources/'runtime/cpu.h').write_text((resources/'runtime/cpu.h').read_text()+'\n/* changed header */\n')
        assert run(),why.value
        assert len(generators())==2 and len(compiles())==9
        las_output=output
        las_hash=hashlib.sha256(las_output.read_bytes()).digest()
        game=tmp/'acnh'; (game/'exefs').mkdir(parents=True)
        for module in get_game('acnh').releases[0].modules:
            header=bytearray(0x60);header[:4]=b'NSO0';header[0x40:0x60]=bytes.fromhex(module.build_id)
            (game/'exefs'/module.name).write_bytes(header)
        work=game/'.gamebuild'
        assert run(),why.value
        assert len(generators())==3 and len(compiles())==13
        assert all(a[a.index('--profile')+1]=='generic' for a in generators())
        assert (work/'subsdk0_table.o').exists()
        assert run() and len(compiles())==1 and not generators(),why.value
        output=next(p for p in game.glob('lasgame.*') if p.suffix!='.id')
        acnh_hash=hashlib.sha256(output.read_bytes()).digest()
        sdk=bytearray((game/'exefs/sdk').read_bytes());sdk[-1]=1;(game/'exefs/sdk').write_bytes(sdk)
        assert not run() and b'sdk' in why.value and not launched
        assert hashlib.sha256(output.read_bytes()).digest()==acnh_hash
        assert hashlib.sha256(las_output.read_bytes()).digest()==las_hash
        # The real builder, real library validator and real installation publisher share one candidate.
        sdk[-1]=0;(game/'exefs/sdk').write_bytes(sdk)
        slot=game
        transaction=library.game_install_begin(str(slot).encode(),True,why,len(why));assert transaction,why.value
        try:
            assert library.game_install_clone(transaction,str(slot).encode(),why,len(why)),why.value
            game=Path(library.game_install_candidate(transaction).decode());work=game/'.gamebuild'
            fail_link=True
            assert not run()
            resolved=ct.create_string_buffer(4096)
            assert library.game_install_resolve(str(slot).encode(),resolved,len(resolved),why,len(why))
            assert Path(resolved.value.decode())==slot
            assert hashlib.sha256(output.read_bytes()).digest()==acnh_hash
            assert library.gamelib_verify(str(slot).encode(),why,len(why)),why.value
            assert run(),why.value
            assert library.game_install_publish(transaction,b'acnh/1.0.0',verify,why,len(why)),why.value
            assert library.game_install_resolve(str(slot).encode(),resolved,len(resolved),why,len(why))
            assert Path(resolved.value.decode())==game
            assert library.gamelib_verify(str(game).encode(),why,len(why)),why.value
            assert hashlib.sha256(output.read_bytes()).digest()==acnh_hash
        finally:
            library.game_install_close(transaction)
        print('Build scheduler and installation: cancel/resume, input isolation, real library verification, failed rebuild preservation and candidate publication passed')
    finally:
        for pid in list(processes): kill(pid)
        if os.name=='nt':
            import _ctypes
            _ctypes.FreeLibrary(library._handle)
            dll_search.close()
