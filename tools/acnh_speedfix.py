"""Prepare a separate ACNH 3.0.1 installation with the reviewed 60 FPS mod.

Only patches are shipped. Executables and resource files are made from the
player's own installation, whose original files are never opened for writing.
"""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import tempfile

import lz4.block

ROOT = Path(getattr(sys, '_MEIPASS', Path(__file__).resolve().parents[1]))
BUNDLE = ROOT / 'adapters/acnh/speedfix60'
MARKER = '.acnh-speedfix60'
DIRECTORY = '.acnh60'


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_nso(raw):
    require(len(raw) >= 0x100 and raw[:4] == b'NSO0', 'Invalid NSO header')
    flags, = struct.unpack_from('<I', raw, 0xc)
    segments = []
    for i in range(3):
        file_at, address, size = struct.unpack_from('<III', raw, 0x10 + i * 0x10)
        stored, = struct.unpack_from('<I', raw, 0x60 + i * 4)
        require(size <= 256 << 20 and address + size <= 512 << 20 and
                file_at >= 0x100 and stored <= len(raw) - file_at, 'Invalid NSO segment')
        data = raw[file_at:file_at + stored]
        if flags & (1 << i):
            data = lz4.block.decompress(data, uncompressed_size=size)
        require(len(data) == size, 'Invalid NSO segment length')
        segments.append((address, bytearray(data)))
    require(all(a + len(data) <= b for (a, data), (b, _) in zip(segments, segments[1:])),
            'Overlapping NSO segments')
    return segments


def segment_digest(segments):
    h = hashlib.sha256()
    for address, data in segments:
        h.update(struct.pack('<II', address, len(data)))
        h.update(data)
    return h.hexdigest()


def read_pchtxt(text, build_id):
    """pchtxt addresses are memory offsets; offset_shift is for IPS headers."""
    words, enabled, stopped, matched = {}, False, False, False
    for number, line in enumerate(text.splitlines(), 1):
        line = line.split('//', 1)[0].strip()
        if not line:
            continue
        require(not stopped, 'Data follows pchtxt stop')
        if line.startswith('@nsobid-'):
            require(not matched and line[8:].lower().ljust(64, '0') == build_id,
                    'SpeedFix patch Build ID mismatch')
            matched = True
        elif line in ('@flag print_values', '@flag offset_shift 0x100'):
            pass
        elif line == '@enabled':
            enabled = True
        elif line == '@stop':
            stopped = True
        else:
            require(matched and enabled and re.fullmatch(r'[0-9A-Fa-f]{8} [0-9A-Fa-f]{8}', line),
                    f'Unsupported pchtxt line {number}')
            address, value = line.split()
            address = int(address, 16)
            require(address % 4 == 0 and address not in words, 'Overlapping or unaligned pchtxt word')
            words[address] = bytes.fromhex(value)
    require(matched and stopped and words, 'Incomplete pchtxt patch')
    return words


def write_nso(raw, segments):
    """Canonical uncompressed NSO, with valid per-segment SHA-256 fields."""
    result = bytearray(raw[:0x100])
    flags, = struct.unpack_from('<I', result, 0xc)
    struct.pack_into('<I', result, 0xc, (flags & ~7) | 0x38)
    # The optional filename is not part of any memory segment.
    struct.pack_into('<I', result, 0x1c, 0)
    struct.pack_into('<I', result, 0x2c, 0)
    for i, (address, data) in enumerate(segments):
        result.extend(b'\0' * (-len(result) % 16))
        struct.pack_into('<III', result, 0x10 + i * 0x10, len(result), address, len(data))
        struct.pack_into('<I', result, 0x60 + i * 4, len(data))
        result[0xa0 + i * 32:0xc0 + i * 32] = hashlib.sha256(data).digest()
        result.extend(data)
    return bytes(result)


def patch_nso(raw, specification, patch_text=None):
    require(raw[0x40:0x60].hex() == specification['build_id'], 'SpeedFix module Build ID mismatch')
    segments = read_nso(raw)
    require(segment_digest(segments) == specification['source_sha256'],
            'SpeedFix requires the unmodified 3.0.1 executable modules')
    words = read_pchtxt(patch_text, specification['build_id']) if patch_text else {}
    require(len(words) == specification['words'], 'SpeedFix code patch count mismatch')
    for address, value in words.items():
        matches = [(base, data) for base, data in segments if base <= address <= base + len(data) - 4]
        require(len(matches) == 1, 'SpeedFix word is outside file-backed NSO segments')
        base, data = matches[0]
        data[address - base:address - base + 4] = value
    result = write_nso(raw, segments)
    require(digest(result) == specification['result_sha256'], 'SpeedFix executable result mismatch')
    return result


