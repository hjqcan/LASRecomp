"""Synthetic ARM exclusive pairs: Unicorn differential checks and native contention."""
from pathlib import Path
import os
import random
import shlex
import struct
import subprocess
import tempfile
import unittest

import unicorn
from unicorn import arm64_const as A
from lifter import Lifter, Unsupported

ROOT = Path(__file__).resolve().parents[1]
CODE, MEMORY = 0x100000, 0x200000


def pair(load, width=64, ordered=False, rt=0, rt2=1, rn=2, status=3):
    # ARM LDXP/LDAXP/STXP/STLXP encoding, independent of the decoder's bit reads.
    return (0x88200000 | ((width == 64) << 30) | (load << 22) |
            ((31 if load else status) << 16) | (ordered << 15) | rt2 << 10 | rn << 5 | rt)


def lifted(name, word):
    return f'static void {name}(Cpu *c) {{ {Lifter(CODE, "c").lift(word, CODE).code} }}\n'


PREFIX = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <stdlib.h>
static jmp_buf fault;
static void alignment_fault(void) __attribute__((noreturn));
static void alignment_fault(void) { longjmp(fault, 1); }
// Catch the production helper's explicit alignment trap without a host crash UI.
#define __builtin_trap() alignment_fault()
#include "cpu.h"
#undef __builtin_trap
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
'''

HARNESS = r'''
static void read_only_load(void) {
#if defined(_WIN32)
    uint64_t *p=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(p); p[0]=11; p[1]=12; DWORD old;
    assert(VirtualProtect(p,4096,PAGE_READONLY,&old));
#else
    size_t size=(size_t)sysconf(_SC_PAGESIZE);
    uint64_t *p=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    assert(p!=MAP_FAILED); p[0]=11; p[1]=12;
    assert(!mprotect(p,size,PROT_READ));
#endif
    Cpu c={0}; c.x[2]=(uintptr_t)p;
    load_acq64(&c); assert(c.x[0]==11 && c.x[1]==12);
    load_acq32(&c); assert(c.x[0]==11 && c.x[1]==0);
#if defined(_WIN32)
    assert(VirtualFree(p,0,MEM_RELEASE));
#else
    assert(!munmap(p,size));
#endif
}
static void reservations(void) {
    __attribute__((aligned(16))) uint64_t a[2]={0x100000002ull,0x300000004ull}, b[2]={5,6};
    Cpu c={0}; uint64_t p=(uintptr_t)a;
    uint64_t single=0xfedcba9876543210ull, q=(uintptr_t)&single;
    assert(ldx8(&c,q)==0x10 && stx8(&c,q,0xaa)==0 && single==0xfedcba98765432aaull);
    assert(ldx16(&c,q)==0x32aa && stx16(&c,q,0xbbcc)==0 && single==0xfedcba987654bbccull);
    assert(ldx32(&c,q)==0x7654bbcc && stx32(&c,q,0xddeeff11)==0 && single==0xfedcba98ddeeff11ull);
    assert(ldx64(&c,q)==single && stx64(&c,q,123)==0 && single==123);
    assert(stxp64(&c,p,7,8)==1 && a[0]==0x100000002ull);
    ldxp64(&c,p); assert(stxp64(&c,(uintptr_t)b,7,8)==1);
    assert(stxp64(&c,p,7,8)==1); // even a mismatched store clears the monitor
    ldxp64(&c,p); assert(stx64(&c,p,7)==1 && a[0]==0x100000002ull);
    ldx64(&c,p); assert(stxp64(&c,p,7,8)==1);
    ldxp32(&c,p); assert(stx32(&c,p,7)==1);
    ldxp64(&c,p); a[1]^=1; assert(stxp64(&c,p,7,8)==1 && a[0]==0x100000002ull);
    ldxp64(&c,p); clear(&c); assert(stxp64(&c,p,7,8)==1);
    c.x[0]=p; alias_load(&c); assert(c.x[0]==a[0] && c.x[1]==a[1]);
    c.x[2]=p; zero_load(&c); assert(c.x[1]==a[1] && c.x[31]==0);
    zero_store(&c); assert(c.x[3]==0 && a[0]==0 && a[1]==0);
    c.sp=p; sp_load(&c); c.x[0]=7; c.x[1]=8; sp_store(&c);
    assert(c.x[3]==0 && a[0]==7 && a[1]==8 && c.sp==p);
    if (!setjmp(fault)) { ldxp64(&c,p+8); abort(); }
    if (!setjmp(fault)) { ldxp32(&c,p+4); abort(); }
    // A fresh load must replace all state after a fault/failed store.
    ldxp64(&c,p); assert(stxp64(&c,p,9,10)==0);
    ldx8(&c,p); assert(stx16(&c,p,11)==1 && a[0]==9);
    ldx64(&c,p); assert(stx64(&c,(uintptr_t)b,11)==1);
    assert(stx64(&c,p,11)==1 && a[0]==9);
}

