"""Bounded read-only PFS0 and XCI/HFS0 containers.

Layout references: SciresM/hactool xci.h, hfs0.h and xci.c;
nicoboss/nsz docs/formats.md. Container hashes detect damage, not authenticity.
"""
import hashlib
import struct


def require(condition, message):
    if not condition:
        raise ValueError(message)


def safe_name(raw):
    name = raw.decode('utf-8')
    require(name and name not in ('.', '..') and len(raw) <= 255,
            'Invalid archive filename')
    require(not any(c in name for c in '/\\:\0<>"|?*') and
            not any(ord(c) < 32 for c in name) and name[-1] not in ' .',
            'Unsafe archive filename')
    stem = name.split('.')[0].upper()
    require(stem not in {'CON', 'PRN', 'AUX', 'NUL'} and
            not (len(stem) == 4 and stem[:3] in ('COM', 'LPT') and stem[3].isdigit()),
            'Reserved archive filename')
    return name


class View:
    def __init__(self, file, start=0, size=None):
        self.file, self.start = file, start
        if size is None:
            file.seek(0, 2)
            size = file.tell() - start
        self.size = size

    def read(self, offset, size):
        require(0 <= offset <= self.size and 0 <= size <= self.size - offset,
                'Archive range exceeds its container')
        self.file.seek(self.start + offset)
        result = self.file.read(size)
        require(len(result) == size, 'Truncated archive')
        return result

    def sub(self, start, size):
        require(0 <= start <= self.size and 0 <= size <= self.size - start,
                'Invalid archive section')
        return View(self.file, self.start + start, size)


def pfs_entries(view, offset=0):
    magic, count, size, _ = struct.unpack('<4sIII', view.read(offset, 16))
    require(magic == b'PFS0' and 0 < count <= 4096 and size <= 1 << 20,
            'Not a supported NSP/NSZ archive (expected PFS0)')
    table = view.read(offset + 16, count * 24)
    strings = view.read(offset + 16 + count * 24, size)
    data_start = offset + 16 + count * 24 + size
    entries, names, ranges = [], set(), []
    for i in range(count):
        start, length, n, _ = struct.unpack_from('<QQII', table, i * 24)
        require(n < len(strings) and b'\0' in strings[n:], 'Invalid string table')
        name = safe_name(strings[n:].split(b'\0', 1)[0])
        require(name.casefold() not in names, 'Duplicate archive filename')
        names.add(name.casefold())
        absolute = data_start + start
        require(absolute <= view.size and length <= view.size - absolute,
                'PFS0 file exceeds its container')
        entries.append((name, absolute, length))
        if length:
            ranges.append((absolute, absolute + length))
    ranges.sort()
    require(all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])), 'Overlapping PFS0 files')
    return entries


def hfs_entries(view):
    magic, count, names_size, reserved = struct.unpack('<4sIII', view.read(0, 16))
    require(magic == b'HFS0' and 0 < count <= 4096 and names_size <= 1 << 20 and reserved == 0,
            'Invalid HFS0 partition header')
    table = view.read(16, count * 64)
    strings = view.read(16 + count * 64, names_size)
    header_size = 16 + count * 64 + names_size
    entries, names, ranges = [], set(), []
    for i in range(count):
        offset, size, name_at, hashed_size, unused, digest = struct.unpack_from('<QQIIQ32s', table, i * 64)
        require(name_at < len(strings) and b'\0' in strings[name_at:], 'Invalid HFS0 name table')
        name = safe_name(strings[name_at:].split(b'\0', 1)[0])
        require(name.casefold() not in names, 'Duplicate HFS0 filename')
        names.add(name.casefold())
        start = header_size + offset
        require(start <= view.size and size <= view.size - start, 'HFS0 file exceeds its partition')
        require(unused == 0 and 0 < hashed_size <= min(size, 16 << 20), 'Invalid HFS0 hashed region')
        require(hashlib.sha256(view.read(start, hashed_size)).digest() == digest, 'HFS0 file hash mismatch')
        entries.append((name, start, size))
        if size:
            ranges.append((start, start + size))
    ranges.sort()
    require(all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])), 'Overlapping HFS0 files')
    return entries, header_size


def open_container(view):
    """Return a content view, bounded entries, and detected format (never trust the extension)."""
    if view.read(0, 4) == b'PFS0':
        entries = pfs_entries(view)
        return view, entries, 'NSZ' if any(n.lower().endswith('.ncz') for n, _, _ in entries) else 'NSP'
    require(view.size >= 0x200 and view.read(0x100, 4) == b'HEAD',
            'Unsupported container; expected NSP/NSZ or XCI/XCZ')
    header = view.read(0, 0x200)
    offset, hashed_size = struct.unpack_from('<QQ', header, 0x130)
    require(0x200 <= offset <= view.size and 16 <= hashed_size <= 1 << 20 and
            hashed_size <= view.size - offset, 'Invalid XCI root partition range')
    root_header = view.read(offset, hashed_size)
    expected = header[0x140:0x160]
    # Global gamecards hash the header directly; China-compatible cards append 0x01.
    # The encrypted gamecard-info block and certificate are not needed for content extraction.
    require(expected in (hashlib.sha256(root_header).digest(), hashlib.sha256(root_header + b'\x01').digest()),
            'XCI root header hash mismatch')
    root = view.sub(offset, view.size - offset)
    partitions, actual_header_size = hfs_entries(root)
    require(actual_header_size <= hashed_size and len(partitions) <= 4 and
            all(n in ('update', 'normal', 'secure', 'logo') for n, _, _ in partitions),
            'Unsupported XCI partition table')
    secure = [entry for entry in partitions if entry[0] == 'secure']
    require(len(secure) == 1, 'XCI has no unique secure content partition')
    _, start, size = secure[0]
    content = root.sub(start, size)
    entries, _ = hfs_entries(content)
    # The gamecard's "update" partition carries console firmware, not a game patch.
    # Only secure content reaches the application metadata reader.
    compressed = any(n.lower().endswith('.ncz') for n, _, _ in entries)
    return content, entries, 'XCZ' if compressed else 'XCI'
