"""Inventory already-extracted executable headers without installing any game."""
from contextlib import redirect_stdout, redirect_stderr
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest

from adapter import main
from game_catalog import get_game
from probe_exefs import probe_exefs


def nso(identity='42' * 32):
    data = bytearray(0x10c)
    data[:4] = b'NSO0'
    data[0x40:0x60] = bytes.fromhex(identity)
    for i in range(3):
        struct.pack_into('<III', data, 0x10 + i * 0x10, 0x100 + i * 4, i * 0x1000, 4)
    struct.pack_into('<I', data, 0x3c, 32)
    return bytes(data)


def exefs(known=False, extra=False):
    release = get_game().releases[0]
    files = {'main': nso(release.main_build_id if known else '42' * 32), 'main.npdm': b'fixture',
             'sdk': nso(release.modules[1].build_id if known else '43' * 32), 'rtld': nso('44' * 32)}
    if extra: files['subsdk0'] = nso('45' * 32)
    return files


class ProbeTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.source = self.root / 'exefs'
        self.source.mkdir()

    def write(self, files):
        for name, data in files.items():
            (self.source / name).write_bytes(data)

    def test_unknown_modules_can_be_inventoried_without_installing(self):
        self.write(exefs())
        result = probe_exefs(self.source)
        self.assertEqual(result['adapter'], {'status': 'unregistered', 'installable': False})
        self.assertEqual(result['modules'][0]['build_id'], '42' * 32)
        self.assertEqual(result['modules'][0]['image_size'], 0x3000)
        self.assertEqual({p.name: p.read_bytes() for p in self.source.iterdir()}, exefs())
        self.assertEqual(list(self.root.iterdir()), [self.source])

    def test_exact_and_unreviewed_module_sets(self):
        self.write(exefs(known=True))
        self.assertEqual(probe_exefs(self.source)['adapter']['status'], 'matched')
        self.write(exefs(known=True, extra=True))
        result = probe_exefs(self.source)
        self.assertEqual(result['adapter']['status'], 'module-mismatch')
        self.assertEqual(result['adapter']['unreviewed_modules'], ['subsdk0'])
        self.assertFalse(result['adapter']['installable'])
        (self.source / 'sdk').write_bytes(nso('43' * 32))
        self.assertEqual(probe_exefs(self.source)['adapter']['mismatched_modules'], ['sdk'])

    def test_invalid_segment_bounds_and_overlaps(self):
        for offset, values in ((0x10, (0x10b, 0, 4)), (0x20, (0x100, 0x1000, 4)),
                               (0x20, (0x104, 0, 4)), (0x30, (0x108, 0x80000000, 4))):
            files = exefs()
            invalid = bytearray(files['main'])
            struct.pack_into('<III', invalid, offset, *values)
            files['main'] = invalid
            self.write(files)
            with self.subTest(offset=offset, values=values), self.assertRaises(ValueError):
                probe_exefs(self.source)

    def test_requires_a_directory_and_main_module(self):
        archive = self.root / 'game.nsz'
        archive.write_bytes(b'not an extracted directory')
        with self.assertRaisesRegex(ValueError, 'ExeFS directory'):
            probe_exefs(archive)
        with self.assertRaisesRegex(ValueError, 'No main'):
            probe_exefs(self.source)

    def test_cli_writes_inventory_and_rejects_archive_options(self):
        self.write(exefs())
        output = self.root / 'inventory.json'
        with redirect_stdout(io.StringIO()):
            self.assertEqual(main(['probe', '--exefs', str(self.source), '--output', str(output)]), 0)
        self.assertEqual(json.loads(output.read_text())['format'], 'ExeFS')
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            main(['probe', '--exefs', str(self.source), '--output', str(output)])
        for options in (['--archive', 'game.nsz'], ['--exefs', str(self.source), '--keys', 'prod.keys']):
            with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                main(['probe', *options])
            self.assertEqual(error.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