static __attribute__((aligned(16))) uint64_t shared[2];
static atomic_int finished;
static int pair_width;
enum { THREADS=4, ITERATIONS=25000 };
static void *increment(void *unused) {
    (void)unused; Cpu c={0}; c.x[2]=(uintptr_t)shared;
    for (int i=0;i<ITERATIONS;i++) {
        do {
            if (pair_width==64) load_acq64(&c); else load_acq32(&c);
            uint64_t lo=c.x[0], hi=c.x[1];
            c.x[0]=lo+1; c.x[1]=~(lo+1);
            if (pair_width==64) store_rel64(&c); else store_rel32(&c);
            if (!c.x[3]) assert(hi==(pair_width==64 ? ~lo : (uint32_t)~lo));
        } while (c.x[3]);
    }
    atomic_fetch_add(&finished,1); return NULL;
}
static void contention(int width) {
    pair_width=width; finished=0;
    shared[0]=width==64 ? 0 : 0xffffffff00000000ull;
    shared[1]=~0ull;
    pthread_t t[THREADS];
    for (int i=0;i<THREADS;i++) assert(!pthread_create(&t[i],NULL,increment,NULL));
    while (atomic_load(&finished)!=THREADS) {
        // An atomic observer must never see a partly updated pair. A zero
        // expected value cannot match the counter/complement invariant.
        if (width==64) {
            __uint128_t v=excl_cas128((uintptr_t)shared,0,0);
            assert((uint64_t)(v>>64)==~(uint64_t)v);
        } else {
            uint64_t v=__atomic_load_n(shared,__ATOMIC_ACQUIRE);
            assert((uint32_t)(v>>32)==(uint32_t)~v);
        }
    }
    for (int i=0;i<THREADS;i++) assert(!pthread_join(t[i],NULL));
    if (width==64) assert(shared[0]==THREADS*ITERATIONS && shared[1]==~shared[0]);
    else assert((uint32_t)shared[0]==THREADS*ITERATIONS && (uint32_t)(shared[0]>>32)==(uint32_t)~shared[0]);
}

