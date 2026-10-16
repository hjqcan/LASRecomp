"""Small synthetic archives only: these tests contain no game data or keys."""
import io
import hashlib
from pathlib import Path
import random
import struct
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr
from unittest.mock import patch

import zstandard
from import_game import View, PatchView, pfs_entries, decompress_ncz, import_game, extract_romfs, romfs_header
from game_versions import BASE, BASE_ID, UPDATE_ID, address, game_version, patch_word, patch_table
from game_catalog import get_game


def pfs(names):
    strings = bytearray()
    records = bytearray()
    for i, name in enumerate(names):
        offset = len(strings)
        strings += name.encode() + b'\0'
        records += struct.pack('<QQII', i, 1, offset, 0)
    return struct.pack('<4sIII', b'PFS0', len(names), len(strings), 0) + records + strings + b'X' * len(names)


def ncz(payload, blocked=False, sections=None, checksum=False):
    sections = sections or [(0x4000, len(payload))]
    header = bytes(0x4000) + b'NCZSECTN' + struct.pack('<Q', len(sections))
    for start, size in sections:
        header += struct.pack('<QQQQ', start, size, 1, 0) + bytes(32)
    compressor = zstandard.ZstdCompressor(write_checksum=checksum)
    if not blocked:
        return header + compressor.compress(payload)
    block_size = 1 << 14
    blocks = []
    for offset in range(0, len(payload), block_size):
        raw = payload[offset:offset + block_size]
        compressed = compressor.compress(raw)
        blocks.append(compressed if len(compressed) < len(raw) else raw)
    header += b'NCZBLOCK' + struct.pack('<BBBBIQ', 2, 1, 0, 14, len(blocks), len(payload))
    header += struct.pack(f'<{len(blocks)}I', *(len(block) for block in blocks))
    return header + b''.join(blocks)


def pfs_files(files):
    strings, records, contents = bytearray(), bytearray(), bytearray()
    for name, data in files.items():
        records += struct.pack('<QQII', len(contents), len(data), len(strings), 0)
        strings += name.encode() + b'\0'
        contents += data
    return struct.pack('<4sIII', b'PFS0', len(files), len(strings), 0) + records + strings + contents


def synthetic_romfs():
    empty = 0xffffffff
    dirs = struct.pack('<6I', 0, empty, 24, empty, empty, 0)
    dirs += struct.pack('<6I', 0, empty, empty, 0, empty, 13) + b'region_common' + bytes(3)
    files = struct.pack('<IIQQII', 24, empty, 0, 8, empty, 11) + b'example.bin' + bytes(1)
    header = (80, 80, 4, 84, len(dirs), 84 + len(dirs), 4, 88 + len(dirs), len(files), 0x1000)
    image = bytearray(0x10000)
    struct.pack_into('<10Q', image, 0, *header)
    image[header[3]:header[3] + len(dirs)] = dirs
    image[header[7]:header[7] + len(files)] = files
    image[0x1000:0x1008] = b'original'
    return image


def synthetic_nso(build_id):
    main = bytearray(0x60)
    main[:4] = b'NSO0'
    main[0x40:0x60] = bytes.fromhex(build_id.ljust(64, '0'))
    return main


def synthetic_package(updated=False, blocked=False):
    main = synthetic_nso(UPDATE_ID if updated else BASE_ID)
    sdk = synthetic_nso(get_game().releases[0].modules[1].build_id)
    exefs = pfs_files({'main': main, 'main.npdm': b'synthetic metadata', 'sdk': sdk,
                       'rtld': b'synthetic loader'})
    section = synthetic_romfs()
    if updated:
        section = bytearray(len(section))
        section[:8] = b'changed!'
        # Two relocation buckets exercise base/patch reads across bucket boundaries.
        struct.pack_into('<IIQQQ', section, 0x4000, 0, 2, 0x10000, 0, 0x1000)
        struct.pack_into('<IIQ', section, 0x8000, 0, 1, 0x1000)
        struct.pack_into('<QQI', section, 0x8010, 0, 0, 0)
        struct.pack_into('<IIQ', section, 0xc000, 0, 2, 0x10000)
        struct.pack_into('<QQI', section, 0xc010, 0x1000, 0, 1)
        struct.pack_into('<QQI', section, 0xc024, 0x1008, 0x1008, 0)
    payload = bytes(section) + exefs
    return pfs_files({'synthetic.ncz': ncz(payload, blocked,
                      [(0x4000, len(section)), (0x4000 + len(section), len(exefs))])})


