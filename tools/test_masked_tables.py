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
from test_unbounded_tables import BASE, ROOT, R_AARCH64_RELATIVE, branch, module, replace_word


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
    m.table_addresses = [BASE + 0x200]
    m.table_count = 1
    return m


def masked_family(count=2, byte_table=False, wide=False):
    m = module(byte_table)
    m.hi = BASE + count * 0x100 + 0x40
    m.words = (0xD65F03C0,) * ((m.hi - BASE) // 4)
    struct.pack_into(f'<{len(m.words)}I', m.nso.image, 0, *m.words)
    m._function_entries = [BASE, BASE + count * 0x100]
    m.branches, m.table_addresses = [], []
    m.table_count = count
    for block in range(count):
        start, table = BASE + block * 0x100, BASE + 0x2000 + block * 16
        code = [0xD503201F, 0xD503201F, adr(9, start + 8, table),
                0x92400588 if wide else 0x12000588,
                adr(10, start + 16, start + 0xA0),
                0x3868692B if byte_table else 0x7868792B, 0x8B0B094A]
        code += [0x1E21294A] * 18
        m.branches.append(start + len(code) * 4)
        code.append(0xD61F0140)
        for i, word in enumerate(code): replace_word(m, start - BASE + i * 4, word)
        for i in range(4):
            case = start + 0xA0 + i * 8
            replace_word(m, case - BASE, 0x91000000 | ((i + 1) << 10))
            replace_word(m, case - BASE + 4, branch(case + 4, start + 0xC0))
        if block + 1 < count:
            replace_word(m, start - BASE + 0xC0, branch(start + 0xC0, start + 0x100))
        struct.pack_into('<4B' if byte_table else '<4H', m.nso.image, table - BASE, 0, 2, 4, 6)
        m.table_addresses.append(table)
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

    def test_multiple_tables_require_complete_local_proofs(self):
        for count in (2, 4):
            m = masked_family(count)
            for i, pc in enumerate(m.branches):
                self.assertEqual(m.jump_table(pc, BASE, m.hi),
                                 [BASE + i * 0x100 + 0xA0 + j * 8 for j in range(4)])

    def test_an_invalid_peer_rejects_the_entire_group(self):
        for offset, word in ((0x10C, 0x121C0D88), (0x11C, 0xAA1F03EA), (0x108, 0xAA1303E9)):
            m = masked_family(); replace_word(m, offset, word)
            self.assertIsNone(m.jump_table(m.branches[0], BASE, m.hi))

    def test_indirect_edges_cannot_bypass_a_peer_mask_or_address_setup(self):
        for byte_table in (False, True):
            for target in (BASE + 0x108, BASE + 0x10C, BASE + 0x110, BASE + 0x164):
                m = masked_family(byte_table=byte_table)
                struct.pack_into('<B' if byte_table else '<H', m.nso.image, 0x2000,
                                 (target - (BASE + 0xA0)) // 4)
                for pc in m.branches: self.assertIsNone(m.jump_table(pc, BASE, m.hi))
            m = masked_family(byte_table=byte_table)
            struct.pack_into('<B' if byte_table else '<H', m.nso.image, 0x2000, (0x100 - 0xA0) // 4)
            self.assertIsNotNone(m.jump_table(m.branches[0], BASE, m.hi))  # complete setup executes

    def test_direct_entry_at_mask_cannot_skip_table_address_definition(self):
        m = masked_module(); replace_word(m, 0, branch(BASE, BASE + 12))
        self.assertIsNone(self.targets(m))

    def test_external_entries_cannot_bypass_any_peer_setup(self):
        for kind in ('adr', 'adrp', 'call', 'branch', 'cbz', 'tbz', 'relocation', 'symbol'):
            for offset in (0x100, 0x108, 0x10C, 0x110, 0x164):
                with self.subTest(kind=kind, offset=hex(offset)):
                    m = masked_family()
                    target, source = BASE + offset, BASE + 0x200
                    if kind == 'adr':
                        replace_word(m, 0x200, adr(15, source, target))
                    elif kind == 'adrp':
                        replace_word(m, 0x200, 0x9000000F)  # adrp x15, current page
                        replace_word(m, 0x204, 0x910001EF | (offset << 10))
                    elif kind in ('call', 'branch'):
                        replace_word(m, 0x200, branch(source, target, kind == 'call'))
                    elif kind in ('cbz', 'tbz'):
                        mask = 0x7FFFF if kind == 'cbz' else 0x3FFF
                        replace_word(m, 0x200, (0x34000000 if kind == 'cbz' else 0x36000000) |
                                     ((((target - source) // 4) & mask) << 5))
                    elif kind == 'relocation':
                        m.nso.relocs = [(0x3000, R_AARCH64_RELATIVE, 0, offset)]
                    else:
                        m._function_entries.insert(1, target)
                    for pc in m.branches:
                        result = m.jump_table(pc, BASE, m.hi)
                        if kind == 'symbol' and (pc == m.branches[0] or offset <= 0x108):
                            self.assertIsNotNone(result)  # a separate function has its own proof
                        elif offset == 0x100:
                            self.assertIsNotNone(result)  # all setup still executes
                        else:
                            self.assertIsNone(result)

    def test_reference_inventory_refreshes_with_replaced_module_data(self):
        m = masked_family()
        self.assertIsNotNone(m.jump_table(m.branches[0], BASE, m.hi))
        replace_word(m, 0x200, adr(15, BASE + 0x200, BASE + 0x10C))
        self.assertIsNone(m.jump_table(m.branches[0], BASE, m.hi))
        m = masked_family()
        self.assertIsNotNone(m.jump_table(m.branches[0], BASE, m.hi))
        m.nso.relocs = [(0x3000, R_AARCH64_RELATIVE, 0, 0x10C)]
        self.assertIsNone(m.jump_table(m.branches[0], BASE, m.hi))

    def test_discovered_entries_invalidate_peer_proofs(self):
        m = masked_family()
        entries, _ = m.discover_functions(m.boundaries() + [BASE + 0x10C])
        self.assertNotIn(m.branches[0], entries[BASE][2])
        m = masked_family()
        code = [0x7100011F, 0x54000108, adr(9, BASE + 0x208, BASE + 0x3000),
                adr(10, BASE + 0x20C, BASE + 0x100), 0x7868792B, 0x8B0B094A, 0xD61F0140]
        for i, word in enumerate(code): replace_word(m, 0x200 + i * 4, word)
        struct.pack_into('<H', m.nso.image, 0x3000, 3)  # external table enters at peer AND
        entries, rounds = m.discover_functions(m.boundaries())
        self.assertGreater(rounds, 1)
        self.assertIn(BASE + 0x10C, entries)
        self.assertNotIn(m.branches[0], entries[BASE][2])

    def test_self_edges_and_excessive_groups_are_rejected(self):
        m = masked_module(); replace_word(m, m.origin_pc - BASE, adr(10, m.origin_pc, BASE + 12))
        self.assertIsNone(self.targets(m))
        m = masked_family(17)
        self.assertIsNone(m.jump_table(m.branches[0], BASE, m.hi))

    def test_native_results_match_arm64_for_all_masked_inputs(self):
        inputs = [0, 1, 2, 3, 4, 7, 0x100, 0x103, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF]
        for count, byte_table, wide in [(n, b, w) for n in (1, 2, 4) for b in (False, True) for w in (False, True)]:
            m = masked_module(byte_table, wide) if count == 1 else masked_family(count, byte_table, wide)
            expected_fp = struct.unpack('<I', struct.pack('<f', 2.0 + 9.0 * count))[0]
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
                mu.emu_start(BASE, BASE + 0x1000, count=800)
                self.assertEqual(mu.reg_read(A.UC_ARM64_REG_PC), BASE + 0x1000)
                self.assertEqual(mu.reg_read(A.UC_ARM64_REG_X0), 10 + count * (1 + (value & 3)))
                self.assertEqual(mu.reg_read(A.UC_ARM64_REG_Q10), expected_fp)
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
                       '#endif', f'    assert(data==(void *)0x{BASE:x}ull);']
            for i, table in enumerate(m.table_addresses):
                source += [f'    uint{8 if byte_table else 16}_t *table{i}=(void *)((char *)data+{table-BASE});',
                           f'    table{i}[0]=0; table{i}[1]=2; table{i}[2]=4; table{i}[3]=6;']
            source += [
                       '    const uint64_t inputs[]={' + ','.join(f'{v}ull' for v in inputs) + '};',
                       f'    for(unsigned i=0;i<{len(inputs)};i++) {{',
                       '        Cpu c={0}; c.x[0]=10; c.x[12]=inputs[i]; c.x[30]=0x1234; c.sp=0x8000;',
                       '        c.v[10]=0x40000000; c.v[1]=0x3f000000;', f'        f_{BASE:x}(&c);',
                       f'        assert(c.x[0]==10+{count}*(1+(inputs[i]&3)) && c.v[10]==0x{expected_fp:x});',
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
