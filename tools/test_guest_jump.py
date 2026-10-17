"""Run translated synthetic setjmp callers through real host stack unwinding."""
from collections import Counter
from pathlib import Path
from types import SimpleNamespace
import os
import shlex
import struct
import subprocess
import tempfile
import unittest

from gen import Module
from lifter import Lifter

BASE = 0x7008000000
ROOT = Path(__file__).resolve().parent.parent


def branch(pc, target, call=True):
    return (0x94000000 if call else 0x14000000) | (((target-pc)//4) & 0x3ffffff)


def module(words, syms=None, relocs=None):
    m = Module.__new__(Module)
    m.name = 'test'; m.base = m.lo = BASE; m.hi = BASE+len(words)*4
    m.words = tuple(words); m.cache = {}; m.stats = Counter(); m.mu = None
    m.lifter = Lifter(BASE, 'c'); m._function_entries = [BASE]
    m.nso = SimpleNamespace(image=bytearray(struct.pack(f'<{len(words)}I', *words)),
                            syms=syms or [], relocs=relocs or [])
    return m


HARNESS = r'''
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include "guest_jump.h"
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t b, uint64_t d) { (void)a; (void)b; (void)d; }
void rt_fatal_msg(const char *s, ...) { (void)s; abort(); }
void rt_fatal_trace(void) { abort(); }
void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; abort(); }
void rt_brk(Cpu *c, uint32_t n, uint64_t pc) { (void)c; (void)n; (void)pc; abort(); }
void rt_dispatch(Cpu *c) { rt_guest_jump_resume(c); abort(); }
static __thread uint64_t saved_sp, saved_pc, saved_r19;
static __thread int reached_after_jump;
void f_7008000100(Cpu *c) {
    saved_sp=c->sp; saved_pc=c->x[30]; saved_r19=c->x[19]; c->x[0]=0;
}
void f_7008000200(Cpu *c);
static void deep(Cpu *c) {
    RtGuestJumpScope *const inner=rt_guest_jump_enter(c);
    if (setjmp(inner->host)) abort();
    // Same continuation PC in another guest frame must not catch the outer jump.
    rt_guest_jump_mark(inner, 0x2000, saved_sp-16, saved_pc);
    c->sp=saved_sp; c->pc=c->x[30]=saved_pc; c->x[19]=saved_r19; c->x[0]=7;
    f_7008000200(c);
    reached_after_jump++;
    abort();
}
void f_7008000040(Cpu *c) { c->sp-=32; c->x[19]=0; deep(c); reached_after_jump++; }
void f_7008000004(Cpu *c) { if (!c->x[0]) f_7008000040(c); c->x[2]=42; }
void f_7008000000(Cpu *c);
static void *worker(void *unused) {
    (void)unused;
    for (int i=0; i<200; i++) {
        Cpu c={0}; c.sp=0x8000; c.x[0]=0x1000; c.x[19]=0x12345678;
        f_7008000000(&c);
        assert(c.x[0]==7 && c.x[2]==42 && c.x[19]==0x12345678 && c.sp==0x8000);
        assert(!rt_guest_jump_head && !reached_after_jump);
    }
    return NULL;
}
static void reuse_and_reset(void) {
    Cpu c={0}, other={0};
    RtGuestJumpScope *const s=rt_guest_jump_enter(&c);
    if (!setjmp(s->host)) {
        rt_guest_jump_mark(s, 100, 200, 300);
        rt_guest_jump_mark(s, 100, 400, 500);
        c.sp=200; c.pc=300; rt_guest_jump_resume(&c); // replaced buffer no longer matches
        c.sp=400; c.pc=500; rt_guest_jump_resume(&c);
        abort();
    }
    rt_guest_jump_leave(s);
    rt_guest_jump_enter(&c);
    RtGuestJumpScope *const keep=rt_guest_jump_enter(&other);
    rt_guest_jump_reset(&c);
    assert(rt_guest_jump_head==keep && !keep->previous);
    rt_guest_jump_leave(keep);
    assert(!rt_guest_jump_head);
}
int main(void) {
    pthread_t threads[4];
    for (int i=0; i<4; i++) assert(!pthread_create(&threads[i], NULL, worker, NULL));
    for (int i=0; i<4; i++) assert(!pthread_join(threads[i], NULL));
    reuse_and_reset();
    return 0;
}
'''


class GuestJumpTests(unittest.TestCase):
    def run_native(self, tail=False, boundary=False):
        words = [0xd65f03c0] * 256
        words[0] = branch(BASE, BASE+0x100)
        # cbnz w0, +12; call or tail-branch into the error path; trap if it ever returns.
        words[1:6] = [0x35000060, branch(BASE+8, BASE+0x40, not tail),
                      0xd4200000, 0x52800542, 0xd65f03c0]
        m = module(words, [('setjmp', 18, 1, 0x100, 0), ('longjmp', 18, 1, 0x200, 0)])
        end = BASE + (4 if boundary else 24)
        labels, _, tables = m.scan(BASE, end)
        caller = '\n'.join(m.emit_function(BASE, end, labels, tables, {}))
        # A longjmp implementation ending with RET must dispatch its restored guest LR.
        lj = '\n'.join(m.emit_function(BASE+0x200, BASE+0x204, set(), {}, {}))
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp)/'jump.c'; exe = Path(tmp)/('jump.exe' if os.name == 'nt' else 'jump')
            source.write_text(HARNESS+'\n'+caller+'\n'+lj)
            cc = shlex.split(os.environ.get('CC', 'clang'))
            subprocess.run(cc+['-std=gnu11', '-O2', '-pthread', '-I'+str(ROOT/'runtime'),
                              str(source), str(ROOT/'runtime/guest_jump.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=20)

    def test_nested_native_unwind_and_threads(self):
        self.run_native()

    def test_tail_fragment_keeps_checkpoint_alive(self):
        self.run_native(tail=True)

    def test_setjmp_at_fragment_boundary(self):
        self.run_native(boundary=True)

    def test_plt_and_alias_discovery(self):
        # ADRP x16, current page; LDR x17,[x16,#0x180]; ADD x16,x16,#0x180; BR x17
        words = [0x90000010, 0xf940c211, 0x91060210, 0xd61f0220]+[0xd65f03c0]*124
        m = module(words, [('_setjmp', 18, 0, 0, 0)], [(0x180, 1026, 0, 0)])
        self.assertEqual(m.nonlocal_jump_targets()[0], {BASE})
        self.assertEqual(m.nonlocal_jump_targets()[1], set())


if __name__ == '__main__':
    unittest.main()
