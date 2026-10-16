"""Inspect/import the player's NSP or NSZ base game and optional v1.1.0 update.

NCZ uses its plaintext Zstandard payload. Encrypted NCA content requires the
player's own local key files; common tickets can supply wrapped title keys.
Formats: nicoboss/nsz docs/formats.md; SciresM/hactool bktr.h; runtime/romfs_build.c.
No game data or cryptographic keys are included with this tool.
"""
import argparse
import hashlib
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
import xml.etree.ElementTree as ET

import zstandard
from game_versions import BASE_ID, UPDATE_ID, game_version
from nca import Keyset, Nca, parse_cnmt, ticket_key
from containers import View, safe_name, pfs_entries, open_container

CHUNK = 8 << 20
MAX_IMAGE = 16 << 30
MAX_WINDOW = 256 << 20
EMPTY = 0xffffffff


def require(condition, message):
    if not condition:
        raise ValueError(message)


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
    require(len(found) == 1, 'No unique supported game executable found in the archive payload')
    return found[0]


APP_ID = '01006bb00c6f0000'
UPDATE_TITLE_ID = '01006bb00c6f0800'


def validate_metadata(metadata, role=None):
    if metadata is None:
        return
    check_role = metadata['role']
    require(metadata['application_id'] == APP_ID and metadata['title_id'] ==
            (APP_ID if check_role == 'base' else UPDATE_TITLE_ID),
            'The selected archive is not Link\'s Awakening')
    if role:
        require(check_role == role, f'Selected {role} archive contains an {check_role}; choose the correct file')
    require(metadata['content_version'] == (0 if check_role == 'base' else 131072),
            'Unsupported content version; use the v1.0.0 base and optional v1.1.0 update')


def package_info(package, entries, keys):
    """Inspect metadata without decompressing the multi-gigabyte NCZ program."""
    has_ncz = any(name.lower().endswith('.ncz') for name, _, _ in entries)
    tickets = {}
    if keys._keys or keys._titles:
        for name, offset, size in entries:
            if name.lower().endswith('.tik'):
                require(size <= 64 << 10, 'Ticket is too large')
                try:
                    rights, key = ticket_key(package.sub(offset, size))
                except ValueError as error:
                    if 'Personalized tickets' in str(error):
                        continue  # an explicitly supplied title.keys can still handle it
                    raise
                require(rights not in tickets or tickets[rights] == key, 'Conflicting tickets')
                tickets[rights] = key
    records, ncas = [], {}
    # NSZ remains usable without keys. An optional XML is only a package label,
    # never cryptographic evidence; executable assertions remain mandatory.
    if has_ncz and not keys._keys:
        for name, offset, size in entries:
            if not name.lower().endswith('.cnmt.xml'):
                continue
            require(size <= 1 << 20, 'Content XML is too large')
            try:
                root = ET.fromstring(package.read(offset, size))
                kind = root.findtext('Type')
                if kind not in ('Application', 'Patch'):
                    continue
                title = f'{int(root.findtext("Id"), 16):016x}'
                records.append(dict(title_id=title, application_id=f'{int(title, 16) & ~0x800:016x}',
                                    content_version=int(root.findtext('Version')), role='update' if kind == 'Patch' else 'base',
                                    contents=[], metadata_source='XML (unverified)'))
            except (ET.ParseError, TypeError, ValueError):
                raise ValueError('Invalid content metadata XML') from None
    else:
        for name, offset, size in entries:
            if not name.lower().endswith('.nca'):
                continue
            nca = Nca(package.sub(offset, size), keys, tickets)
            ncas[name.casefold()] = nca
            if nca.content_type != 1:
                continue
            require(size <= 16 << 20, 'Content metadata NCA is too large')
            for section in nca.sections:
                if section.header[2:4] != b'\x01\x02':
                    continue
                pfs = section.pfs()
                for entry, pos, length in pfs_entries(pfs):
                    if entry.lower().endswith('.cnmt'):
                        require(length <= 4 << 20, 'Content metadata is too large')
                        records.append(parse_cnmt(pfs.read(pos, length)))
    require(len(records) <= 1, 'Archive contains multiple applications/updates; select a single-game package')
    metadata = records[0] if records else None
    require(has_ncz or metadata is not None, 'NSP has no readable application metadata; check the archive and prod.keys')
    validate_metadata(metadata)
    return metadata, ncas, 'NSZ' if has_ncz else 'NSP'


def inspect_package(path, keys=None):
    with path.open('rb') as file:
        view, entries, fmt = open_container(View(file))
        metadata, _, _ = package_info(view, entries, keys or Keyset())
        return dict(format=fmt, **({k: v for k, v in metadata.items() if k != 'contents'} if metadata else
                                  dict(role='unknown', content_version=None, metadata_source='unavailable')))


def verify_content(view, content):
    require(view.size == content['size'], 'Program NCA size does not match content metadata')
    digest = hashlib.sha256()
    for offset in range(0, view.size, CHUNK):
        digest.update(view.read(offset, min(CHUNK, view.size - offset)))
    require(digest.hexdigest() == content['sha256'], 'Program NCA SHA-256 mismatch; archive is damaged')


