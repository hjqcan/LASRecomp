"""Synthetic gamecard containers: no game data, certificates or console keys."""
import hashlib
import io
from pathlib import Path
import struct
import tempfile
import unittest
from contextlib import redirect_stdout
from types import SimpleNamespace
from unittest.mock import patch

from containers import View, hfs_entries, open_container, pfs_entries
from import_game import import_game, inspect_package
from nca import Keyset
from test_import_game import synthetic_package
from test_nsp_import import package, nca, program_sections, HEADER, AREA, TITLE_KEK


def hfs(files):
    strings, records, data = bytearray(), bytearray(), bytearray()
    for name, content in files.items():
        prefix = min(512, len(content))
        records += struct.pack('<QQIIQ32s', len(data), len(content), len(strings), prefix, 0,
                               hashlib.sha256(content[:prefix]).digest())
        strings += name.encode() + b'\0'
        data += content
    strings += bytes((-(16 + len(records) + len(strings))) % 512)
    return struct.pack('<4sIII', b'HFS0', len(files), len(strings), 0) + records + strings + data


def gamecard(pfs, china=False, secure=True):
    view = View(io.BytesIO(pfs))
    files = {name: view.read(start, size) for name, start, size in pfs_entries(view)}
    # A deliberately unrelated firmware partition must never be interpreted as a game update.
    partitions = {'update': b'console firmware fixture', 'normal': b'normal partition fixture'}
    if secure:
        partitions['secure'] = hfs(files)
    root = hfs(partitions)
    header = bytearray(0xf000)
    header[0x100:0x104] = b'HEAD'
    struct.pack_into('<QQ', header, 0x130, len(header), 512)
    header[0x140:0x160] = hashlib.sha256(root[:512] + (b'\x01' if china else b'')).digest()
    return bytes(header) + root


class XciTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.keyfile = self.root / 'synthetic.keys'
        self.keyfile.write_text(f'header_key = {HEADER.hex()}\nkey_area_key_application_06 = {AREA.hex()}\n'
                                f'titlekek_06 = {TITLE_KEK.hex()}\n')
        self.keys = Keyset(self.keyfile)
        self.enterContext(patch('import_game.shutil.disk_usage', return_value=SimpleNamespace(free=100 << 30)))
        self.enterContext(redirect_stdout(io.StringIO()))

    def test_detects_xci_and_extracts_secure_base(self):
        source = self.root / 'misleading.nsz'
        source.write_bytes(gamecard(package()))
        before = source.read_bytes()
        info = inspect_package(source, self.keys)
        self.assertEqual((info['format'], info['role'], info['content_version']), ('XCI', 'base', 0))
        output = self.root / 'base'
        import_game(source, None, output, keys_path=self.keyfile, expected_version='1.0.0')
        self.assertEqual((output / 'romfs/region_common/example.bin').read_bytes(), b'original')
        self.assertEqual(source.read_bytes(), before)

    def test_xci_base_with_nsp_update(self):
        source, update = self.root / 'base.xci', self.root / 'update.nsp'
        source.write_bytes(gamecard(package()))
        update.write_bytes(package(True, rights=True))
        output = self.root / 'updated'
        import_game(source, update, output, keys_path=self.keyfile, expected_version='1.1.0')
        self.assertEqual((output / 'romfs/region_common/example.bin').read_bytes(), b'changed!')

    def test_xcz_solid_and_blocked_without_keys(self):
        for blocked in (False, True):
            source = self.root / f'base-{blocked}.xcz'
            source.write_bytes(gamecard(synthetic_package(blocked=blocked)))
            self.assertEqual(inspect_package(source)['format'], 'XCZ')
            output = self.root / str(blocked)
            import_game(source, None, output)
            self.assertEqual((output / 'romfs/region_common/example.bin').read_bytes(), b'original')

    def test_mixed_xcz_and_nsp_update(self):
        source, update = self.root / 'base.xcz', self.root / 'update.nsp'
        view = View(io.BytesIO(synthetic_package()))
        files = {n: view.read(o, s) for n, o, s in pfs_entries(view)}
        # Non-program encrypted content can coexist with a compressed main program.
        files['control.nca'] = nca(program_sections(), content_type=2)
        from test_import_game import pfs_files
        source.write_bytes(gamecard(pfs_files(files)))
        update.write_bytes(package(True))
        output = self.root / 'updated'
        import_game(source, update, output, keys_path=self.keyfile)
        self.assertEqual((output / 'romfs/region_common/example.bin').read_bytes(), b'changed!')

    def test_china_root_hash_suffix(self):
        source = self.root / 'china.xci'
        source.write_bytes(gamecard(package(), china=True))
        self.assertEqual(inspect_package(source, self.keys)['format'], 'XCI')

    def test_header_damage_and_bad_root_bounds(self):
        data = bytearray(gamecard(package()))
        for offset in (0x140, 0xf000 + 40):
            damaged = bytearray(data); damaged[offset] ^= 1
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                open_container(View(io.BytesIO(damaged)))
        struct.pack_into('<Q', data, 0x130, len(data) + 10)
        with self.assertRaisesRegex(ValueError, 'range'):
            open_container(View(io.BytesIO(data)))

    def test_missing_secure_and_truncation(self):
        with self.assertRaisesRegex(ValueError, 'secure'):
            open_container(View(io.BytesIO(gamecard(package(), secure=False))))
        with self.assertRaises(ValueError):
            open_container(View(io.BytesIO(gamecard(package())[:-1])))

    def test_hfs_unsafe_names_and_duplicate_names(self):
        for files in ({'../escape': b'bad'}, {'NUL': b'bad'}, {'a': b'one', 'A': b'two'}):
            with self.assertRaises(ValueError):
                hfs_entries(View(io.BytesIO(hfs(files))))

    def test_hfs_bounds_and_hashed_region(self):
        data = bytearray(hfs({'file': b'one'}))
        for offset, fmt, val in ((16, '<Q', 10000), (24, '<Q', 10000), (36, '<I', 4), (40, '<Q', 1)):
            damaged = bytearray(data); struct.pack_into(fmt, damaged, offset, val)
            with self.assertRaises(ValueError):
                hfs_entries(View(io.BytesIO(damaged)))
        data[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            hfs_entries(View(io.BytesIO(data)))

    def test_slot_mismatch_preserves_other_installation(self):
        source, update = self.root / 'base.xci', self.root / 'update.nsp'
        source.write_bytes(gamecard(package()))
        update.write_bytes(package(True))
        output = self.root / 'base'
        import_game(source, None, output, keys_path=self.keyfile, expected_version='1.0.0')
        with self.assertRaisesRegex(ValueError, 'installation slot'):
            import_game(source, update, output, True, self.keyfile, expected_version='1.0.0')
        self.assertEqual((output / 'romfs/region_common/example.bin').read_bytes(), b'original')


if __name__ == '__main__':
    unittest.main()