def apply_ips(patch, original):
    require(patch[:5] == b'PATCH', 'Invalid IPS header')
    result, cursor, ranges = bytearray(original), 5, []
    while patch[cursor:cursor + 3] != b'EOF':
        require(cursor + 5 <= len(patch), 'Truncated IPS record')
        address = int.from_bytes(patch[cursor:cursor + 3], 'big')
        count = int.from_bytes(patch[cursor + 3:cursor + 5], 'big')
        cursor += 5
        if count:
            require(cursor + count <= len(patch), 'Truncated IPS payload')
            data = patch[cursor:cursor + count]
            cursor += count
        else:
            require(cursor + 3 <= len(patch), 'Truncated IPS repeat')
            count = int.from_bytes(patch[cursor:cursor + 2], 'big')
            data = patch[cursor + 2:cursor + 3] * count
            cursor += 3
        require(count > 0 and address <= len(result) - count, 'IPS range changes the resource size')
        ranges.append((address, address + count))
        result[address:address + count] = data
    require(cursor + 3 == len(patch), 'Unsupported IPS trailer')
    ranges.sort()
    require(all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])), 'Overlapping IPS records')
    return bytes(result)


def patch_resource(original, specification, patch=None):
    require(len(original) == specification['size'] and digest(original) == specification['source_sha256'],
            f"SpeedFix requires the unmodified resource: {specification['path']}")
    if patch is not None:
        result = apply_ips(patch, original)
    else:
        result = bytearray(original)
        for offset, before, after in specification['words']:
            require(offset % 4 == 0 and 0 <= offset <= len(result) - 4 and
                    result[offset:offset + 4] == bytes.fromhex(before) and len(bytes.fromhex(after)) == 4,
                    'SpeedFix parameter word mismatch')
            result[offset:offset + 4] = bytes.fromhex(after)
    require(len(result) == len(original) and digest(result) == specification['result_sha256'],
            'SpeedFix parameter result mismatch')
    return bytes(result)


def load_bundle():
    raw = (BUNDLE / 'manifest.json').read_bytes()
    manifest = json.loads(raw)
    require(manifest['schema'] == 1 and manifest['game'] == 'acnh' and manifest['version'] == '3.0.1',
            'Unsupported SpeedFix bundle')
    require(set(manifest['modules']) == {'main', 'sdk', 'subsdk0'} and
            [item['path'] for item in manifest['resources']] ==
            ['Pack/StaticParam.pack', 'Bcsv/ItemNpcFtrActionParam.bcsv',
             'Bcsv/ItemNpcWherearenFtrActionParam.bcsv', 'Bcsv/NpcInterest.bcsv'],
            'Unexpected SpeedFix module or resource paths')
    for item in list(manifest['modules'].values()) + manifest['resources']:
        if item.get('patch'):
            require(item['patch'] in manifest['files'] and Path(item['patch']).name == item['patch'],
                    'Unexpected SpeedFix patch path')
    for name, expected in manifest['files'].items():
        require(Path(name).name == name and digest((BUNDLE / name).read_bytes()) == expected,
                'SpeedFix bundle integrity check failed')
    return manifest, digest(raw)


def render_native_manifest():
    manifest, identity = load_bundle()
    rows = [(f'exefs/{name}', item['result_size'], item['result_sha256'])
            for name, item in manifest['modules'].items()]
    rows += [(f"romfs/{item['path']}", item['size'], item['result_sha256']) for item in manifest['resources']]
    lines = ['// Generated from adapters/acnh/speedfix60/manifest.json by tools/acnh_speedfix.py.',
             f'#define ACNH60_MANIFEST_SHA "{identity}"',
             f'#define ACNH60_MAIN_TEXT_SHA "{manifest["modules"]["main"]["result_text_sha256"]}"',
             'static const struct { const char *path; uint64_t size; const char *sha; } acnh60_files[] = {']
    lines += [f'    {{"{path}", UINT64_C({size}), "{sha}"}},' for path, size, sha in rows]
    return '\n'.join(lines + ['};', ''])


