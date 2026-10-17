"""Executable inventory and installation boundaries, using synthetic archives only."""
from contextlib import redirect_stdout
import io
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest

from adapter import main
from containers import View
from game_catalog import get_game
from import_game import inspect_package
from nca import Keyset
from probe_archive import executable_candidates, probe_archive
from test_import_game import ncz, pfs_files


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


def package(files=None, metadata=True, duplicate=False):
    payload = ncz(pfs_files(files or exefs()))
    entries = {'program.ncz': payload}
    if duplicate: entries['second.ncz'] = payload
    if metadata:
        entries['content.cnmt.xml'] = (b'<ContentMeta><Type>Application</Type><Id>0x0100123400000000</Id>'
                                      b'<Version>0</Version></ContentMeta>')
    return pfs_files(entries)


class ProbeTest(unittest.TestCase):
    def run_probe(self, contents, **options):
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / 'untrusted-name.nsz'
            archive.write_bytes(contents)
            return probe_archive(archive, **options)

    def test_unknown_metadata_can_be_inventoried_but_not_installed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / 'claims-LAS-1.0.0.nsz'
            archive.write_bytes(package())
            output = root / 'exefs'
            result = probe_archive(archive, exefs=output)
            self.assertEqual(result['metadata']['application_id'], '0100123400000000')
            self.assertEqual(result['metadata']['metadata_source'], 'XML (unverified)')
            self.assertEqual(result['adapter'], {'status': 'unregistered', 'installable': False})
            self.assertEqual(result['modules'][0]['build_id'], '42' * 32)
            self.assertEqual(result['modules'][0]['image_size'], 0x3000)
            self.assertEqual({p.name: p.read_bytes() for p in output.iterdir()}, exefs())
            with self.assertRaisesRegex(ValueError, 'No supported game adapter'):
                inspect_package(archive)
            with self.assertRaisesRegex(ValueError, 'already exists'):
                probe_archive(archive, exefs=output)
            self.assertFalse((root / '.installations').exists())

    def test_metadata_free_inventory(self):
        result = self.run_probe(package(metadata=False))
        self.assertIsNone(result['metadata'])
        self.assertFalse(result['adapter']['installable'])

    def test_executable_crosses_ncz_counter_ranges(self):
        payload = pfs_files(exefs())
        sections = [(0x4000, 512), (0x4200, len(payload) - 512)]
        result = self.run_probe(pfs_files({'program.ncz': ncz(payload, sections=sections)}))
        self.assertEqual(result['modules'][0]['build_id'], '42' * 32)

    def test_exact_modules_and_unreviewed_modules(self):
        for extra in (False, True):
            result = self.run_probe(package(exefs(known=True, extra=extra), metadata=False))
            self.assertEqual(result['adapter']['status'], 'module-mismatch' if extra else 'matched')
            self.assertFalse(result['adapter']['installable'])
        files = exefs(known=True)
        files['sdk'] = nso('43' * 32)
        result = self.run_probe(package(files, metadata=False))
        self.assertEqual(result['adapter']['mismatched_modules'], ['sdk'])

    def test_ambiguous_programs_leave_no_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'exefs'
            with self.assertRaisesRegex(ValueError, 'No unique executable'):
                self.run_probe(package(duplicate=True), exefs=output)
            self.assertFalse(output.exists())

    def test_invalid_nso_bounds_and_overlaps(self):
        for offset, values in ((0x10, (0x10b, 0, 4)), (0x20, (0x100, 0x1000, 4)),
                               (0x20, (0x104, 0, 4)), (0x30, (0x108, 0x80000000, 4))):
            files = exefs()
            invalid = bytearray(files['main'])
            struct.pack_into('<III', invalid, offset, *values)
            files['main'] = invalid
            with self.subTest(offset=offset, values=values), self.assertRaises(ValueError):
                self.run_probe(package(files))

    def test_pfs_cannot_escape_declared_region(self):
        data = pfs_files(exefs())
        self.assertEqual(executable_candidates(View(io.BytesIO(data)), [(0, len(data) - 1)]), [])

    def test_cli_writes_new_json_without_changing_catalog(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive, output = root / 'unknown.nsz', root / 'inventory.json'
            archive.write_bytes(package())
            with redirect_stdout(io.StringIO()):
                self.assertEqual(main(['probe', '--archive', str(archive), '--output', str(output)]), 0)
            self.assertEqual(json.loads(output.read_text())['adapter']['status'], 'unregistered')

    def test_encrypted_nsp_uses_supplied_keys_and_content_hash(self):
        from test_nsp_import import nca, pfs_section, cnmt, HEADER, AREA
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            keyfile = root / 'synthetic.keys'
            keyfile.write_text(f'header_key = {HEADER.hex()}\nkey_area_key_application_06 = {AREA.hex()}\n')
            program = nca([pfs_section(exefs())])
            metadata = nca([pfs_section({'Application.cnmt': cnmt(program)})], content_type=1)
            archive = root / 'encrypted.nsp'
            archive.write_bytes(pfs_files({hashlib.sha256(program).hexdigest()[:32] + '.nca': program,
                                          'metadata.cnmt.nca': metadata}))
            result = probe_archive(archive, Keyset(keyfile))
            self.assertEqual(result['format'], 'NSP')
            self.assertEqual(result['metadata']['metadata_source'], 'CNMT')
            self.assertEqual(result['modules'][0]['build_id'], '42' * 32)
            self.assertFalse(result['adapter']['installable'])


if __name__ == '__main__':
    unittest.main()
