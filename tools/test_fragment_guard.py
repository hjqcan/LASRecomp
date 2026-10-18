"""A signed relative table whose materialized origin splits it from its guard."""
from collections import Counter
from pathlib import Path
from types import SimpleNamespace
import os
import shlex
import struct
import subprocess
import tempfile
import unittest

import unicorn as uc
from unicorn import arm64_const as A
from gen import Module
from lifter import Lifter
from test_cross_fragment_tables import adr

BASE = 0x7008000000
ROOT = Path(__file__).resolve().parents[1]


def module():
    words = [0xD65F03C0] * 80
    for offset, value in ((0, 10), (0xA0, 20), (0xE0, 30)):
        words[offset // 4] = 0x91000000 | (value << 10)  # add x0,x0,#value
    words[0x70 // 4] = 0xD2800C60  # mov x0,#99 (default)
    words[8:16] = [0x2A0003E9, 0x7100093F, 0x54000248,
                   adr(8, BASE + 0x2C, BASE + 0x180), adr(10, BASE + 0x30, BASE + 0x30),
                   0xB8A9790B, 0x8B0B014A, 0xD61F0140]
    image = bytearray(0x4000)
    struct.pack_into('<80I', image, 0, *words)
    struct.pack_into('<3i', image, 0x180, -0x30, 0x70, 0xB0)
    m = Module.__new__(Module)
    m.name = 'test'; m.base = m.lo = BASE; m.hi = BASE + 0x140
    m.nso = SimpleNamespace(image=image, syms=[], relocs=[])
    m._function_entries = [BASE, BASE + 0x20]
    m.words = tuple(words); m.cache = {}; m.stats = Counter(); m.mu = None
    m.lifter = Lifter(BASE, 'c')
    return m


def masked_module(wide_mask=False, mutate_source=False):
    m = module(); words = [0xD65F03C0] * 80
    for offset, value in ((0, 10), (0xA0, 20), (0xE0, 30), (0x70, 99)):
        words[offset // 4:offset // 4 + 3] = [0xD2800000 | (value << 5), 0xAA1503FE, 0xD65F03C0]
    # Preserve LR, mask a callee-saved source, make a call, and test that mask.
    body = [0xAA1E03F5, 0x12001E96, 0x94000036, 0x71000ADF, 0x54000208]
    if mutate_source: body.append(0xAA0003F4)  # mov x20,x0: no longer the guarded source
    body.append(0x92402288 if wide_mask else 0x92401E88)  # and x8,x20,#0x1ff / #0xff
    at = BASE + 0x20 + 4 * len(body)
    origin = at + 4
    body += [adr(11, at, BASE + 0x180), adr(9, origin, origin), 0xB8A8796A, 0x8B0A0129, 0xD61F0120]
    words[8:8 + len(body)] = body
    words[0x100 // 4:0x110 // 4] = [0xD2800EE0, 0xD2800CC8, 0xD2800AA9, 0xD65F03C0]
    m.words = tuple(words); struct.pack_into('<80I', m.nso.image, 0, *words)
    struct.pack_into('<3i', m.nso.image, 0x180, BASE - origin, BASE + 0xA0 - origin, BASE + 0xE0 - origin)
    m._function_entries.append(BASE + 0x100)
    m.table_origin = origin
    return m


class FragmentGuardTest(unittest.TestCase):
    def test_signed_targets_across_origin_fragment(self):
        m = module()
        targets = [BASE, BASE + 0xA0, BASE + 0xE0]
        self.assertEqual(m.jump_table(BASE + 0x3C, BASE + 0x20, BASE + 0x70), targets)
        self.assertEqual(m.jump_table(BASE + 0x3C, BASE + 0x30, BASE + 0x70), targets)
        entries, _ = m.discover_functions(m.boundaries())
        self.assertIn(BASE + 0x30, entries)  # independently dispatched origin stays intact
        self.assertTrue(set(targets).issubset(entries))
        end, labels, tables = entries[BASE + 0x30]
        emitted = '\n'.join(m.emit_function(BASE + 0x30, end, labels, tables, {}))
        for target in targets:
            self.assertIn(f'return f_{target:x}(c);', emitted)

    def test_guard_cannot_cross_a_real_function_entry(self):
        m = module(); m._function_entries.append(BASE + 0x30)
        self.assertIsNone(m.jump_table(BASE + 0x3C, BASE + 0x30, BASE + 0x70))

    def test_guard_cannot_cross_a_call(self):
        m = module()
        words = list(m.words); words[0x2C // 4] = 0x94000010
        m.words = tuple(words); struct.pack_into('<I', m.nso.image, 0x2C, words[0x2C // 4])
        self.assertIsNone(m.jump_table(BASE + 0x3C, BASE + 0x30, BASE + 0x70))

    def test_overwritten_flags_are_not_a_bound(self):
        m = module()
        words = list(m.words)
        words[8:10] = [0x7100081F, 0x31000009]  # cmp w0,#2; adds w9,w0,#0
        m.words = tuple(words); struct.pack_into('<80I', m.nso.image, 0, *words)
        # ADDS clears carry here, so B.HI cannot reject an out-of-range index.
        self.assertIsNone(m.jump_table(BASE + 0x3C, BASE + 0x30, BASE + 0x70))

    def test_native_cases_and_default_match_arm64(self):
        self.check_native(module(), [0, 1, 2, 3], [10, 21, 32, 99])

    def test_recomputed_mask_after_call_has_all_targets(self):
        m = masked_module()
        self.assertEqual(m.jump_table(m.table_origin + 12, m.table_origin, BASE + 0x70),
                         [BASE, BASE + 0xA0, BASE + 0xE0])

    def test_mask_cannot_inherit_unchecked_bits_or_a_changed_source(self):
        for m in (masked_module(wide_mask=True), masked_module(mutate_source=True)):
            self.assertIsNone(m.jump_table(m.table_origin + 12, m.table_origin, BASE + 0x70))

    def test_mask_identity_is_not_inherited_across_a_join(self):
        m = masked_module()
        # A separate path can skip the mask definition and enter at CMP.
        words = list(m.words); words[0x20 // 4] = 0x14000003
        m.words = tuple(words); struct.pack_into('<I', m.nso.image, 0x20, words[0x20 // 4])
        aliases, recomputed, projected = m._guard_index_values(
            m.table_origin, BASE + 0x2C, m.table_origin + 12, 22)
        self.assertNotIn(20, aliases)
        self.assertFalse(recomputed)
        self.assertFalse(projected)

    def test_recomputed_mask_native_execution_matches_arm64(self):
        self.check_native(masked_module(), [0x100, 0x101, 0x102, 0x103], [10, 20, 30, 99], 20)

    def check_native(self, m, inputs, expected, input_register=0):
        observed = []
        mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        mu.mem_map(BASE, 0x4000); mu.mem_write(BASE, bytes(m.nso.image))
        for value in inputs:
            for r in range(31): mu.reg_write(getattr(A, f'UC_ARM64_REG_X{r}'), 0)
            mu.reg_write(getattr(A, f'UC_ARM64_REG_X{input_register}'), value)
            mu.reg_write(A.UC_ARM64_REG_SP, 0x8000)
            mu.reg_write(A.UC_ARM64_REG_X30, BASE + 0x1000)
            mu.emu_start(BASE + 0x20, BASE + 0x1000, count=100)
            self.assertEqual(mu.reg_read(A.UC_ARM64_REG_PC), BASE + 0x1000)
            self.assertEqual(mu.reg_read(A.UC_ARM64_REG_SP), 0x8000)
            if input_register:
                self.assertEqual(mu.reg_read(getattr(A, f'UC_ARM64_REG_X{input_register}')), value)
            observed.append(mu.reg_read(A.UC_ARM64_REG_X0))
        self.assertEqual(observed, expected)
        entries, _ = m.discover_functions(m.boundaries())
        source = ['#include <assert.h>', '#include <stdlib.h>', '#include "cpu.h"',
                  '#ifdef _WIN32', '#include <windows.h>', '#else', '#include <sys/mman.h>', '#endif',
                  'volatile int g_fnprof;', 'void fnprof_hit(uint64_t a,uint64_t b,uint64_t d) {}',
                  'void rt_udf(Cpu *c,uint64_t pc) { abort(); }', 'void rt_dispatch(Cpu *c) { abort(); }']
        source += [f'void f_{entry:x}(Cpu *c);' for entry in entries]
        for entry, (end, labels, tables) in entries.items():
            source += m.emit_function(entry, end, labels, tables, {})
        source += ['int main(void) {',
                   '#ifdef _WIN32',
                   f'    void *data=VirtualAlloc((void *)0x{BASE:x}ull,0x4000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);',
                   '#else',
                   f'    void *data=mmap((void *)0x{BASE:x}ull,0x4000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);',
                   '#endif', f'    assert(data==(void *)0x{BASE:x}ull);',
                   '    int32_t *table=(int32_t *)((char *)data+0x180);',
                   *[f'    table[{i}]={offset};' for i, offset in enumerate(struct.unpack_from('<3i', m.nso.image, 0x180))],
                   '    const uint64_t expected[]={' + ','.join(map(str, expected)) + '};',
                   '    const uint64_t inputs[]={' + ','.join(map(str, inputs)) + '};',
                   '    for(unsigned index=0;index<4;index++) {',
                   f'        Cpu c={{0}}; c.x[{input_register}]=inputs[index]; c.x[30]=0x1234; c.sp=0x8000;',
                   f'        f_{BASE + 0x20:x}(&c);',
                   '        assert(c.x[0]==expected[index] && c.sp==0x8000 && c.x[30]==0x1234);',
                   *([f'        assert(c.x[{input_register}]==inputs[index]);'] if input_register else []),
                   '    }', '#ifdef _WIN32', '    VirtualFree(data,0,MEM_RELEASE);',
                   '#else', '    munmap(data,0x4000);', '#endif', '    return 0;', '}']
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory); code = path / 'case.c'
            code.write_text('\n'.join(source))
            exe = path / ('case.exe' if os.name == 'nt' else 'case')
            subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
                '-std=gnu11', '-O2', '-I' + str(ROOT / 'runtime'), str(code), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
