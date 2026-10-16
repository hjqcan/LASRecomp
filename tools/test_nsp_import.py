"""Synthetic encrypted NSP/NCA fixtures; no console keys or game data."""
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from contextlib import redirect_stdout
from types import SimpleNamespace
from unittest.mock import patch

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from import_game import View, import_game, inspect_package, pfs_entries
from nca import Keyset, Nca, ticket_key
from test_import_game import pfs_files, synthetic_romfs, synthetic_package, synthetic_nso
from game_catalog import get_game
from game_versions import BASE_ID, UPDATE_ID

# Arbitrary test bytes, deliberately unrelated to any real system's keys.
HEADER = bytes(range(32))
AREA = bytes(range(32, 48))
DATA = bytes(range(48, 64))
TITLE_KEK = bytes(range(64, 80))
RIGHTS = bytes(range(80, 96))
AREA_PLAIN = bytes(range(96, 128)) + DATA + bytes(16)
APP = 0x01006bb00c6f0000


def encrypt(key, mode, data):
    cipher = Cipher(algorithms.AES(key), mode).encryptor()
    return cipher.update(data) + cipher.finalize()


def enc_xts(key, data, first=0):
    # Independent fixture generation: Nintendo's sector number is big-endian.
    return b''.join(encrypt(key, modes.XTS((first + i // 512).to_bytes(16, 'big')),
                           data[i:i + 512]) for i in range(0, len(data), 512))


def enc_ctr(data, absolute, counter):
    prefix = struct.pack('>II', 0x10203040, counter)
    skip = absolute % 16
    return encrypt(DATA, modes.CTR(prefix + (absolute // 16).to_bytes(8, 'big')), bytes(skip) + data)[skip:]


def pfs_section(files, crypto=3):
    pfs = pfs_files(files)
    hashes = b''.join(hashlib.sha256(pfs[i:i + 4096]).digest() for i in range(0, len(pfs), 4096))
    offset = (len(hashes) + 511) // 512 * 512
    data = hashes + bytes(offset - len(hashes)) + pfs
    data += bytes((-len(data)) % 512)
    fs = bytearray(512)
    fs[2:5] = bytes((1, 2, crypto))
    fs[8:40] = hashlib.sha256(hashes).digest()
    struct.pack_into('<IIQQQQ', fs, 0x28, 4096, 2, 0, len(hashes), offset, len(pfs))
    return data, fs


def program_sections(updated=False, crypto=3, build=None):
    main = bytearray(0x60)
    main[:4] = b'NSO0'
    main[0x40:0x54] = bytes.fromhex(build or (UPDATE_ID if updated else BASE_ID))
    sdk = synthetic_nso(get_game().releases[0].modules[1].build_id)
    code = pfs_section({'main': main, 'main.npdm': b'fixture', 'sdk': sdk, 'rtld': b'fixture rtld'}, crypto)
    fs = bytearray(512)
    fs[2:5] = bytes((0, 3, 4 if updated else crypto))
    if not updated:
        return [(bytes(synthetic_romfs()), fs), code]
    data = bytearray(0x18000)
    data[:8] = b'changed!'
    # Two relocation buckets, then one encryption bucket with two counter ranges.
    struct.pack_into('<IIQQQ', data, 0x4000, 0, 2, 0x10000, 0, 0x1000)
    struct.pack_into('<IIQ', data, 0x8000, 0, 1, 0x1000)
    struct.pack_into('<QQI', data, 0x8010, 0, 0, 0)
    struct.pack_into('<IIQ', data, 0xc000, 0, 2, 0x10000)
    struct.pack_into('<QQI', data, 0xc010, 0x1000, 0, 1)
    struct.pack_into('<QQI', data, 0xc024, 0x1008, 0x1008, 0)
    struct.pack_into('<IIQQ', data, 0x10000, 0, 1, 0x10000, 0)
    struct.pack_into('<IIQ', data, 0x14000, 0, 2, 0x10000)
    struct.pack_into('<QII', data, 0x14010, 0, 0, 10)
    struct.pack_into('<QII', data, 0x14020, 0x2000, 0, 11)
    struct.pack_into('<QQ4sIII', fs, 0x100, 0x4000, 0xc000, b'BKTR', 1, 3, 0)
    struct.pack_into('<QQ4sIII', fs, 0x120, 0x10000, 0x8000, b'BKTR', 1, 2, 0)
    return [(bytes(data), fs), code]


def nca(sections, content_type=0, title=APP, rights=False, plaintext_header=False, nca2=False):
    h = bytearray(0xc00)
    h[0x200:0x204] = b'NCA2' if nca2 else b'NCA3'
    h[0x205] = content_type
    h[0x206] = h[0x220] = 7
    struct.pack_into('<Q', h, 0x210, title)
    if rights:
        h[0x230:0x240] = RIGHTS
    h[0x300:0x340] = encrypt(AREA, modes.ECB(), AREA_PLAIN)
    body = bytearray(0x4000)
    for i, (data, original_fs) in enumerate(sections):
        fs = bytearray(original_fs)
        struct.pack_into('<II', fs, 0x140, 3, 0x10203040)
        start = len(body)
        struct.pack_into('<II', h, 0x240 + i * 16, start // 512, (start + len(data)) // 512)
        h[0x400 + i * 512:0x600 + i * 512] = fs
        h[0x280 + i * 32:0x2a0 + i * 32] = hashlib.sha256(fs).digest()
        if fs[4] == 1:
            encrypted = data
        elif fs[4] == 2:
            encrypted = enc_xts(AREA_PLAIN[:32], data)
        elif fs[4] == 3:
            encrypted = enc_ctr(data, start, 3)
        else:
            encrypted = (enc_ctr(data[:0x2000], start, 10) +
                         enc_ctr(data[0x2000:0x4000], start + 0x2000, 11) +
                         enc_ctr(data[0x4000:], start + 0x4000, 3))
        body += encrypted
    struct.pack_into('<Q', h, 0x208, len(body))
    if not plaintext_header:
        h = (enc_xts(HEADER, h[:0x400]) + b''.join(enc_xts(HEADER, h[i:i + 512]) for i in range(0x400, 0xc00, 512))
             if nca2 else enc_xts(HEADER, h))
    body[:0xc00] = h
    return bytes(body)


def cnmt(program, updated=False, title=None, version=None):
    h = bytearray(0x30)
    struct.pack_into('<QIBBHHH', h, 0, title or APP + (0x800 if updated else 0),
                     (131072 if updated else 0) if version is None else version,
                     0x81 if updated else 0x80, 0, 0x10, 1, 0)
    struct.pack_into('<Q', h, 0x20, APP)
    digest = hashlib.sha256(program).digest()
    return bytes(h) + digest + digest[:16] + len(program).to_bytes(6, 'little') + bytes((1, 0))


def ticket(personal=False):
    data = bytearray(0x400)
    struct.pack_into('>I', data, 0, 0x10004)
    data[0x180:0x190] = encrypt(TITLE_KEK, modes.ECB(), DATA)
    data[0x281] = int(personal)
    data[0x2a0:0x2b0] = RIGHTS
    return bytes(data)


def package(updated=False, rights=False, **kwargs):
    program = nca(program_sections(updated, build=kwargs.get('build')), rights=rights, title=APP + (0x800 if updated else 0))
    metadata = cnmt(program, updated, kwargs.get('title'), kwargs.get('version'))
    meta = nca([pfs_section({'Application.cnmt': metadata})], content_type=1)
    files = {hashlib.sha256(program).hexdigest()[:32] + '.nca': program, 'metadata.cnmt.nca': meta}
    if rights:
        files['common.tik'] = ticket()
    return pfs_files(files)


class NspTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.keys_path = self.root / 'fixture.keys'
        self.keys_path.write_text(f'header_key = {HEADER.hex()}\nkey_area_key_application_06 = {AREA.hex()}\n'
                                  f'titlekek_06 = {TITLE_KEK.hex()}\n', encoding='utf-8')
        self.keys = Keyset(self.keys_path)
        self.enterContext(patch('import_game.shutil.disk_usage', return_value=SimpleNamespace(free=100 << 30)))
        self.enterContext(redirect_stdout(io.StringIO()))

    def test_nca_headers_and_unaligned_ctr_reads(self):
        for old in (False, True):
            sections = program_sections()
            image = Nca(View(io.BytesIO(nca(sections, nca2=old))), self.keys)
            section = image.sections[0]
            self.assertEqual(section.read(7, 4091), sections[0][0][7:4098])
            self.assertEqual(image.sub(section.start + 9, 40).read(3, 21), sections[0][0][12:33])
            with self.assertRaises(ValueError):
                section.read(section.size - 1, 2)

    def test_xts_section_uses_relative_sector_tweak(self):
        sections = program_sections(crypto=2)
        image = Nca(View(io.BytesIO(nca(sections))), self.keys)
        self.assertEqual(image.sections[0].read(333, 1400), sections[0][0][333:1733])

    def test_bktr_counter_changes_and_unaligned_reads(self):
        sections = program_sections(updated=True)
        image = Nca(View(io.BytesIO(nca(sections))), self.keys)
        for offset, count in ((1, 37), (0x1ff9, 50), (0x3ff7, 123), (0xffff, 900)):
            self.assertEqual(image.sections[0].read(offset, count), sections[0][0][offset:offset + count])

    def test_base_and_bktr_update_end_to_end(self):
        base, update, output = self.root / 'base.nsp', self.root / 'update.nsp', self.root / 'installed'
        base.write_bytes(package())
        update.write_bytes(package(True, rights=True))
        before = [p.read_bytes() for p in (base, update, self.keys_path)]
        result = import_game(base, update, output, keys_path=self.keys_path)
        self.assertEqual(result['content_version'], 131072)
        self.assertEqual(result['metadata_source'], 'CNMT')
        self.assertEqual(result['main_build_id'], UPDATE_ID)
        self.assertEqual((output / 'romfs/region_common/example.bin').read_bytes(), b'changed!')
        self.assertEqual(before, [p.read_bytes() for p in (base, update, self.keys_path)])
        self.assertNotIn(HEADER.hex(), (output / 'import-manifest.json').read_text())

    def test_nsz_base_with_nsp_update(self):
        base, update = self.root / 'base.nsz', self.root / 'update.nsp'
        base.write_bytes(synthetic_package())
        update.write_bytes(package(True))
        output = self.root / 'installed'
        import_game(base, update, output, keys_path=self.keys_path)
        self.assertEqual((output / 'romfs/region_common/example.bin').read_bytes(), b'changed!')

    def test_inspection_reads_metadata_without_installing(self):
        path = self.root / 'misleading-name.nsp'
        path.write_bytes(package(True))
        result = inspect_package(path, self.keys)
        self.assertEqual((result['format'], result['role'], result['content_version']), ('NSP', 'update', 131072))
        self.assertEqual(len(list(self.root.iterdir())), 2)

    def test_missing_and_wrong_keys_do_not_disclose_secrets(self):
        path = self.root / 'base.nsp'
        path.write_bytes(package())
        with self.assertRaisesRegex(ValueError, 'Missing header_key'):
            inspect_package(path)
        self.keys_path.write_text('header_key = ' + bytes(range(1, 33)).hex())
        with self.assertRaisesRegex(ValueError, 'could not be decrypted') as raised:
            inspect_package(path, Keyset(self.keys_path))
        self.assertNotIn(bytes(range(1, 33)).hex(), str(raised.exception))

    def test_wrong_game_wrong_version_and_wrong_role(self):
        path = self.root / 'base.nsp'
        for kwargs, message in (({'title': APP + 0x10000}, 'not Link'), ({'version': 999}, 'Unsupported content')):
            path.write_bytes(package(**kwargs))
            with self.assertRaisesRegex(ValueError, message):
                inspect_package(path, self.keys)
        path.write_bytes(package(True))
        with self.assertRaisesRegex(ValueError, 'Selected base'):
            import_game(path, None, self.root / 'out', keys_path=self.keys_path)

    def test_corrupt_program_hash_preserves_installed_files(self):
        path, output = self.root / 'base.nsp', self.root / 'out'
        data = bytearray(package())
        path.write_bytes(data)
        import_game(path, None, output, keys_path=self.keys_path)
        entry = pfs_entries(View(io.BytesIO(data)))[0]
        data[entry[1] + 0x4010] ^= 1
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'SHA-256 mismatch'):
            import_game(path, None, output, replace=True, keys_path=self.keys_path)
        self.assertEqual((output / 'romfs/region_common/example.bin').read_bytes(), b'original')

    def test_corrupt_section_header_and_metadata_data_fail(self):
        data = bytearray(nca(program_sections(), plaintext_header=True))
        data[0x405] ^= 1
        with self.assertRaisesRegex(ValueError, 'header hash'):
            Nca(View(io.BytesIO(data)), self.keys)
        data = bytearray(nca([pfs_section({'meta.cnmt': b'fixture'})], content_type=1))
        data[0x4210] ^= 1
        image = Nca(View(io.BytesIO(data)), self.keys)
        with self.assertRaisesRegex(ValueError, 'content hash'):
            image.sections[0].pfs()

    def test_common_ticket_and_explicit_title_keys(self):
        rights, wrapped = ticket_key(View(io.BytesIO(ticket())))
        self.assertEqual(self.keys.title_key(rights, 6, {rights: wrapped}), DATA)
        path = self.root / 'title.keys'
        path.write_text(RIGHTS.hex() + ' = ' + wrapped.hex())
        self.assertEqual(Keyset(self.keys_path, path).title_key(RIGHTS, 6, {}), DATA)
        with self.assertRaisesRegex(ValueError, 'Personalized'):
            ticket_key(View(io.BytesIO(ticket(True))))

    def test_unknown_build_and_missing_program_are_rejected(self):
        path = self.root / 'base.nsp'
        path.write_bytes(package(build='ab' * 20))
        with self.assertRaisesRegex(ValueError, 'No unique'):
            import_game(path, None, self.root / 'out', keys_path=self.keys_path)
        raw = package()
        v = View(io.BytesIO(raw))
        files = {n: v.read(o, size) for n, o, size in pfs_entries(v) if n.endswith('.cnmt.nca')}
        path.write_bytes(pfs_files(files))
        with self.assertRaisesRegex(ValueError, 'missing'):
            import_game(path, None, self.root / 'out', keys_path=self.keys_path)


if __name__ == '__main__':
    unittest.main()
