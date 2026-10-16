"""Import the player's NSZ base game and optional v1.1.0 NSZ update.

Only the plaintext Zstandard payload already present in NCZ is read. Encrypted
NCA headers and uncompressed encrypted NCA content are neither read nor decoded.
Formats: nicoboss/nsz docs/formats.md; SciresM/hactool bktr.h; runtime/romfs_build.c.
No game data or cryptographic keys are included with this tool.
"""
import argparse
from bisect import bisect_right
from contextlib import ExitStack
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import time
import uuid

import zstandard
from game_versions import BASE_ID, UPDATE_ID, game_version

CHUNK = 8 << 20
MAX_IMAGE = 16 << 30
MAX_WINDOW = 256 << 20
EMPTY = 0xffffffff


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
            'Not a supported PFS0/NSZ archive')
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


def decompress_frame(view, expected, write):
    require(view.read(0, 4) == bytes.fromhex('28b52ffd'), 'Unknown NCZ compression')
    params = zstandard.get_frame_parameters(view.read(0, min(18, view.size)))
    require(params.window_size <= MAX_WINDOW, 'NCZ compression window exceeds 256 MiB')
    decoder = zstandard.ZstdDecompressor().decompressobj()
    pos, decoded = 0, 0
    # Small compressed reads bound even highly compressible output. Unlike
    # stream_reader, decompressobj exposes EOF, including a missing checksum.
    while pos < view.size and not decoder.eof:
        data = view.read(pos, min(4096, view.size - pos))
        pos += len(data)
        data = decoder.decompress(data)
        require(len(data) <= expected - decoded, 'NCZ frame exceeds declared size')
        decoded += len(data)
        write(data)
    require(decoder.eof and decoded == expected, 'Incomplete NCZ frame')
    require(not decoder.unused_data and pos == view.size, 'Unexpected data after NCZ frame')


