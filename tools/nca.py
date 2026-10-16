"""Bounded NCA2/3 reads using keys supplied by the player.

Layout references: SciresM/hactool nca.h, bktr.h and pfs0.h; nicoboss/nsz
Fs/Ticket.py. AES is provided by cryptography, not implemented here. No keys
are bundled, inferred from filenames, logged, or written to import manifests.
"""
from bisect import bisect_right
import hashlib
from pathlib import Path
import re
import struct

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

MAX_NCA = 16 << 30


def check(ok, message):
    if not ok:
        raise ValueError(message)


def aes(key, mode, data):
    cipher = Cipher(algorithms.AES(key), mode).decryptor()
    return cipher.update(data) + cipher.finalize()


def xts(key, data, first_sector=0):
    check(len(data) % 512 == 0, 'Invalid AES-XTS sector length')
    # Nintendo's sector tweak uses the big-endian sector number (hactool aes.c).
    return b''.join(aes(key, modes.XTS((first_sector + i // 512).to_bytes(16, 'big')),
                       data[i:i + 512]) for i in range(0, len(data), 512))


class Slice:
    def __init__(self, source, start, size):
        check(0 <= start <= source.size and 0 <= size <= source.size - start, 'Invalid content section range')
        self.source, self.start, self.size = source, start, size

    def read(self, offset, size):
        check(0 <= offset <= self.size and 0 <= size <= self.size - offset, 'Content read exceeds section')
        return self.source.read(self.start + offset, size)

    def sub(self, start, size):
        return Slice(self, start, size)


class Keyset:
    def __init__(self, path=None, title_path=None):
        self._keys, self._titles = {}, {}
        for filename, titles in ((path, False), (title_path, True)):
            if not filename:
                continue
            p = Path(filename)
            check(p.stat().st_size <= 1 << 20, 'Key file exceeds 1 MiB')
            for number, line in enumerate(p.read_text(encoding='utf-8-sig').splitlines(), 1):
                line = line.split('#', 1)[0].strip()
                if not line:
                    continue
                fields = line.split('=', 1)
                check(len(fields) == 2, f'Invalid key file entry on line {number}')
                name, value = (v.strip().lower() for v in fields)
                # Ignore unrelated key families without echoing their names or values.
                wanted = (bool(re.fullmatch('[0-9a-f]{32}', name)) if titles else
                          bool(re.fullmatch(r'header_key|key_area_key_(application|ocean|system)_[0-9a-f]{2}|titlekek_[0-9a-f]{2}', name)))
                if not wanted:
                    continue
                size = 32 if name == 'header_key' else 16
                check(bool(re.fullmatch(f'[0-9a-f]{{{size * 2}}}', value)),
                      f'Invalid key length or encoding on line {number}')
                target = self._titles if titles else self._keys
                decoded = bytes.fromhex(value)
                check(name not in target or target[name] == decoded, f'Conflicting key entry on line {number}')
                target[name] = decoded

    def get(self, name):
        check(name in self._keys, f'Missing {name}; select a prod.keys file containing this derived key')
        return self._keys[name]

    def title_key(self, rights, revision, tickets):
        encrypted = self._titles.get(rights.hex()) or tickets.get(rights)
        check(encrypted is not None, 'Missing common ticket/title key; supply title.keys for this content')
        return aes(self.get(f'titlekek_{revision:02x}'), modes.ECB(), encrypted)


def ticket_key(view):
    # Signature type is big-endian; the signature body is padded to 0x40.
    kind, = struct.unpack('>I', view.read(0, 4))
    sizes = {0x10000: 0x200, 0x10001: 0x100, 0x10002: 0x3c,
             0x10003: 0x200, 0x10004: 0x100, 0x10005: 0x3c}
    check(kind in sizes, 'Unsupported ticket signature type')
    body = (4 + sizes[kind] + 63) & ~63
    check(view.read(body + 0x141, 1) == b'\0',
          'Personalized tickets are not supported; supply a title.keys file instead')
    return view.read(body + 0x160, 16), view.read(body + 0x40, 16)


class Section:
    def __init__(self, owner, start, size, header):
        self.owner, self.start, self.size, self.header = owner, start, size, header
        self.crypto = header[4]
        check(self.crypto in (1, 2, 3, 4), 'Unsupported NCA encryption mode')
        self.prefix = header[0x140:0x148][::-1]
        self.counters = None

    def ctr_read(self, offset, size, prefix):
        absolute = self.start + offset
        skip = absolute % 16
        ctr = prefix + (absolute // 16).to_bytes(8, 'big')
        return aes(self.owner.data_key(), modes.CTR(ctr),
                   self.owner.raw.read(absolute - skip, size + skip))[skip:]

    def load_counters(self):
        reloc, reloc_size, rmagic = struct.unpack_from('<QQ4s', self.header, 0x100)
        sub, sub_size, smagic = struct.unpack_from('<QQ4s', self.header, 0x120)
        check(rmagic == smagic == b'BKTR' and 0 < reloc < sub and
              reloc + reloc_size == sub and sub + sub_size == self.size and
              0x8000 <= sub_size <= 16 << 20, 'Invalid BKTR encryption table bounds')
        data = self.ctr_read(sub, sub_size, self.prefix)
        zero, count, end = struct.unpack_from('<IIQ', data)
        check(zero == 0 and 0 < count <= 128 and end == sub and (count + 1) * 0x4000 <= len(data),
              'Invalid BKTR encryption root')
        roots = struct.unpack_from(f'<{count}Q', data, 16)
        check(roots[0] == 0 and all(a < b for a, b in zip(roots, roots[1:])), 'Invalid BKTR encryption roots')
        entries = []
        for i in range(count):
            pos = (i + 1) * 0x4000
            _, n, bucket_end = struct.unpack_from('<IIQ', data, pos)
            check(0 < n <= 0x3ff and bucket_end <= sub, 'Invalid BKTR encryption bucket')
            group = [struct.unpack_from('<QII', data, pos + 16 + j * 16) for j in range(n)]
            check(group[0][0] == roots[i] and all(e[0] < reloc for e in group),
                  'Invalid BKTR encryption offsets')
            entries.extend((o, c) for o, _, c in group)
        check(entries[0][0] == 0 and all(a[0] < b[0] for a, b in zip(entries, entries[1:])),
              'Unordered BKTR encryption offsets')
        entries.append((reloc, int.from_bytes(self.prefix[4:], 'big')))
        self.counters = entries
        self.counter_starts = [e[0] for e in entries]

    def read(self, offset, size):
        check(0 <= offset <= self.size and 0 <= size <= self.size - offset, 'Invalid NCA section read')
        if not size:
            return b''
        if self.crypto == 1:
            return self.owner.raw.read(self.start + offset, size)
        if self.crypto == 2:
            first = offset // 512 * 512
            end = (offset + size + 511) // 512 * 512
            return xts(self.owner.area_keys()[:32], self.owner.raw.read(self.start + first, end - first),
                       first // 512)[offset - first:offset - first + size]
        if self.crypto == 3:
            return self.ctr_read(offset, size, self.prefix)
        if self.counters is None:
            self.load_counters()
        result = []
        while size:
            i = bisect_right(self.counter_starts, offset) - 1
            end = self.counter_starts[i + 1] if i + 1 < len(self.counters) else self.size
            n = min(size, end - offset)
            prefix = self.prefix[:4] + self.counters[i][1].to_bytes(4, 'big')
            result.append(self.ctr_read(offset, n, prefix))
            offset += n
            size -= n
        return b''.join(result)

    def sub(self, offset, size):
        return Slice(self, offset, size)

    def pfs(self):
        check(self.header[2:4] == b'\x01\x02', 'NCA section is not PFS0')
        offset, size = struct.unpack_from('<QQ', self.header, 0x40)
        # Validate the PFS hash table before consuming metadata. Program content is
        # additionally checked against the CNMT's SHA-256 at import time.
        block, layers, table, table_size = struct.unpack_from('<IIQQ', self.header, 0x28)
        check(layers == 2 and 512 <= block <= 64 << 20 and block & (block - 1) == 0 and
              0 < table_size <= 16 << 20 and table + table_size <= offset and size > 0,
              'Invalid NCA PFS0 hash layout')
        hashes = self.read(table, table_size)
        check(hashlib.sha256(hashes).digest() == self.header[8:40], 'NCA PFS0 hash table mismatch (wrong key or damaged content)')
        view = self.sub(offset, size)
        check(((size + block - 1) // block) * 32 <= len(hashes), 'NCA PFS0 hash table is too short')
        for i, start in enumerate(range(0, size, block)):
            check(hashlib.sha256(view.read(start, min(block, size - start))).digest() == hashes[i * 32:(i + 1) * 32],
                  'NCA PFS0 content hash mismatch (wrong key or damaged content)')
        return view


class Nca:
    def __init__(self, raw, keys, tickets=None):
        self.raw, self.keys, self.tickets = raw, keys, tickets or {}
        header = raw.read(0, 0xc00)
        if header[0x200:0x204] not in (b'NCA2', b'NCA3'):
            key = keys.get('header_key')
            first = xts(key, header[:0x400])
            check(first[0x200:0x204] in (b'NCA2', b'NCA3'), 'NCA header could not be decrypted; check prod.keys')
            header = (xts(key, header) if first[0x200:0x204] == b'NCA3' else
                      first + b''.join(xts(key, header[i:i + 512]) for i in range(0x400, 0xc00, 512)))
        self.header = header
        self.content_type = header[0x205]
        self.size, self.title_id = struct.unpack_from('<QQ', header, 0x208)
        check(0xc00 <= self.size == raw.size <= MAX_NCA, 'NCA length does not match its header')
        self.revision = max(0, max(header[0x206], header[0x220]) - 1)
        self.rights = header[0x230:0x240]
        self._area, self._key = None, None
        self.sections = []
        self.relocation = None
        for i in range(4):
            start, end = struct.unpack_from('<II', header, 0x240 + i * 16)
            if start == end == 0:
                continue
            start, end = start * 512, end * 512
            check(0xc00 <= start < end <= self.size, 'Invalid NCA section bounds')
            fs = header[0x400 + i * 512:0x600 + i * 512]
            check(hashlib.sha256(fs).digest() == header[0x280 + i * 32:0x2a0 + i * 32], 'NCA section header hash mismatch')
            section = Section(self, start, end - start, fs)
            self.sections.append(section)
            if section.crypto == 4:
                check(self.relocation is None, 'Multiple patch RomFS sections are unsupported')
                reloc, reloc_size = struct.unpack_from('<QQ', fs, 0x100)
                self.relocation = (start, end - start, reloc, reloc_size)
        self.sections.sort(key=lambda s: s.start)
        check(self.sections and all(a.start + a.size <= b.start for a, b in zip(self.sections, self.sections[1:])),
              'Missing or overlapping NCA sections')
        self.regions = [(s.start, s.size) for s in self.sections]

    def area_keys(self):
        if self._area is None:
            index = self.header[0x207]
            check(index < 3, 'Unsupported NCA key area index')
            family = ('application', 'ocean', 'system')[index]
            self._area = aes(self.keys.get(f'key_area_key_{family}_{self.revision:02x}'), modes.ECB(), self.header[0x300:0x340])
        return self._area

    def data_key(self):
        if self._key is None:
            self._key = (self.keys.title_key(self.rights, self.revision, self.tickets) if any(self.rights)
                         else self.area_keys()[32:48])
        return self._key

    def read(self, offset, size):
        check(0 <= offset <= self.size and 0 <= size <= self.size - offset, 'Invalid NCA read')
        if not size:
            return b''
        for s in self.sections:
            if s.start <= offset and offset + size <= s.start + s.size:
                return s.read(offset - s.start, size)
        check(offset + size <= len(self.header), 'NCA read crosses section boundaries')
        return self.header[offset:offset + size]

    def sub(self, start, size):
        return Slice(self, start, size)


def parse_cnmt(data):
    check(0x20 <= len(data) <= 4 << 20, 'Invalid CNMT size')
    title, version, kind, _, ext, count, metas = struct.unpack_from('<QIBBHHH', data)
    check(kind in (0x80, 0x81) and 0 < count <= 4096 and ext >= 8 and
          0x20 + ext + count * 56 + metas * 16 <= len(data), 'Unsupported or truncated application metadata')
    application = struct.unpack_from('<Q', data, 0x20)[0] if kind == 0x81 else title
    contents = []
    for i in range(count):
        record = data[0x20 + ext + i * 56:0x20 + ext + (i + 1) * 56]
        contents.append(dict(sha256=record[:32].hex(), id=record[32:48].hex(),
                             size=int.from_bytes(record[48:54], 'little'), type=record[54], index=record[55]))
    check(len({c['id'] for c in contents}) == len(contents), 'Duplicate CNMT content ID')
    return dict(title_id=f'{title:016x}', application_id=f'{application:016x}',
                content_version=version, role='update' if kind == 0x81 else 'base', contents=contents,
                metadata_source='CNMT')
