"""Version-specific address and instruction mapping, using synthetic words."""
import unittest
import struct
from game_versions import BASE, BASE_ID, UPDATE_ID, address, game_version, patch_word, patch_table


class VersionTests(unittest.TestCase):
    def test_supported_builds_and_changed_function_guard(self):
        self.assertEqual(game_version(BASE_ID), '1.0.0')
        self.assertEqual(game_version(UPDATE_ID), '1.1.0')
        with self.assertRaises(ValueError):
            game_version('unknown')
        self.assertEqual(address(BASE + 0xfabf70, '1.1.0'), BASE + 0xfac040)
        self.assertEqual(address(BASE + 0x1cbb220, '1.1.0'), BASE + 0x1cbb220)
        with self.assertRaises(ValueError):
            address(BASE + 0xdfaf00, '1.1.0')

    def test_patch_call_and_label_relocation(self):
        old = BASE + 0x100
        target = BASE + 0xfabf70
        new_target = address(target, '1.1.0')
        word = 0x94000000 | (((new_target - old) // 4) & 0x3ffffff)
        image = bytearray(0x104)
        struct.pack_into('<I', image, 0x100, word)
        from types import SimpleNamespace
        gen = SimpleNamespace(base=BASE, game_version='1.1.0', nso=SimpleNamespace(image=image))
        self.assertEqual(patch_word(gen, old), 0x94000000 | (((target - old) // 4) & 0x3ffffff))
        table = patch_table({old: lambda gen, a: 'f_7000fabf70(c); goto L_7000fabf74;'}, '1.1.0')
        self.assertEqual(table[old](gen, old), 'f_7000fac040(c); goto L_7000fac044;')

    def test_shifted_patch_site_with_backward_call(self):
        from types import SimpleNamespace
        old, target = BASE + 0xfabf70, BASE + 0x100
        actual = address(old, '1.1.0')
        image = bytearray(4)
        struct.pack_into('<I', image, 0, 0x94000000 | (((target - actual) // 4) & 0x3ffffff))
        gen = SimpleNamespace(base=actual, game_version='1.1.0', nso=SimpleNamespace(image=image))
        self.assertEqual(patch_word(gen, old), 0x94000000 | (((target - old) // 4) & 0x3ffffff))


if __name__ == '__main__':
    unittest.main()