def decompress_ncz(view, destination):
    require(view.read(0x4000, 8) == b'NCZSECTN', 'Missing NCZ section header')
    count, = struct.unpack('<Q', view.read(0x4008, 8))
    require(0 < count <= 64, 'Invalid NCZ section count')
    sections = []
    for i in range(count):
        start, size, crypto, _ = struct.unpack('<QQQQ', view.read(0x4010 + i * 64, 32))
        require(size > 0 and 0x4000 <= start < MAX_IMAGE and size <= MAX_IMAGE - start,
                'Unsupported NCZ size')
        require(crypto in (1, 3, 4), 'Unsupported NCZ section type')
        if sections:
            require(start == sum(sections[-1]), 'Non-contiguous NCZ sections')
        sections.append((start, size))
    end = sum(sections[-1])
    pos = 0x4010 + count * 64
    last = time.monotonic()
    with destination.open('xb') as output:
        output.write(bytes(0x4000))
        done = 0x4000
        def write(data):
            nonlocal done, last
            require(len(data) <= end - done, 'NCZ payload exceeds declared size')
            output.write(data)
            done += len(data)
            if time.monotonic() - last >= 5:
                print(f'import: decompress {done / end:.1%}', flush=True)
                last = time.monotonic()
        if view.read(pos, 8) == b'NCZBLOCK':
            version, kind, unused, exponent, blocks, decoded = struct.unpack('<BBBBIQ', view.read(pos + 8, 16))
            require(version == 2 and 14 <= exponent <= 32 and decoded == end - 0x4000,
                    'Unsupported NCZ block header')
            block_size = 1 << exponent
            require(blocks == (decoded + block_size - 1) // block_size, 'Invalid NCZ block count')
            lengths = struct.unpack(f'<{blocks}I', view.read(pos + 24, blocks * 4))
            pos += 24 + blocks * 4
            for length in lengths:
                expected = min(block_size, end - done)
                require(0 < length <= expected, 'Invalid compressed NCZ block size')
                if length < expected:
                    decompress_frame(view.sub(pos, length), expected, write)
                else:
                    for offset in range(0, length, CHUNK):
                        write(view.read(pos + offset, min(CHUNK, length - offset)))
                pos += length
            require(pos == view.size, 'Unexpected data after NCZ blocks')
        else:
            decompress_frame(view.sub(pos, view.size - pos), end - done, write)
        require(done == end, 'Incomplete decompressed NCZ payload')
    return sections


def find_exefs(view, sections):
    found = []
    for start, length in sections:
        data = view.read(start, min(length, 32 << 20))
        pos = 0
        while (pos := data.find(b'PFS0', pos)) >= 0:
            candidate = start + pos
            pos += 4
            try:
                entries = pfs_entries(view, candidate)
                names = {e[0] for e in entries}
                if {'main', 'main.npdm', 'sdk', 'rtld'} <= names:
                    main = next(e for e in entries if e[0] == 'main')
                    header = view.read(main[1], 0x60)
                    require(header[:4] == b'NSO0', 'Invalid main executable')
                    build_id = header[0x40:0x54].hex()
                    game_version(build_id)
                    found.append((entries, build_id))
            except (ValueError, UnicodeError, struct.error):
                continue
    require(len(found) == 1, 'No unique supported game executable found in the NSZ payload')
    return found[0]


def open_program(source, scratch, stack, label):
    source_file = stack.enter_context(source.open('rb'))
    package = View(source_file)
    entries = pfs_entries(package)
    nc = [entry for entry in entries if entry[0].lower().endswith('.ncz')]
    require(nc, 'NSZ contains no compressed program; provide extracted exefs/romfs instead')
    programs = []
    for i, (_, start, size) in enumerate(nc):
        output = scratch / f'{label}-{i}.sections'
        print(f'import: reading {label} archive', flush=True)
        sections = decompress_ncz(package.sub(start, size), output)
        file = stack.enter_context(output.open('rb'))
        view = View(file)
        try:
            files, build_id = find_exefs(view, sections)
            programs.append((view, sections, files, build_id))
        except ValueError:
            continue
    require(len(programs) == 1, f'No unique supported {label} program found')
    return programs[0]


def romfs_header(view, offset):
    h = struct.unpack('<10Q', view.read(offset, 80))
    require(h[0] == 0x50 and 0x50 <= h[9] <= 1 << 20, 'Invalid RomFS header')
    for i in (1, 3, 5, 7):
        require(h[i] >= 0x50 and 0 < h[i + 1] <= 16 << 20 and
                h[i] + h[i + 1] <= view.size - offset, 'Invalid RomFS table range')
    root = struct.unpack('<6I', view.read(offset + h[3], 24))
    require(root[0] == 0 and root[5] == 0, 'Invalid RomFS root')
    child, visited, names = root[2], set(), set()
    while child != EMPTY:
        require(child not in visited and child + 24 <= h[4], 'Invalid RomFS directory chain')
        visited.add(child)
        entry = struct.unpack('<6I', view.read(offset + h[3] + child, 24))
        require(child + 24 + entry[5] <= h[4], 'Invalid RomFS name length')
        names.add(safe_name(view.read(offset + h[3] + child + 24, entry[5])))
        child = entry[1]
    require('region_common' in names, 'Not the game RomFS')
    return h


def find_romfs(view, regions):
    found = []
    for start, size in regions:
        data = view.read(start, min(size, 32 << 20))
        pos = 0
        while (pos := data.find(struct.pack('<Q', 0x50), pos)) >= 0:
            offset = start + pos
            pos += 8
            try:
                h = romfs_header(view, offset)
                found.append((offset, h, start, size))
            except (ValueError, UnicodeError, struct.error):
                continue
    require(len(found) == 1, 'No unique game RomFS found')
    return found[0]


class PatchView:
    def __init__(self, base, update, entries, size):
        self.base, self.update, self.entries, self.size = base, update, entries, size
        self.starts = [e[0] for e in entries]

    def read(self, offset, size):
        require(0 <= offset <= self.size and 0 <= size <= self.size - offset, 'Invalid patch read')
        result = []
        while size:
            i = bisect_right(self.starts, offset) - 1
            virtual, physical, patch = self.entries[i]
            end = self.starts[i + 1] if i + 1 < len(self.starts) else self.size
            count = min(size, end - offset)
            result.append((self.update if patch else self.base).read(physical + offset - virtual, count))
            offset += count
            size -= count
        return b''.join(result)


def update_view(base, update, sections):
    candidates = []
    patch = update.sub(sections[0][0], sum(sections[-1]) - sections[0][0])
    for section_start, section_size in sections[:-1]:
        for offset in range(section_start, section_start + min(section_size, 16 << 20), 0x4000):
            if offset + 0x8000 > update.size:
                continue
            zero, buckets, size = struct.unpack('<IIQ', update.read(offset, 16))
            if zero or not 0 < buckets <= 128 or not base.size // 2 < size <= MAX_IMAGE:
                continue
            try:
                entries = []
                roots = struct.unpack(f'<{buckets}Q', update.read(offset + 16, buckets * 8))
                require(roots[0] == 0 and all(a < b for a, b in zip(roots, roots[1:])),
                        'Invalid BKTR bucket roots')
                for i in range(buckets):
                    block = offset + (i + 1) * 0x4000
                    _, count, bucket_end = struct.unpack('<IIQ', update.read(block, 16))
                    expected_end = roots[i + 1] if i + 1 < buckets else size
                    require(0 < count <= 0x3ff0 // 20 and bucket_end == expected_end,
                            'Invalid BKTR bucket')
                    group = [struct.unpack('<QQI', update.read(block + 16 + j * 20, 20)) for j in range(count)]
                    require(group[0][0] == roots[i] and all(e[0] < bucket_end for e in group),
                            'BKTR bucket root mismatch')
                    entries.extend(group)
                require(entries[0][0] == 0 and all(a[0] < b[0] for a, b in zip(entries, entries[1:])),
                        'Invalid BKTR virtual offsets')
                for i, (virtual, physical, is_patch) in enumerate(entries):
                    end = entries[i + 1][0] if i + 1 < len(entries) else size
                    require(is_patch in (0, 1) and virtual < end <= size, 'Invalid BKTR extent')
                    source = patch if is_patch else base
                    require(physical <= source.size and end - virtual <= source.size - physical,
                            'BKTR extent exceeds its source')
                merged = PatchView(base, patch, entries, size)
                find_romfs(merged, [(0, size)])
                candidates.append(merged)
            except (ValueError, UnicodeError, struct.error):
                continue
    require(len(candidates) == 1, 'No unique valid update relocation table found')
    return candidates[0]


def copy_range(view, offset, size, destination):
    with destination.open('xb') as output:
        while size:
            chunk = view.read(offset, min(size, CHUNK))
            output.write(chunk)
            offset += len(chunk)
            size -= len(chunk)


def extract_romfs(view, offset, header, destination):
    dirs = view.read(offset + header[3], header[4])
    files = view.read(offset + header[7], header[8])
    seen_dirs, seen_files = set(), set()
    total, last = 0, time.monotonic()
    image_size = max(header[i] + header[i + 1] for i in (1, 3, 5, 7))
    def directory(entry_offset, parent, expected_parent=0, depth=0):
        nonlocal total, last, image_size
        require(depth <= 32 and len(seen_dirs) < 10000 and entry_offset not in seen_dirs and
                entry_offset + 24 <= len(dirs), 'Invalid RomFS directory graph')
        seen_dirs.add(entry_offset)
        par, sibling, child, first_file, _, length = struct.unpack_from('<6I', dirs, entry_offset)
        require(par == expected_parent and entry_offset + 24 + length <= len(dirs), 'Invalid RomFS directory')
        name = safe_name(dirs[entry_offset + 24:entry_offset + 24 + length]) if entry_offset else ''
        target = parent / name
        target.mkdir(exist_ok=True)
        names = set()
        current = first_file
        while current != EMPTY:
            require(len(seen_files) < 100000 and current not in seen_files and current + 32 <= len(files),
                    'Invalid RomFS file graph')
            seen_files.add(current)
            par, next_file, data, size, _, length = struct.unpack_from('<IIQQII', files, current)
            require(par == entry_offset and current + 32 + length <= len(files), 'Invalid RomFS file entry')
            name = safe_name(files[current + 32:current + 32 + length])
            require(name.casefold() not in names, 'Duplicate RomFS filename')
            names.add(name.casefold())
            copy_range(view, offset + header[9] + data, size, target / name)
            image_size = max(image_size, header[9] + data + size)
            total += size
            current = next_file
            if time.monotonic() - last >= 5:
                print(f'import: extracted {len(seen_files)} files', flush=True)
                last = time.monotonic()
        current = child
        while current != EMPTY:
            require(current + 24 <= len(dirs), 'Invalid RomFS child')
            length = struct.unpack_from('<I', dirs, current + 20)[0]
            require(current + 24 + length <= len(dirs), 'Invalid RomFS child name')
            name = safe_name(dirs[current + 24:current + 24 + length])
            require(name.casefold() not in names, 'Duplicate RomFS child name')
            names.add(name.casefold())
            directory(current, target, entry_offset, depth + 1)
            current = struct.unpack_from('<I', dirs, current + 4)[0]
    directory(0, destination)
    print('import: writing game data image', flush=True)
    copy_range(view, offset, image_size, destination.parent / 'romfs.bin')
    return len(seen_files), total


def import_game(base_path, update_path, output, replace=False):
    base_path = base_path.resolve(strict=True)
    update_path = update_path.resolve(strict=True) if update_path else None
    require(not output.is_symlink(), 'Output cannot be a symbolic link')
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    require(not base_path.is_relative_to(output.resolve()) and
            (not update_path or not update_path.is_relative_to(output.resolve())),
            'Source archives must be outside the output directory')
    allowed = {'exefs', 'romfs', 'romfs.bin', 'lasgame.dll', 'lasgame.dll.id', 'lasgame.so',
               'lasgame.so.id', 'lasgame.dylib', 'lasgame.dylib.id', '.gamebuild', 'import-manifest.json'}
    if output.exists():
        require(replace and output.is_dir(), 'Output exists; select a new directory or use --replace')
        require(all(p.name in allowed and not p.is_symlink() for p in output.iterdir()),
                'Output contains files not managed by the importer; refusing replacement')
    require(shutil.disk_usage(output.parent).free >= 20 << 30, 'Import needs at least 20 GiB free')
    with tempfile.TemporaryDirectory(prefix='.las-import-', dir=output.parent) as temporary:
        scratch = Path(temporary)
        ready = scratch / 'ready'
        (ready / 'exefs').mkdir(parents=True)
        (ready / 'romfs').mkdir()
        with ExitStack() as stack:
            base, sections, exefs, build_id = open_program(base_path, scratch, stack, 'base')
            require(build_id == BASE_ID, 'The base archive must contain game v1.0.0')
            offset, header, section_start, section_size = find_romfs(base, sections)
            romfs = base
            executable = base
            if update_path:
                update, update_sections, exefs, build_id = open_program(update_path, scratch, stack, 'update')
                require(build_id == UPDATE_ID, 'Only the verified v1.1.0 update is supported')
                print('import: applying update extents', flush=True)
                romfs = update_view(base.sub(section_start, section_size), update, update_sections)
                offset, header, _, _ = find_romfs(romfs, [(0, romfs.size)])
                executable = update
            for name, start, size in exefs:
                copy_range(executable, start, size, ready / 'exefs' / name)
            count, size = extract_romfs(romfs, offset, header, ready / 'romfs')
        manifest = dict(game_version=game_version(build_id), main_build_id=build_id,
                        source_base=base_path.name, source_update=update_path.name if update_path else None,
                        romfs_files=count, romfs_bytes=size)
        (ready / 'import-manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding='utf-8')
        backup = None
        if output.exists():
            backup = output.with_name(output.name + '.previous-' + uuid.uuid4().hex)
            output.rename(backup)
        try:
            ready.rename(output)
        except BaseException:
            if backup:
                backup.rename(output)
            raise
        if backup:
            require(backup.resolve().parent == output.parent.resolve(), 'Unexpected backup location')
            try:
                shutil.rmtree(backup)
            except OSError as error:
                # The new installation is committed. A locked old file must not
                # turn that success into a misleading import failure.
                print(f'Import succeeded; remove the old backup manually: {backup} ({error})',
                      file=sys.stderr, flush=True)
    print(f'import: ready v{manifest["game_version"]}, {count} files', flush=True)
    return manifest


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        if stream is not None and hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True, type=Path)
    parser.add_argument('--update', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--replace', action='store_true')
    args = parser.parse_args(argv)
    try:
        import_game(args.base, args.update, args.output, args.replace)
    except (ValueError, OSError, zstandard.ZstdError) as error:
        print(f'Import failed: {error}', file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