def open_program(source, scratch, stack, label, keys=None):
    source_file = stack.enter_context(source.open('rb'))
    package, entries, fmt = open_container(View(source_file))
    metadata, ncas, _ = package_info(package, entries, keys or Keyset())
    validate_metadata(metadata, label)
    content = None
    if metadata and metadata['metadata_source'] == 'CNMT':
        programs = [c for c in metadata['contents'] if c['type'] == 1]
        require(len(programs) == 1 and programs[0]['index'] == 0, 'No unique main program in content metadata')
        content = programs[0]
    nc = [entry for entry in entries if entry[0].lower().endswith(('.ncz', '.nca')) and
          (content is None or entry[0].lower() in (content['id'] + '.nca', content['id'] + '.ncz'))]
    require(nc, 'Program content listed in metadata is missing')
    programs = []
    for i, (name, start, size) in enumerate(nc):
        print(f'import: reading {label} archive', flush=True)
        if name.lower().endswith('.ncz'):
            output = scratch / f'{label}-{i}.sections'
            sections = decompress_ncz(package.sub(start, size), output)
            file = stack.enter_context(output.open('rb'))
            view = View(file)
        else:
            if name.casefold() not in ncas:  # NSZ without keys: unrelated encrypted metadata is optional
                continue
            view = ncas[name.casefold()]
            if view.content_type != 0:
                continue
            require(view.title_id & ~0x800 == int(APP_ID, 16), 'Program NCA belongs to a different game')
            if content:
                print('import: verifying program content', flush=True)
                verify_content(view.raw, content)
            for section in view.sections:
                if section.header[2:4] == b'\x01\x02':
                    section.pfs()  # verify decrypted ExeFS before searching executable headers
            sections = view.regions
        try:
            files, build_id = find_exefs(view, sections)
            programs.append((view, sections, files, build_id, metadata))
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
    relocation = getattr(update, 'relocation', None)
    if relocation:
        section_start, section_size, table, table_size = relocation
        patch = update.sub(section_start, section_size)
        offsets = [section_start + table]
    else:
        patch = update.sub(sections[0][0], sum(sections[-1]) - sections[0][0])
        offsets = [offset for section_start, section_size in sections[:-1]
                   for offset in range(section_start, section_start + min(section_size, 16 << 20), 0x4000)]
    for offset in offsets:
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


def import_game(base_path, update_path, output, replace=False, keys_path=None, title_keys_path=None, expected_version=None):
    base_path = base_path.resolve(strict=True)
    update_path = update_path.resolve(strict=True) if update_path else None
    require(not output.is_symlink(), 'Output cannot be a symbolic link')
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    require(not base_path.is_relative_to(output.resolve()) and
            (not update_path or not update_path.is_relative_to(output.resolve())),
            'Source archives must be outside the output directory')
    for path in (keys_path, title_keys_path):
        require(not path or not Path(path).resolve().is_relative_to(output), 'Key files must be outside the output directory')
    keys = Keyset(keys_path, title_keys_path)
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
            base, sections, exefs, build_id, metadata = open_program(base_path, scratch, stack, 'base', keys)
            require(build_id == BASE_ID, 'The base archive must contain game v1.0.0')
            offset, header, section_start, section_size = find_romfs(base, sections)
            romfs = base
            executable = base
            if update_path:
                update, update_sections, exefs, build_id, metadata = open_program(update_path, scratch, stack, 'update', keys)
                require(build_id == UPDATE_ID, 'Only the verified v1.1.0 update is supported')
                print('import: applying update extents', flush=True)
                romfs = update_view(base.sub(section_start, section_size), update, update_sections)
                offset, header, _, _ = find_romfs(romfs, [(0, romfs.size)])
                executable = update
            require(expected_version is None or game_version(build_id) == expected_version,
                    'The imported game does not match the selected installation slot')
            for name, start, size in exefs:
                copy_range(executable, start, size, ready / 'exefs' / name)
            count, size = extract_romfs(romfs, offset, header, ready / 'romfs')
        manifest = dict(game_version=game_version(build_id), main_build_id=build_id,
                        source_base=base_path.name, source_update=update_path.name if update_path else None,
                        romfs_files=count, romfs_bytes=size)
        if metadata:
            manifest.update({k: metadata[k] for k in ('title_id', 'content_version', 'metadata_source')})
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
    parser.add_argument('--output', type=Path)
    parser.add_argument('--keys', type=Path, help='Local prod.keys containing derived NCA keys')
    parser.add_argument('--title-keys', type=Path, help='Optional local title.keys for content without a common ticket')
    parser.add_argument('--inspect', action='store_true', help='Read package metadata without installing')
    parser.add_argument('--gui-summary', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--replace', action='store_true')
    parser.add_argument('--expected-version', choices=['1.0.0', '1.1.0'], help='Require this installation slot version')
    args = parser.parse_args(argv)
    try:
        if args.inspect:
            keys = Keyset(args.keys, args.title_keys)
            info = {'base': inspect_package(args.base, keys)}
            require(info['base']['role'] in ('base', 'unknown'), 'Choose a base game archive, not an update')
            if args.update:
                info['update'] = inspect_package(args.update, keys)
                require(info['update']['role'] in ('update', 'unknown'), 'Choose an update archive, not a base game')
            if args.gui_summary:
                fields = ['LASMETA1']
                for role in ('base', 'update'):
                    item = info.get(role, {})
                    version = {0: '1.0.0', 131072: '1.1.0'}.get(item.get('content_version'), '?')
                    fields += [item.get('format', '-'), version, {'CNMT': 'CNMT', 'XML (unverified)': 'XML'}.get(item.get('metadata_source'), 'none')]
                print('\t'.join(fields))
            else:
                print(json.dumps(info, ensure_ascii=False))
        else:
            require(args.output is not None, '--output is required for installation')
            import_game(args.base, args.update, args.output, args.replace, args.keys, args.title_keys, args.expected_version)
    except (ValueError, OSError, zstandard.ZstdError) as error:
        print(f'Import failed: {error}', file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
