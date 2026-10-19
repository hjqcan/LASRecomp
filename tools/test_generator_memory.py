"""Bound generator memory without changing emitted control flow or diagnostics."""
from array import array
from types import SimpleNamespace
from unittest.mock import patch
import struct
import tracemalloc
import unittest

import gen
from adapter_patches import apply_instruction_patches
from test_guest_jump import BASE, branch, module


class GeneratorMemoryTests(unittest.TestCase):
    def test_large_stream_has_bounded_retained_memory(self):
        m = module(array('I', [0x91000400]) * 600000)
        tracemalloc.start()
        try:
            for pc in range(m.lo, m.hi, 4):
                m.lift(pc)
            retained, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertLess(peak, 64 * 1024 * 1024)
        self.assertLess(retained, 64 * 1024 * 1024)

    def test_eviction_preserves_scan_and_emission(self):
        words = [0x91000400] * 256
        words[0] = branch(BASE, BASE + 0x300)
        words[1] = 0x35000060  # cbnz w0, +12
        words[4] = 0xffffffff  # unsupported instruction
        words[127] = 0x14000002  # branch across the eviction boundary
        words[-1] = 0xd65f03c0
        syms = [('setjmp', 18, 1, 0x300, 0)]
        reference, bounded = module(words, syms), module(words, syms)
        reference.LIFT_CACHE_LIMIT = len(words) + 1
        bounded.LIFT_CACHE_LIMIT = 8
        expected = reference.scan(BASE, reference.hi)
        for _ in range(3):
            actual = bounded.scan(BASE, bounded.hi)
            self.assertEqual(actual, expected)
            self.assertEqual(bounded.emit_function(BASE, bounded.hi, actual[0], actual[2], {}),
                             reference.emit_function(BASE, reference.hi, expected[0], expected[2], {}))
            self.assertEqual(bounded._refs, reference._refs)
        self.assertEqual(reference.stats['unsupported'], 1)
        self.assertEqual(bounded.stats['unsupported'], 1)

    def test_compact_words_retain_little_endian_and_patch_guards(self):
        image = bytearray(struct.pack('<4I', 0x91000400, 0x14000002, 0xd503201f, 0xd65f03c0))
        with patch.object(gen, 'Nso', return_value=SimpleNamespace(image=image, text=(0, len(image)))):
            m = gen.Module('main', 'synthetic', BASE, 'c')
        original = tuple(m.words)
        self.assertEqual(original, (0x91000400, 0x14000002, 0xd503201f, 0xd65f03c0))
        edit = dict(module='main', offset=0, expected=original[0], replacement=0x91000800)
        invalid = dict(module='main', offset=4, expected=0, replacement=0xd503201f)
        with self.assertRaises(ValueError):
            apply_instruction_patches(m, SimpleNamespace(instruction_patches=[edit, invalid]))
        self.assertEqual(tuple(m.words), original)
        self.assertEqual(struct.unpack('<4I', image), original)
        self.assertEqual(apply_instruction_patches(m, SimpleNamespace(instruction_patches=[edit])), 1)
        self.assertEqual(m.word(BASE), 0x91000800)
        self.assertEqual(struct.unpack_from('<I', image)[0], m.word(BASE))


if __name__ == '__main__':
    unittest.main()
