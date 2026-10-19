"""Synthetic ARM64 switch table whose destinations are in other text fragments."""
import struct
import unittest
from collections import Counter
from types import SimpleNamespace
from gen import Module
from lifter import Lifter


def adr(reg, pc, target):
    imm = (target - pc) & 0x1fffff
    return 0x10000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | reg


class CrossFragmentTableTest(unittest.TestCase):
    def run_table(self, with_object_loads=False, loads_after_guard=False):
        base = 0x7008000000
        image = bytearray(0x4000)
        words = [0xd65f03c0] * (0x200 // 4)
        prefix = [0xf940002e, 0xf94001cf] if with_object_loads else []  # ldr x14,[x1]; ldr x15,[x14]
        at = base + len(prefix) * 4
        suffix = [0xf9401400, 0xf9400014] if loads_after_guard else []  # ldr x0,[x0,#40]; ldr x20,[x0]
        lookup = at + len(suffix) * 4
        code = prefix + [
            0x2a0003e8,                         # mov w8, w0
            0x7100051f,                         # cmp w8, #1
            0x54000148,                         # b.hi .+0x28
        ] + suffix + [
            adr(11, lookup + 12, base + 0x80),  # table base
            adr(9, lookup + 16, base + 0x100),  # target base
            0x7868796a,                         # ldrh w10, [x11, x8, lsl #1]
            0x8b0a0929,                         # add x9, x9, x10, lsl #2
            0xd61f0120,                         # br x9
        ]
        words[:len(code)] = code
        struct.pack_into('<128I', image, 0, *words)
        struct.pack_into('<2H', image, 0x80, 4, 32)
        m = Module.__new__(Module)
        m.name = 'test'; m.base = m.lo = base; m.hi = base + 0x200
        m.nso = SimpleNamespace(image=image)
        m._function_entries = [base]
        m.words = tuple(words); m.cache = {}; m.stats = Counter(); m.mu = None
        m.lifter = Lifter(base, 'c')
        targets = [base + 0x110, base + 0x180]
        self.assertEqual(m.jump_table(lookup + 28, base, base + 0x40), targets)
        labels, external, tables = m.scan(base, base + 0x40)
        self.assertTrue(set(targets).issubset(external))
        self.assertFalse(set(targets) & labels)
        code = '\n'.join(m.emit_function(base, base + 0x40, labels, tables, {}))
        for target in targets:
            self.assertIn(f'return f_{target:x}(c);', code)
            self.assertNotIn(f'goto L_{target:x};', code)

    def test_targets_are_discovered_and_emitted_across_fragment_boundary(self):
        self.run_table()

    def test_unrelated_object_reads_before_guard_do_not_hide_targets(self):
        self.run_table(with_object_loads=True)

    def test_unrelated_object_reads_after_guard_do_not_hide_targets(self):
        self.run_table(loads_after_guard=True)

    def test_hoisted_table_base_precedes_current_fragment(self):
        base = 0x7008000000
        image = bytearray(0x4000)
        words = [0xd65f03c0] * 128
        words[:8] = [adr(11, base, base + 0x80), 0x2a0003e8, 0x7100051f, 0x54000148,
                     adr(9, base + 16, base + 0x100), 0x7868796a, 0x8b0a0929, 0xd61f0120]
        struct.pack_into('<128I', image, 0, *words)
        struct.pack_into('<2H', image, 0x80, 4, 32)
        m = Module.__new__(Module)
        m.name = 'test'; m.base = m.lo = base; m.hi = base + 0x200
        m.nso = SimpleNamespace(image=image); m._function_entries = [base]
        m.words = tuple(words); m.cache = {}; m.stats = Counter(); m.mu = None
        m.lifter = Lifter(base, 'c')
        self.assertEqual(m.jump_table(base + 28, base + 4, base + 0x40), [base + 0x110, base + 0x180])

    def table_module(self, code):
        base = 0x7008000000
        image = bytearray(0x4000)
        words = [0xd65f03c0] * 128
        words[:len(code)] = code
        struct.pack_into('<128I',image,0,*words)
        struct.pack_into('<2H',image,0x80,4,32)
        m = Module.__new__(Module)
        m.name='test'; m.base=m.lo=base; m.hi=base+0x200
        m.nso=SimpleNamespace(image=image); m._function_entries=[base]
        m.words=tuple(words); m.cache={}; m.stats=Counter(); m.mu=None; m.lifter=Lifter(base,'c')
        return m

    def test_guard_in_an_earlier_fragment_for_taken_table(self):
        b=0x7008000000
        m=self.table_module([0x7100041f,0x54000069,0xd65f03c0,0xd503201f,
            adr(11,b+16,b+0x80),adr(9,b+20,b+0x100),0x7860796a,0x8b0a0929,0xd61f0120])
        self.assertEqual(m.jump_table(b+32,b+16,b+0x40),[b+0x110,b+0x180])

    def test_fallthrough_guard_with_scheduled_register_initializers(self):
        b=0x7008000000
        # The lookup result remains in X9 while other case-live registers are
        # initialized. This includes LR used as an ordinary saved temporary.
        for byte_table in (False, True):
            code=[0x7100041f,0x54000de8,adr(11,b+8,b+0x80),adr(9,b+12,b+0x100),
                  0x3860696a if byte_table else 0x7860796a,0x8b0a0929]
            registers=[r for r in range(31) if r!=9][:19]+[30]
            code += [0x52800000 | ((r+1)<<5) | r for r in registers]
            at=b+len(code)*4;code.append(0xd61f0120)
            m=self.table_module(code)
            if byte_table:struct.pack_into('<2B',m.nso.image,0x80,4,32)
            targets=[b+0x110,b+0x180]
            self.assertEqual(m.jump_table(at,b,b+0x80),targets)
            labels,external,tables=m.scan(b,b+0x80)
            self.assertTrue(set(targets).issubset(external))
            emitted='\n'.join(m.emit_function(b,b+0x80,labels,tables,{}))
            for target in targets:self.assertIn(f'return f_{target:x}(c);',emitted)
            # A call breaks the proof, even inside the wider search window.
            code[8]=0x94000030;m=self.table_module(code)
            self.assertIsNone(m.jump_table(at,b,b+0x80))

    def test_taken_guard_before_long_stack_frame_prologue(self):
        b=0x7008000000
        # cmp w0,#1; b.ls body; ret; sub sp,sp,#32; stp x29,x30,[sp]; mov x29,sp.
        prologue=[0xd10083ff,0xa9007bfd,0x910003fd]+[0xd503201f]*12
        code=[0x7100041f,0x54000049,0xd65f03c0]+prologue
        at=b+4*len(code)
        code += [adr(11,at,b+0x80),adr(9,at+4,b+0x100),0x7860796a,0x8b0a0929,0xd61f0120]
        m=self.table_module(code)
        targets=[b+0x110,b+0x180]
        self.assertEqual(m.jump_table(at+16,b,b+0x80),targets)
        labels,external,tables=m.scan(b,b+0x80)
        self.assertTrue(set(targets).issubset(external))
        self.assertEqual(tables[at+16],targets)
        emitted='\n'.join(m.emit_function(b,b+0x80,labels,tables,{}))
        for target in targets:
            self.assertIn(f'return f_{target:x}(c);',emitted)
        # A call may clobber the index; do not walk through it to use the guard.
        code[6]=0x94000030
        m=self.table_module(code)
        self.assertIsNone(m.jump_table(at+16,b,b+0x80))

    def test_index_copies_survive_a_call_before_the_guard(self):
        b=0x7008000000
        m=self.table_module([0x92407c13,0xaa0003f4,0xd2800540,0x9400000d,
            0x7100069f,0x54000148,adr(11,b+24,b+0x80),adr(9,b+28,b+0x100),
            0x7873796a,0x8b0a0929,0xd61f0120])
        self.assertEqual(m._surviving_index_aliases(b+16,b+16,20),[19,20])
        self.assertEqual(m.jump_table(b+40,b+16,b+0x40),[b+0x110,b+0x180])

    def test_conditional_compare_guard_proves_the_index_bound(self):
        b=0x7008000000
        # cmp w1,w2; ccmp w0,#1,#2,ne; b.hi default
        m=self.table_module([0x6b02003f,0x7a411802,0x54000148,
            adr(11,b+12,b+0x80),adr(9,b+16,b+0x100),0x7860796a,0x8b0a0929,0xd61f0120])
        self.assertEqual(m.jump_table(b+28,b,b+0x40),[b+0x110,b+0x180])
        # Default flags #0 do not take b.hi: then the index has no proven bound.
        m.words=tuple([*m.words[:1],0x7a411800,*m.words[2:]])
        self.assertIsNone(m.jump_table(b+28,b,b+0x40))

    def test_two_tables_share_flags_across_a_test_bit_branch(self):
        b=0x7008000000
        # cmp w0,#1; tbz w1,#0,second; first: ...; second: b.hi default
        m=self.table_module([0x7100041f,0x360000a1,0x54000208,
            0x94000030,0x14000030,0xd503201f,0x54000148,
            adr(11,b+28,b+0x80),adr(9,b+32,b+0x100),
            0x7860796a,0x8b0a0929,0xd61f0120])
        self.assertEqual(m.jump_table(b+44,b+24,b+0x40),[b+0x110,b+0x180])
        # Reaching flags from a call cannot establish the bound.
        m.words=tuple([0x94000030,*m.words[1:]])
        struct.pack_into('<I',m.nso.image,0,0x94000030)
        m.cache.clear()
        self.assertIsNone(m.jump_table(b+44,b+24,b+0x40))


if __name__ == '__main__':
    unittest.main()
