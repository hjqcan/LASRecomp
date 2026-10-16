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
    def test_targets_are_discovered_and_emitted_across_fragment_boundary(self):
        base = 0x7008000000
        image = bytearray(0x4000)
        words = [0xd65f03c0] * (0x200 // 4)
        words[:8] = [
            0x2a0003e8,                         # mov w8, w0
            0x7100051f,                         # cmp w8, #1
            0x54000148,                         # b.hi .+0x28
            adr(11, base + 12, base + 0x80),    # table base
            adr(9, base + 16, base + 0x100),    # target base
            0x7868796a,                         # ldrh w10, [x11, x8, lsl #1]
            0x8b0a0929,                         # add x9, x9, x10, lsl #2
            0xd61f0120,                         # br x9
        ]
        struct.pack_into('<128I', image, 0, *words)
        struct.pack_into('<2H', image, 0x80, 4, 32)
        m = Module.__new__(Module)
        m.name = 'test'; m.base = m.lo = base; m.hi = base + 0x200
        m.nso = SimpleNamespace(image=image)
        m.words = tuple(words); m.cache = {}; m.stats = Counter(); m.mu = None
        m.lifter = Lifter(base, 'c')
        targets = [base + 0x110, base + 0x180]
        self.assertEqual(m.jump_table(base + 28, base, base + 0x40), targets)
        labels, external, tables = m.scan(base, base + 0x40)
        self.assertTrue(set(targets).issubset(external))
        self.assertFalse(set(targets) & labels)
        code = '\n'.join(m.emit_function(base, base + 0x40, labels, tables, {}))
        for target in targets:
            self.assertIn(f'return f_{target:x}(c);', code)
            self.assertNotIn(f'goto L_{target:x};', code)


if __name__ == '__main__':
    unittest.main()
