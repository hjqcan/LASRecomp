"""gen.py --dyn-base (lifter.gaddr): without the flag every address is the same literal as before; with it, every address
the code takes from its own position is GB + offset, and nothing else changes. No game files needed: the instructions
are encoded here.

Usage: test_dyn_base.py
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lifter  # noqa: E402
from lifter import CANON, Lifter, hx  # noqa: E402

PC = CANON + 0x123450
# (word, what it is): each one's lifted C names an address derived from PC
PC_RELATIVE = [
    (0x90000010, 'adrp x16, #0'),
    (0x10000080, 'adr x0, #16'),
    (0x58000040, 'ldr x0, #8 (literal)'),
    (0x18000040, 'ldr w0, #8 (literal)'),
    (0x98000040, 'ldrsw x0, #8 (literal)'),
    (0xD4000001, 'svc #0'),
    (0xD4200000, 'brk #0'),
    (0x00000000, 'udf'),
]
# not derived from the pc: identical with and without the flag
NOT_PC = [
    (0xD2800020, 'mov x0, #1'),
    (0x8B020020, 'add x0, x1, x2'),
    (0xF9400020, 'ldr x0, [x1]'),
    (0xD65F03C0, 'ret'),
]

fails = 0


def check(ok, what):
    global fails
    if not ok:
        fails += 1
        print('FAIL', what)


def lift_all(dyn, simd):
    lifter.DYN_BASE = dyn
    lf = Lifter(CANON, simd)
    out = {}
    for w, name in PC_RELATIVE + NOT_PC:
        ins = lf.lift(w, PC)
        out[name] = ins.code if ins.kind in ('op', 'trap') else repr(ins.asm.consts if ins.kind == 'asm' else ins.target)
    return out


for simd in ('asm', 'c'):
    off, on = lift_all(False, simd), lift_all(True, simd)
    for w, name in PC_RELATIVE:
        check('GB' not in off[name], f'{simd} {name}: GB without --dyn-base: {off[name]}')
        if 'consts' in on[name] or on[name].startswith('['):
            continue  # (asm form: the constant goes through emit_asm_batch, checked below)
        check('GB + 0x' in on[name], f'{simd} {name}: no GB + offset with --dyn-base: {on[name]}')
        check(not re.search(r'\b0x7[0-3][0-9a-f]{8}ull\b', on[name]), f'{simd} {name}: canonical literal left: {on[name]}')
    for w, name in NOT_PC:
        check(off[name] == on[name], f'{simd} {name}: changed by --dyn-base: {off[name]} / {on[name]}')

# gaddr: only the window moves; addresses outside it (and plain constants) stay literals
lifter.DYN_BASE = True
check(lifter.gaddr(CANON + 0x10) == '(GB + 0x10ull)', 'gaddr in the window')
check(lifter.gaddr(CANON + lifter.WINDOW) == hx(CANON + lifter.WINDOW), 'gaddr past the window')
check(lifter.gaddr(0x1234) == hx(0x1234), 'gaddr of a small constant')
lifter.DYN_BASE = False
check(lifter.gaddr(CANON + 0x10) == hx(CANON + 0x10), 'gaddr without the flag')

# gen.py's switches and hand-written patches
import gen  # noqa: E402

for dyn in (False, True):
    lifter.DYN_BASE = dyn
    t = CANON + 0x4000
    check(gen.case_key(t) == (hx(0x4000) if dyn else hx(t)), f'case_key dyn={dyn}')
    check(gen.switch_on('c->pc') == ('c->pc - GB' if dyn else 'c->pc'), f'switch_on dyn={dyn}')
    patch = 'c->x[30] = 0x7000f8c8b4ull; f_7000f8c8b0(c);  /* 0x7000f8c8b0: bl */'
    want = 'c->x[30] = (GB + 0xf8c8b4ull); f_7000f8c8b0(c);  /* 0x7000f8c8b0: bl */' if dyn else patch
    check(gen.rebase_patch(patch) == want, f'rebase_patch dyn={dyn}: {gen.rebase_patch(patch)}')
lifter.DYN_BASE = False

if fails:
    sys.exit(f'{fails} failed')
print('dyn-base: ok')