class ImportTests(unittest.TestCase):
    def test_pfs_file_ranges(self):
        archive = View(io.BytesIO(pfs(['main', 'sdk'])))
        entries = pfs_entries(archive)
        self.assertEqual([name for name, _, _ in entries], ['main', 'sdk'])
        self.assertEqual([archive.read(start, size) for _, start, size in entries], [b'X', b'X'])

    def test_untrusted_names_and_case_collisions(self):
        for name in ('../outside', 'a/b', 'C:drive', 'NUL', 'trailing.', 'a\\b'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                pfs_entries(View(io.BytesIO(pfs([name]))))
        with self.assertRaises(ValueError):
            pfs_entries(View(io.BytesIO(pfs(['main', 'MAIN']))))

    def test_pfs_truncated_and_overlapping_extents(self):
        with self.assertRaises(ValueError):
            pfs_entries(View(io.BytesIO(pfs(['main'])[:-1])))
        bad = bytearray(pfs(['main', 'sdk']))
        struct.pack_into('<Q', bad, 16 + 24, 0)
        with self.assertRaises(ValueError):
            pfs_entries(View(io.BytesIO(bad)))

    def test_solid_and_block_roundtrip_with_short_final_block(self):
        payload = b'A' * (1 << 14) + bytes(range(251)) * 3 + b'end'
        for blocked in (False, True):
            with self.subTest(blocked=blocked), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / 'decoded'
                sections = decompress_ncz(View(io.BytesIO(ncz(payload, blocked))), output)
                self.assertEqual(sections, [(0x4000, len(payload))])
                self.assertEqual(output.read_bytes(), bytes(0x4000) + payload)

    def test_truncated_compressed_stream_cannot_succeed(self):
        archive = ncz(b'ABCDEF' * 100)[:-3]
        with tempfile.TemporaryDirectory() as directory, self.assertRaises((ValueError, zstandard.ZstdError)):
            decompress_ncz(View(io.BytesIO(archive)), Path(directory) / 'decoded')

    def test_missing_checksum_and_trailing_frames_are_rejected(self):
        payload = b'synthetic payload' * 32
        malformed = [ncz(payload, checksum=True)[:-1], ncz(payload) + b'trailing',
                     ncz(payload) + zstandard.ZstdCompressor().compress(b'')]
        for archive in malformed:
            with self.subTest(length=len(archive)), tempfile.TemporaryDirectory() as directory:
                with self.assertRaises((ValueError, zstandard.ZstdError)):
                    decompress_ncz(View(io.BytesIO(archive)), Path(directory) / 'decoded')

    def test_uncompressed_blocks_and_short_final_block(self):
        payload = random.Random(42).randbytes((1 << 14) + 23)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'decoded'
            decompress_ncz(View(io.BytesIO(ncz(payload, True))), output)
            self.assertEqual(output.read_bytes()[0x4000:], payload)

    def test_block_header_size_and_count_are_validated(self):
        valid = ncz(b'A' * 100, True)
        for offset, value in ((0x4060, 2), (0x4064, 101)):
            archive = bytearray(valid)
            struct.pack_into('<I', archive, offset, value)
            with self.subTest(offset=offset), tempfile.TemporaryDirectory() as directory:
                with self.assertRaises(ValueError):
                    decompress_ncz(View(io.BytesIO(archive)), Path(directory) / 'decoded')

    def test_romfs_cycle_and_file_extent_are_rejected(self):
        for corrupt in ('cycle', 'extent'):
            image = synthetic_romfs()
            header = struct.unpack_from('<10Q', image)
            if corrupt == 'cycle':
                struct.pack_into('<I', image, header[3] + 24 + 4, 24)
            else:
                struct.pack_into('<Q', image, header[7] + 16, len(image) * 2)
            with self.subTest(corrupt=corrupt), tempfile.TemporaryDirectory() as directory:
                view = View(io.BytesIO(image))
                with self.assertRaises(ValueError):
                    extract_romfs(view, 0, romfs_header(view, 0), Path(directory) / 'romfs')

    def test_patch_read_crosses_multiple_source_boundaries(self):
        base = View(io.BytesIO(b'abcdefghij'))
        update = View(io.BytesIO(b'XYZ'))
        patch = PatchView(base, update, [(0, 0, 0), (3, 0, 1), (6, 6, 0)], 10)
        self.assertEqual(patch.read(0, 10), b'abcXYZghij')
        self.assertEqual(patch.read(2, 6), b'cXYZgh')
        self.assertEqual(base.read(0, 10), b'abcdefghij')
        with self.assertRaises(ValueError):
            patch.read(8, 3)

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


class InstallTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.base = self.root / 'base 游戏.nsz'
        self.update = self.root / 'update 游戏.nsz'
        self.output = self.root / 'extracted'
        self.base.write_bytes(synthetic_package())
        self.update.write_bytes(synthetic_package(True, True))
        self.stdout = self.enterContext(redirect_stdout(io.StringIO()))
        self.stderr = self.enterContext(redirect_stderr(io.StringIO()))
        # Synthetic fixtures need kilobytes, independent of the CI worker's disk.
        from types import SimpleNamespace
        self.enterContext(patch('import_game.shutil.disk_usage', return_value=SimpleNamespace(free=100 << 30)))

    def install(self, updated=False, replace=False):
        return import_game(self.base, self.update if updated else None, self.output, replace)

    def resource(self):
        return (self.output / 'romfs/region_common/example.bin').read_bytes()

    def test_base_then_update_replaces_old_code_and_preserves_sources(self):
        hashes = [hashlib.sha256(p.read_bytes()).digest() for p in (self.base, self.update)]
        self.assertEqual(self.install()['game_version'], '1.0.0')
        self.assertEqual(self.resource(), b'original')
        (self.output / 'lasgame.dll').write_bytes(b'stale compiled code')
        self.assertEqual(self.install(True, True)['game_version'], '1.1.0')
        self.assertEqual(self.resource(), b'changed!')
        self.assertEqual((self.output / 'romfs.bin').read_bytes()[0x1000:0x1008], b'changed!')
        self.assertFalse((self.output / 'lasgame.dll').exists())
        self.assertEqual(hashes, [hashlib.sha256(p.read_bytes()).digest() for p in (self.base, self.update)])
        self.assertFalse(list(self.root.glob('.las-import-*')))
        self.assertFalse(list(self.root.glob('*.previous-*')))

    def test_block_base_and_solid_update(self):
        self.base.write_bytes(synthetic_package(False, True))
        self.update.write_bytes(synthetic_package(True, False))
        self.install(True)
        self.assertEqual(self.resource(), b'changed!')

    def test_damaged_update_preserves_existing_installation(self):
        self.install()
        self.update.write_bytes(self.update.read_bytes()[:-10])
        with self.assertRaises(ValueError):
            self.install(True, True)
        self.assertEqual(self.resource(), b'original')
        self.assertFalse(list(self.root.glob('.las-import-*')))

    def test_failed_commit_restores_previous_installation(self):
        self.install()
        rename = Path.rename
        def fail_commit(path, destination):
            if path.name == 'ready':
                raise OSError('simulated rename failure')
            return rename(path, destination)
        with patch.object(Path, 'rename', fail_commit), self.assertRaises(OSError):
            self.install(True, True)
        self.assertEqual(self.resource(), b'original')
        self.assertFalse(list(self.root.glob('*.previous-*')))

    def test_locked_backup_does_not_report_failed_install(self):
        self.install()
        import shutil
        remove = shutil.rmtree
        def locked_backup(path, *args, **kwargs):
            if '.previous-' in str(path):
                raise PermissionError('simulated locked file')
            return remove(path, *args, **kwargs)
        with patch('import_game.shutil.rmtree', side_effect=locked_backup):
            self.assertEqual(self.install(True, True)['game_version'], '1.1.0')
        self.assertEqual(self.resource(), b'changed!')
        self.assertEqual(len(list(self.root.glob('*.previous-*'))), 1)
        self.assertIn('Import succeeded', self.stderr.getvalue())

    def test_refuse_unrelated_files_and_implicit_replacement(self):
        self.install()
        with self.assertRaises(ValueError):
            self.install(True)
        (self.output / 'save.sav').write_bytes(b'keep me')
        with self.assertRaises(ValueError):
            self.install(True, True)
        self.assertEqual((self.output / 'save.sav').read_bytes(), b'keep me')
        self.assertEqual(self.resource(), b'original')

    def test_refuse_source_inside_destination(self):
        self.output.mkdir()
        self.base = self.output / 'base.nsz'
        self.base.write_bytes(synthetic_package())
        with self.assertRaisesRegex(ValueError, 'outside'):
            self.install(replace=True)
        self.assertTrue(self.base.exists())

    def test_low_disk_space_leaves_installation_untouched(self):
        self.install()
        from types import SimpleNamespace
        with patch('import_game.shutil.disk_usage', return_value=SimpleNamespace(free=0)):
            with self.assertRaisesRegex(ValueError, 'free'):
                self.install(True, True)
        self.assertEqual(self.resource(), b'original')

    def test_unknown_executable_fails_before_commit(self):
        # Rebuild the compressed package with an unrecognized header ID.
        with patch(__name__ + '.BASE_ID', '01' * 20):
            self.base.write_bytes(synthetic_package())
        with self.assertRaises(ValueError):
            self.install()
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
