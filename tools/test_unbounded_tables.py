"""Keep local relative-table arithmetic intact without losing independent entries."""
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
from gen import Module, R_AARCH64_RELATIVE
from lifter import Lifter
from test_cross_fragment_tables import adr

BASE = 0x7008000000
ROOT = Path(__file__).resolve().parents[1]


def branch(pc, target, call=False):
    return (0x94000000 if call else 0x14000000) | (((target - pc) // 4) & 0x3FFFFFF)


def module(byte_table=False):
    words = [0xD65F03C0] * 64
    words[:5] = [0xD503201F, adr(10, BASE + 4, BASE + 0x30),
                 0x3868692B if byte_table else 0x7868792B, 0x8B0B094A, 0xD61F0140]
    for index in range(3):
        pc = BASE + 0x30 + index * 8
        words[(pc - BASE) // 4] = 0x91000000 | ((index + 1) << 10)  # add x0,x0,#N
        words[(pc - BASE) // 4 + 1] = branch(pc + 4, BASE + 0x70)
    image = bytearray(0x4000)
    struct.pack_into('<64I', image, 0, *words)
    struct.pack_into('<3B' if byte_table else '<3H', image, 0x200, 0, 2, 4)
    m = Module.__new__(Module)
    m.name = 'test'; m.base = m.lo = BASE; m.hi = BASE + 0x100
    m.nso = SimpleNamespace(image=image, syms=[], relocs=[])
    m._function_entries = [BASE, BASE + 0x80]
    m.words = tuple(words); m.cache = {}; m.stats = Counter(); m.mu = None
    m.lifter = Lifter(BASE, 'c')
    return m


def replace_word(m, offset, word):
    words = list(m.words); words[offset // 4] = word; m.words = tuple(words)
    struct.pack_into('<I', m.nso.image, offset, word)
    m.cache.clear()


def mixed_module(byte_table=False):
    """A bounded table's distant case cuts across a second, unresolved table."""
    m = module(byte_table)
    m._function_entries = [BASE, BASE + 0xE0]
    code = [0x7100091F, 0x54000668, adr(9, BASE + 8, BASE + 0x200),
            adr(10, BASE + 12, BASE + 0x30), 0x7868792B, 0x8B0B094A, 0xD61F0140]
    for i, word in enumerate(code):
        replace_word(m, i * 4, word)
    struct.pack_into('<3H', m.nso.image, 0x200, 0, 2, 24)
    for offset, value, destination in ((0x30, 1, 0x70), (0x38, 2, 0x70), (0x90, 3, 0xD0),
                                        (0xA0, 10, 0xD0), (0xA8, 20, 0xD0), (0xB0, 30, 0xD0)):
        replace_word(m, offset, 0x91000000 | (value << 10))
        replace_word(m, offset + 4, branch(BASE + offset + 4, BASE + destination))
    second = [adr(9, BASE + 0x70, BASE + 0x220), adr(10, BASE + 0x74, BASE + 0xA0),
              0x386C692B if byte_table else 0x786C792B, 0x8B0B094A, 0xD61F0140]
    for i, word in enumerate(second):
        replace_word(m, 0x70 + i * 4, word)
    struct.pack_into('<3B' if byte_table else '<3H', m.nso.image, 0x220, 0, 2, 4)
    return m


class UnboundedTableTests(unittest.TestCase):
    def test_mixed_tables_keep_all_cases_in_the_real_function(self):
        for byte_table in (False, True):
            m = mixed_module(byte_table)
            self.assertEqual(m.jump_table(BASE + 24, BASE, BASE + 0xE0),
                             [BASE + 0x30, BASE + 0x38, BASE + 0x90])
            self.assertIsNone(m.jump_table(BASE + 0x80, BASE, BASE + 0xE0))
            entries, _ = m.discover_functions(m.boundaries())
            self.assertEqual(set(entries), {BASE, BASE + 0xE0})
            end, labels, tables = entries[BASE]
            self.assertEqual(tables[BASE + 0x80], 'all')
            code = '\n'.join(m.emit_function(BASE, end, labels, tables, {}))
            for offset in (0xA0, 0xA8, 0xB0):
                self.assertIn(f'case 0x{BASE + offset:x}ull: goto L_{BASE + offset:x};', code)

    def test_mixed_origin_with_an_independent_reference_still_splits(self):
        for kind in ('callback', 'adrp', 'call', 'relocation', 'symbol'):
            with self.subTest(kind=kind):
                m = mixed_module()
                if kind == 'callback':
                    replace_word(m, 0xE0, adr(0, BASE + 0xE0, BASE + 0x30))
                elif kind == 'adrp':
                    replace_word(m, 0xE0, 0x90000000)  # adrp x0,current page
                    replace_word(m, 0xE4, 0x9100C000)  # add x0,x0,#0x30
                elif kind == 'call':
                    replace_word(m, 0xE0, branch(BASE + 0xE0, BASE + 0x30, call=True))
                elif kind == 'relocation':
                    m.nso.relocs = [(0x208, R_AARCH64_RELATIVE, 0, 0x30)]
                else:
                    m._function_entries.insert(1, BASE + 0x30)
                entries, _ = m.discover_functions(m.boundaries())
                self.assertIn(BASE + 0x30, entries)

    def test_unresolved_table_does_not_change_a_separate_bounded_function(self):
        m = mixed_module(); m._function_entries = [BASE, BASE + 0x70, BASE + 0xE0]
        # Put all first-table cases in the first real function.
        struct.pack_into('<3H', m.nso.image, 0x200, 0, 2, 4)
        targets = m.code_address_targets()
        self.assertIn(BASE + 0x30, targets)
        self.assertNotIn(BASE + 0xA0, targets)

    def test_disjoint_tables_in_the_same_function_keep_bounded_partition(self):
        m = mixed_module()
        struct.pack_into('<3H', m.nso.image, 0x200, 0, 2, 4)
        targets = m.code_address_targets()
        self.assertIn(BASE + 0x30, targets)
        self.assertNotIn(BASE + 0xA0, targets)

    def test_mixed_native_cases_and_default_match_arm64(self):
        for byte_table in (False, True):
            m = mixed_module(byte_table)
            expected = []
            mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
            mu.mem_map(BASE, 0x4000); mu.mem_write(BASE, bytes(m.nso.image))
            for first in range(4):
                for second in range(3):
                    for r in range(31): mu.reg_write(getattr(A, f'UC_ARM64_REG_X{r}'), 0)
                    mu.reg_write(A.UC_ARM64_REG_X0, 10)
                    mu.reg_write(A.UC_ARM64_REG_X8, first)
                    mu.reg_write(A.UC_ARM64_REG_X12, second)
                    mu.reg_write(A.UC_ARM64_REG_SP, 0x8000)
                    mu.reg_write(A.UC_ARM64_REG_X30, BASE + 0x1000)
                    mu.emu_start(BASE, BASE + 0x1000, count=100)
                    self.assertEqual(mu.reg_read(A.UC_ARM64_REG_PC), BASE + 0x1000)
                    self.assertEqual(mu.reg_read(A.UC_ARM64_REG_SP), 0x8000)
                    expected.append(mu.reg_read(A.UC_ARM64_REG_X0))
            self.assertEqual(expected, [21, 31, 41, 22, 32, 42, 13, 13, 13, 10, 10, 10])
            entries, _ = m.discover_functions(m.boundaries())
            source = ['#include <assert.h>', '#include <stdlib.h>', '#include "cpu.h"',
                      '#ifdef _WIN32', '#include <windows.h>', '#else', '#include <sys/mman.h>', '#endif',
                      'volatile int g_fnprof;', 'void fnprof_hit(uint64_t a,uint64_t b,uint64_t d) {}',
                      'void rt_udf(Cpu *c,uint64_t pc) { abort(); }', 'void rt_dispatch(Cpu *c) { abort(); }']
            source += [f'void f_{entry:x}(Cpu *c);' for entry in entries]
            for entry, (end, labels, tables) in entries.items():
                source += m.emit_function(entry, end, labels, tables, {})
            source += ['int main(void) {', '#ifdef _WIN32',
                       f'    void *data=VirtualAlloc((void *)0x{BASE:x}ull,0x4000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);',
                       '#else',
                       f'    void *data=mmap((void *)0x{BASE:x}ull,0x4000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);',
                       '#endif', f'    assert(data==(void *)0x{BASE:x}ull);',
                       '    uint16_t *first=(uint16_t *)((char *)data+0x200); first[0]=0; first[1]=2; first[2]=24;',
                       f'    uint{8 if byte_table else 16}_t *second=(void *)((char *)data+0x220);',
                       '    second[0]=0; second[1]=2; second[2]=4;',
                       '    const uint64_t expected[]={' + ','.join(map(str, expected)) + '};',
                       '    for(unsigned i=0;i<4;i++) for(unsigned j=0;j<3;j++) {',
                       '        Cpu c={0}; c.x[0]=10; c.x[8]=i; c.x[12]=j; c.x[30]=0x1234; c.sp=0x8000;',
                       f'        f_{BASE:x}(&c);',
                       '        assert(c.x[0]==expected[i*3+j] && c.sp==0x8000 && c.x[30]==0x1234);',
                       '    }', '#ifdef _WIN32', '    VirtualFree(data,0,MEM_RELEASE);', '#else',
                       '    munmap(data,0x4000);', '#endif', '    return 0;', '}']
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory); file = path / 'case.c'
                file.write_text('\n'.join(source))
                exe = path / ('case.exe' if os.name == 'nt' else 'case')
                subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
                    '-std=gnu11', '-O2', '-I' + str(ROOT / 'runtime'), str(file), '-o', str(exe)], check=True)
                subprocess.run([str(exe)], check=True)

    def test_unbounded_cases_remain_reachable(self):
        for byte_table in (False, True):
            m = module(byte_table)
            self.assertIsNone(m.jump_table(BASE + 16, BASE, BASE + 0x30))
            self.assertTrue(m.relative_table_base(BASE + 4, BASE + 0x30))
            entries, _ = m.discover_functions(m.boundaries())
            self.assertNotIn(BASE + 0x30, entries)
            end, labels, tables = entries[BASE]
            self.assertEqual(tables[BASE + 16], 'all')
            code = '\n'.join(m.emit_function(BASE, end, labels, tables, {}))
            for offset in (0x30, 0x38, 0x40):
                self.assertIn(f'case 0x{BASE + offset:x}ull: goto L_{BASE + offset:x};', code)

    def test_independent_callback_call_relocation_and_symbol_still_split(self):
        for kind in ('callback', 'adrp', 'call', 'relocation', 'symbol'):
            with self.subTest(kind=kind):
                m = module()
                if kind == 'callback':
                    replace_word(m, 0x80, adr(0, BASE + 0x80, BASE + 0x30))
                elif kind == 'adrp':
                    replace_word(m, 0x80, 0x90000000)
                    replace_word(m, 0x84, 0x9100C000)
                elif kind == 'call':
                    replace_word(m, 0x80, branch(BASE + 0x80, BASE + 0x30, call=True))
                elif kind == 'relocation':
                    m.nso.relocs = [(0x208, R_AARCH64_RELATIVE, 0, 0x30)]
                else:
                    m._function_entries.insert(1, BASE + 0x30)
                entries, _ = m.discover_functions(m.boundaries())
                self.assertIn(BASE + 0x30, entries)

    def test_ambiguous_register_uses_are_not_filtered(self):
        for offset, word in [(8, 0x7868794B), (8, 0x7868792A), (12, 0x8B0B096A),
                             (16, 0xD61F0160), (8, 0xF900002A), (12, 0x8B0B094C)]:
            with self.subTest(word=hex(word)):
                m = module(); replace_word(m, offset, word)
                self.assertFalse(m.relative_table_base(BASE + 4, BASE + 0x30))
                self.assertIn(BASE + 0x30, m.code_address_targets())

    def test_other_function_address_is_not_filtered(self):
        m = module(); replace_word(m, 4, adr(10, BASE + 4, BASE + 0x90))
        self.assertFalse(m.relative_table_base(BASE + 4, BASE + 0x90))
        self.assertIn(BASE + 0x90, m.code_address_targets())

    def test_scheduled_return_clear_preserves_branch_register(self):
        for clear in (0xAA1F03E0, 0x2A1F03E0):  # mov x0/w0,zr
            m = module()
            replace_word(m, 16, clear); replace_word(m, 20, 0xD61F0140)
            self.assertTrue(m.relative_table_base(BASE + 4, BASE + 0x30))
            entries, _ = m.discover_functions(m.boundaries())
            self.assertNotIn(BASE + 0x30, entries)
            self.assertEqual(entries[BASE][2][BASE + 20], 'all')
        for operation in (0xAA1F03EA, 0x2A1F03EA, 0xF900002A):  # clear target / store target
            m = module()
            replace_word(m, 16, operation); replace_word(m, 20, 0xD61F0140)
            self.assertFalse(m.relative_table_base(BASE + 4, BASE + 0x30))

    def test_bounded_table_keeps_existing_partition(self):
        m = module()
        # cmp w8,#2; b.hi default; adr x9,table; adr x10,base; ldrh; add; br.
        code = [0x7100091F, 0x54000368, adr(9, BASE + 8, BASE + 0x200),
                adr(10, BASE + 12, BASE + 0x30), 0x7868792B, 0x8B0B094A, 0xD61F0140]
        for i, word in enumerate(code):
            replace_word(m, i * 4, word)
        self.assertEqual(m.jump_table(BASE + 24, BASE, BASE + 0x80),
                         [BASE + 0x30, BASE + 0x38, BASE + 0x40])
        self.assertFalse(m.relative_table_base(BASE + 12, BASE + 0x30))
        self.assertIn(BASE + 0x30, m.code_address_targets())

    def test_generated_native_execution_matches_arm64(self):
        for byte_table, clear in ((False, None), (True, None), (False, 0xAA1F03E0),
                                  (True, 0xAA1F03E0), (False, 0x2A1F03E0), (True, 0x2A1F03E0)):
            m = module(byte_table)
            if clear is not None:
                replace_word(m, 16, clear); replace_word(m, 20, 0xD61F0140)
            expected = []
            mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
            mu.mem_map(BASE, 0x4000); mu.mem_write(BASE, bytes(m.nso.image))
            for index in range(3):
                mu.reg_write(A.UC_ARM64_REG_X0, 10)
                mu.reg_write(A.UC_ARM64_REG_X8, index)
                mu.reg_write(A.UC_ARM64_REG_X9, BASE + 0x200)
                mu.reg_write(A.UC_ARM64_REG_X30, BASE + 0x1000)
                mu.emu_start(BASE, BASE + 0x1000, count=100)
                self.assertEqual(mu.reg_read(A.UC_ARM64_REG_PC), BASE + 0x1000)
                expected.append(mu.reg_read(A.UC_ARM64_REG_X0))
            self.assertEqual(expected, [11, 12, 13] if clear is None else [1, 2, 3])
            entries, _ = m.discover_functions(m.boundaries())
            source = ['#include <assert.h>', '#include <stdlib.h>', '#include "cpu.h"',
                      'volatile int g_fnprof;', 'void fnprof_hit(uint64_t a,uint64_t b,uint64_t d) {}',
                      'void rt_udf(Cpu *c,uint64_t pc) { abort(); }',
                      'void rt_dispatch(Cpu *c) { abort(); }']
            source += [f'void f_{entry:x}(Cpu *c);' for entry in entries]
            for entry, (end, labels, tables) in entries.items():
                source += m.emit_function(entry, end, labels, tables, {})
            source += ['int main(void) {',
                       f'    uint{8 if byte_table else 16}_t offsets[]={{0,2,4}};',
                       '    for (unsigned index=0;index<3;index++) {',
                       '        Cpu c={0}; c.x[0]=10; c.x[8]=index; c.x[9]=(uintptr_t)offsets;',
                       '        c.x[30]=0x1234; c.sp=0x8000;', f'        f_{BASE:x}(&c);',
                       f'        assert(c.x[0]=={11 if clear is None else 1}+index && c.sp==0x8000 && c.x[30]==0x1234);',
                       '    }', '    return 0;', '}']
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory); file = path / 'case.c'
                file.write_text('\n'.join(source))
                exe = path / ('case.exe' if os.name == 'nt' else 'case')
                subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
                    '-std=gnu11', '-O2', '-I' + str(ROOT / 'runtime'), str(file), '-o', str(exe)], check=True)
                subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