def read_resource(root, relative):
    image = root / 'romfs.bin'
    if not image.exists():
        return (root / 'romfs' / relative).read_bytes()
    # Match runtime precedence: a retained image is authoritative, even if an
    # extracted folder exists alongside it.
    from containers import View
    from import_game import romfs_header
    from game_catalog import get_game
    with image.open('rb') as stream:
        view = View(stream)
        h = romfs_header(view, 0, get_game('acnh'))
        dirs, files = view.read(h[3], h[4]), view.read(h[7], h[8])
        at = 0
        for part in relative.split('/')[:-1]:
            child, = struct.unpack_from('<I', dirs, at + 8)
            seen = set()
            while child != 0xffffffff:
                require(child not in seen and child + 24 <= len(dirs), 'Invalid RomFS directory chain')
                seen.add(child)
                length, = struct.unpack_from('<I', dirs, child + 20)
                require(child + 24 + length <= len(dirs), 'Invalid RomFS directory name')
                if dirs[child + 24:child + 24 + length].decode('utf-8') == part:
                    at = child
                    break
                child, = struct.unpack_from('<I', dirs, child + 4)
            else:
                raise ValueError(f'Missing SpeedFix resource directory: {relative}')
        at, = struct.unpack_from('<I', dirs, at + 12)
        seen = set()
        while at != 0xffffffff:
            require(at not in seen and at + 32 <= len(files), 'Invalid RomFS file chain')
            seen.add(at)
            parent, sibling, offset, size, _, length = struct.unpack_from('<IIQQII', files, at)
            require(at + 32 + length <= len(files), 'Invalid RomFS filename')
            if files[at + 32:at + 32 + length].decode('utf-8') == relative.split('/')[-1]:
                require(size <= 128 << 20, 'SpeedFix resource is too large')
                return view.read(h[9] + offset, size)
            at = sibling
    raise ValueError(f'Missing SpeedFix resource: {relative}')


def link_or_copy(source, destination):
    require(not source.is_symlink() and source.is_file(), 'SpeedFix source must be an ordinary file')
    try:
        os.link(source, destination)
    except OSError:
        shutil.copyfile(source, destination)


def copy_romfs(source, destination, replacements):
    # Keep the real extracted directory structure for launcher validation and
    # resource preparation, even when runtime reads prefer the retained image.
    for directory, children, names in os.walk(source, followlinks=False):
        current = Path(directory)
        require(not current.is_symlink() and all(not (current / n).is_symlink() for n in children),
                'SpeedFix does not follow source directory links')
        relative = current.relative_to(source)
        target = destination / relative
        target.mkdir(exist_ok=True)
        for name in names:
            if (relative / name).as_posix() not in replacements:
                link_or_copy(current / name, target / name)


def prepare(source):
    source = Path(source).resolve(strict=True)
    require(not (source / MARKER).exists() and source.name != DIRECTORY, 'Select the normal 3.0.1 installation')
    require((source / 'romfs/Movie').is_dir(), 'Select a complete installed 3.0.1 game folder')
    manifest, identity = load_bundle()
    destination = source / DIRECTORY
    require(not destination.exists() and not destination.is_symlink(),
            'The SpeedFix installation already exists; use its own build/resume action')
    prepared = {}
    for name, specification in manifest['modules'].items():
        patch = (BUNDLE / specification['patch']).read_text() if specification.get('patch') else None
        prepared['exefs/' + name] = patch_nso((source / 'exefs' / name).read_bytes(), specification, patch)
    for specification in manifest['resources']:
        patch = (BUNDLE / specification['patch']).read_bytes() if specification.get('patch') else None
        prepared['romfs/' + specification['path']] = patch_resource(read_resource(source, specification['path']), specification, patch)
    print('SpeedFix: all executable and resource guards passed', flush=True)
    stage = Path(tempfile.mkdtemp(prefix='.acnh60-prepare-', dir=source))
    try:
        (stage / 'exefs').mkdir()
        (stage / 'romfs').mkdir()
        if (source / 'romfs.bin').is_file():
            link_or_copy(source / 'romfs.bin', stage / 'romfs.bin')
        copy_romfs(source / 'romfs', stage / 'romfs', {p[6:] for p in prepared if p.startswith('romfs/')})
        for item in (source / 'exefs').iterdir():
            if 'exefs/' + item.name not in prepared:
                link_or_copy(item, stage / 'exefs' / item.name)
        for relative, data in prepared.items():
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (stage / MARKER).write_bytes(('LASACNH60-1\n' + identity + '\n').encode('ascii'))
        # The publication is last. A cancelled preparation never becomes playable.
        stage.rename(destination)
    finally:
        if stage.exists():
            require(stage.resolve().parent == source and stage.name.startswith('.acnh60-prepare-'),
                    'Unexpected SpeedFix staging path')
            shutil.rmtree(stage)
    print(f'SpeedFix: prepared {destination}; compile its game code before playing', flush=True)
    return destination