static __attribute__((aligned(16))) uint64_t signal_pair[2];
static uint64_t payload;
static void *producer(void *unused) {
    (void)unused; Cpu c={0}; c.x[2]=(uintptr_t)signal_pair;
    for (uint64_t i=1;i<=10000;i++) {
        do { load_acq64(&c); } while (c.x[0]!=0);
        payload=i;
        do {
            load_acq64(&c); assert(c.x[0]==0);
            c.x[0]=1; c.x[1]=i; store_rel64(&c);
        } while(c.x[3]);
    }
    return NULL;
}
static void ordering(void) {
    pthread_t t; Cpu c={0}; c.x[2]=(uintptr_t)signal_pair;
    assert(!pthread_create(&t,NULL,producer,NULL));
    for (uint64_t i=1;i<=10000;i++) {
        // The flag (low half) is the acquire/release handoff. A non-atomic
        // payload is safe only when the translated ordering is preserved.
        do { load_acq64(&c); } while (c.x[0]!=1);
        assert(payload==i && c.x[1]==i);
        do {
            load_acq64(&c); assert(c.x[0]==1);
            c.x[0]=0; c.x[1]=i; store_rel64(&c);
        } while(c.x[3]);
    }
    assert(!pthread_join(t,NULL));
}
'''


class ExclusivePairs(unittest.TestCase):
    def compile_run(self, source):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            file, exe = root/'pairs.c', root/('pairs.exe' if os.name == 'nt' else 'pairs')
            file.write_text(source, encoding='utf-8')
            cc = shlex.split(os.environ.get('CC', 'clang'))
            subprocess.run(cc + ['-std=gnu11', '-O2', '-pthread', '-I'+str(ROOT/'runtime'),
                                 str(file), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=60)

    def test_native_reservations_contention_and_ordering(self):
        functions = ''.join(lifted(f'{name}{width}', pair(load,width,ordered))
                            for width in (32,64) for name,load,ordered in
                            [('load_acq',True,True),('store_rel',False,True)])
        functions += lifted('clear',0xd5033f5f)
        functions += lifted('alias_load',pair(True,rn=0))
        functions += lifted('zero_load',pair(True,rt=31))
        functions += lifted('zero_store',pair(False,rt=31,rt2=31))
        functions += lifted('sp_load',pair(True,rn=31))
        functions += lifted('sp_store',pair(False,rn=31))
        self.compile_run(PREFIX+functions+HARNESS+
                         '\nint main(void) { read_only_load(); reservations(); contention(32); contention(64); ordering(); return 0; }')

    def test_pair_sequences_match_unicorn(self):
        rng = random.Random(20261017)
        emulator = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        emulator.mem_map(CODE,0x1000); emulator.mem_map(MEMORY,0x1000)
        definitions, cases = [], []
        for width in (32,64):
            for ordered in (False,True):
                load, store = pair(True,width,ordered), pair(False,width,ordered)
                suffix=f'{width}_{int(ordered)}'
                definitions += [lifted('load'+suffix,load),lifted('store'+suffix,store)]
                emulator.mem_write(CODE,struct.pack('<II',load,store))
                # Force Unicorn to discard translations when testing another encoding.
                emulator.ctl_remove_cache(CODE,CODE+8)
                for _ in range(32):
                    before=[rng.getrandbits(64) for _ in range(2)]
                    after=[rng.getrandbits(64) for _ in range(2)]
                    emulator.mem_write(MEMORY,struct.pack('<QQ',*before))
                    emulator.reg_write(A.UC_ARM64_REG_X2,MEMORY)
                    emulator.reg_write(A.UC_ARM64_REG_X3,~0 & ((1<<64)-1))
                    emulator.emu_start(CODE,CODE+4,count=1)
                    loaded=[emulator.reg_read(reg) for reg in (A.UC_ARM64_REG_X0,A.UC_ARM64_REG_X1)]
                    for reg,value in zip((A.UC_ARM64_REG_X0,A.UC_ARM64_REG_X1),after): emulator.reg_write(reg,value)
                    emulator.emu_start(CODE+4,CODE+8,count=1)
                    status=emulator.reg_read(A.UC_ARM64_REG_X3)
                    final=struct.unpack('<QQ',emulator.mem_read(MEMORY,16))
                    h=lambda value:f'0x{value:x}ull'
                    cases.append(f'{{ Cpu c={{0}}; __attribute__((aligned(16))) uint64_t m[2]={{ {h(before[0])},{h(before[1])} }}; '
                                 f'c.x[2]=(uintptr_t)m; load{suffix}(&c); '
                                 f'assert(c.x[0]=={h(loaded[0])} && c.x[1]=={h(loaded[1])}); '
                                 f'c.x[0]={h(after[0])}; c.x[1]={h(after[1])}; c.x[3]=~0ull; store{suffix}(&c); '
                                 f'assert(c.x[3]=={h(status)} && m[0]=={h(final[0])} && m[1]=={h(final[1])}); }}')
        self.compile_run(PREFIX+''.join(definitions)+'\nint main(void) {\n'+'\n'.join(cases)+'\nreturn 0; }')

    def test_invalid_or_different_atomic_encodings_remain_traps(self):
        lifter = Lifter(CODE,'c')
        invalid = [pair(True,rt2=0),pair(False,status=0),pair(False,status=1),pair(False,status=2),
                   pair(True)&~(1<<31), pair(False)|(1<<23), pair(True)&~(1<<16)]
        for word in invalid:
            with self.subTest(word=hex(word)),self.assertRaises(Unsupported): lifter.lift(word,CODE)


if __name__ == '__main__': unittest.main()
