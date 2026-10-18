"""Masked relative tables with scheduled vector work; no game assets required."""
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import unittest

import unicorn as uc
from unicorn import arm64_const as A
from test_cross_fragment_tables import adr
from test_unbounded_tables import BASE, ROOT, branch, module, replace_word


def masked_module(byte_table=False, wide=False, between=()):
    m = module(byte_table)
    m._function_entries = [BASE, BASE + 0xE0]
    for offset in range(0, 0x100, 4):
        replace_word(m, offset, 0xD65F03C0)
    code = [0xD503201F, 0xD503201F, adr(9, BASE + 8, BASE + 0x200),
            0x92400588 if wide else 0x12000588]  # and x8/w8,x12/w12,#3
    code += list(between)
    m.origin_pc = BASE + len(code) * 4
    code += [adr(10, m.origin_pc, BASE + 0xA0),
             0x3868692B if byte_table else 0x7868792B, 0x8B0B094A]
    m.gap_pc = BASE + len(code) * 4
    code += [0x1E21294A] * 18  # fadd s10,s10,s1: S10 does not overwrite X10
    m.br_pc = BASE + len(code) * 4
    code += [0xD61F0140]
    for i, word in enumerate(code):
        replace_word(m, i * 4, word)
    for i in range(4):
        offset = 0xA0 + i * 8
        replace_word(m, offset, 0x91000000 | ((i + 1) << 10))
        replace_word(m, offset + 4, branch(BASE + offset + 4, BASE + 0xC0))
    struct.pack_into('<4B' if byte_table else '<4H', m.nso.image, 0x200, 0, 2, 4, 6)
    return m


class MaskedTableTests(unittest.TestCase):
    def targets(self, m):
        return m.jump_table(m.br_pc, BASE, BASE + 0xE0)

    def test_mask_bounds_byte_and_halfword_tables_across_vector_work(self):
        for byte_table in (False, True):
            for wide in (False, True):
                m = masked_module(byte_table, wide)
                self.assertEqual(self.targets(m), [BASE + 0xA0 + i * 8 for i in range(4)])
                entries, _ = m.discover_functions(m.boundaries())
                self.assertTrue(all(BASE + 0xA0 + i * 8 in entries for i in range(4)))

    def test_target_register_writes_calls_and_branches_are_barriers(self):
        for word in (0xAA1F03EA, 0x2A1F03EA, 0xF940002A, 0x94000001, 0x14000001):
            with self.subTest(word=hex(word)):
                m = masked_module(); replace_word(m, m.gap_pc - BASE, word)
                self.assertIsNone(self.targets(m))

    def test_reaching_index_definition_must_be_the_mask(self):
        m = masked_module(between=(0x2A0C03E8,))  # mov w8,w12: loses the bound
        self.assertIsNone(self.targets(m))
        m = masked_module(between=(0x2A1F03EC,))  # mov w12,wzr: bound in W8 survives
        self.assertEqual(len(self.targets(m)), 4)

    def test_non_low_mask_and_unmasked_index_are_rejected(self):
        for word in (0x121C0D88, 0x1200F188, 0x2A0C03E8):  # #0xf0, #0x55555555, MOV
            m = masked_module(); replace_word(m, 12, word)
            self.assertIsNone(self.targets(m))

    def test_incoming_edges_cannot_bypass_the_mask(self):
        for kind in ('b', 'bl', 'cbz', 'tbz', 'backedge'):
            m = masked_module()
            if kind in ('b', 'bl'):
                replace_word(m, 0, branch(BASE, m.origin_pc, kind == 'bl'))
            elif kind == 'backedge':
                replace_word(m, 0xC4, branch(BASE + 0xC4, m.origin_pc))
            else:
                replace_word(m, 0, (0x34000000 if kind == 'cbz' else 0x36000000) |
                             (((m.origin_pc - BASE) // 4) << 5))
            self.assertIsNone(self.targets(m), kind)

    def test_independent_entry_cannot_borrow_an_earlier_mask(self):
        m = masked_module()
        self.assertIsNone(m.jump_table(m.br_pc, m.origin_pc, BASE + 0xE0))
        m._function_entries.insert(1, m.origin_pc)
        self.assertIsNone(self.targets(m))

    def test_other_indirect_branches_cannot_introduce_unknown_incoming_edges(self):
        m = masked_module(); replace_word(m, 0xC4, 0xD61F0160)  # br x11
        self.assertIsNone(self.targets(m))

    def test_native_results_match_arm64_for_all_masked_inputs(self):
        inputs = [0, 1, 2, 3, 4, 7, 0x100, 0x103, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF]
        for byte_table, wide in ((False, False), (True, False), (False, True), (True, True)):
            m = masked_module(byte_table, wide)
            mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
            mu.mem_map(BASE, 0x4000); mu.mem_write(BASE, bytes(m.nso.image))
            mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)
            for value in inputs:
                for i in range(31): mu.reg_write(getattr(A, f'UC_ARM64_REG_X{i}'), 0)
                mu.reg_write(A.UC_ARM64_REG_X0, 10)
                mu.reg_write(A.UC_ARM64_REG_X12, value)
                mu.reg_write(A.UC_ARM64_REG_Q10, 0x40000000)  # 2.0f
                mu.reg_write(A.UC_ARM64_REG_Q1, 0x3F000000)   # 0.5f
                mu.reg_write(A.UC_ARM64_REG_SP, 0x8000)
                mu.reg_write(A.UC_ARM64_REG_X30, BASE + 0x1000)
                mu.emu_start(BASE, BASE + 0x1000, count=200)
                self.assertEqual(mu.reg_read(A.UC_ARM64_REG_PC), BASE + 0x1000)
                self.assertEqual(mu.reg_read(A.UC_ARM64_REG_X0), 11 + (value & 3))
                self.assertEqual(mu.reg_read(A.UC_ARM64_REG_Q10), 0x41300000)  # 11.0f
            entries, _ = m.discover_functions(m.boundaries())
            source = ['#include <assert.h>', '#include <stdlib.h>', '#include "cpu.h"', '#include "simd.h"',
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
                       f'    uint{8 if byte_table else 16}_t *table=(void *)((char *)data+0x200);',
                       '    table[0]=0; table[1]=2; table[2]=4; table[3]=6;',
                       '    const uint64_t inputs[]={' + ','.join(f'{v}ull' for v in inputs) + '};',
                       f'    for(unsigned i=0;i<{len(inputs)};i++) {{',
                       '        Cpu c={0}; c.x[0]=10; c.x[12]=inputs[i]; c.x[30]=0x1234; c.sp=0x8000;',
                       '        c.v[10]=0x40000000; c.v[1]=0x3f000000;', f'        f_{BASE:x}(&c);',
                       '        assert(c.x[0]==11+(inputs[i]&3) && c.v[10]==0x41300000);',
                       '        assert(c.sp==0x8000 && c.x[30]==0x1234);', '    }',
                       '#ifdef _WIN32', '    VirtualFree(data,0,MEM_RELEASE);', '#else',
                       '    munmap(data,0x4000);', '#endif', '    return 0;', '}']
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory); file = path / 'case.c'; file.write_text('\n'.join(source))
                exe = path / ('case.exe' if os.name == 'nt' else 'case')
                subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
                    '-std=gnu11', '-O2', '-I' + str(ROOT / 'runtime'), str(file), '-o', str(exe)] +
                    ([] if os.name == 'nt' else ['-lm']), check=True)
                subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
