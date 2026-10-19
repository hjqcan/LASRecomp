"""Inventory already-extracted NSO modules without installing or running a game."""
import re
import struct

from binary_view import View, require
from game_catalog import game_for_build
from host_paths import io_path, logical_path

MODULE = re.compile(r'(?:main|sdk|rtld|subsdk[0-9])')


def nso_header(view, offset, size):
    """Read bounded NSO metadata only; do not load or execute the module."""
    require(size >= 0x100, 'Truncated NSO header')
    header = view.read(offset, 0x100)
    require(header[:4] == b'NSO0', 'Invalid NSO header')
    flags, = struct.unpack_from('<I', header, 0xc)
    segments, file_ranges, memory_ranges = [], [], []
    for i, name in enumerate(('text', 'rodata', 'data')):
        start, memory, length = struct.unpack_from('<III', header, 0x10 + i * 0x10)
        compressed = bool(flags & (1 << i))
        stored, = struct.unpack_from('<I', header, 0x60 + i * 4)
        stored = stored if compressed else length
        require(start >= 0x100 and start <= size and stored <= size - start,
                'NSO segment exceeds its file')
        require(memory % 0x1000 == 0 and length <= (1 << 31) - memory,
                'Invalid NSO memory segment')
        if stored: file_ranges.append((start, start + stored))
        if length: memory_ranges.append((memory, memory + length))
        segments.append(dict(name=name, memory_offset=hex(memory), size=length,
                             stored_size=stored, compressed=compressed))
    for ranges in (file_ranges, memory_ranges):
        ranges.sort()
        require(all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])), 'Overlapping NSO segments')
    bss, = struct.unpack_from('<I', header, 0x3c)
    end = int(segments[2]['memory_offset'], 16) + segments[2]['size'] + bss
    require(0 < end <= 1 << 31, 'Invalid NSO memory size')
    return dict(build_id=header[0x40:0x60].hex(), file_size=size, image_size=(end + 0xfff) & ~0xfff,
                bss_size=bss, segments=segments)


def adapter_match(modules):
    """A known main with a different SDK is not an exact reviewed module set."""
    main = next(module for module in modules if module['name'] == 'main')
    try:
        game = game_for_build(main['build_id'])
    except ValueError:
        return dict(status='unregistered', installable=False)
    release = game.release(main['build_id'])
    actual = {module['name']: module['build_id'] for module in modules}
    mismatched = [module.name for module in release.modules if actual.get(module.name) != module.build_id]
    unreviewed = sorted(set(actual) - {module.name for module in release.modules} - {'rtld'})
    exact = not mismatched and not unreviewed
    return dict(status='matched' if exact else 'module-mismatch', game_id=game.id,
                version=release.version, qualification=release.status,
                installable=False, catalog_installable=release.installable,
                mismatched_modules=mismatched, unreviewed_modules=unreviewed)


def probe_exefs(source):
    source = io_path(source)
    require(source.is_dir(), 'Choose an already-extracted ExeFS directory')
    modules = []
    for path in sorted(source.iterdir(), key=lambda p: (p.name != 'main', p.name)):
        if not MODULE.fullmatch(path.name):
            continue
        require(path.is_file() and not path.is_symlink(), 'Executable modules must be ordinary files')
        with path.open('rb') as stream:
            view = View(stream)
            modules.append(dict(name=path.name, **nso_header(view, 0, view.size)))
    require(any(m['name'] == 'main' for m in modules), 'No main NSO module found')
    require(len(modules) <= 13, 'Unexpected executable module count')
    return dict(schema=1, format='ExeFS', exefs=str(logical_path(source)), modules=modules,
                adapter=adapter_match(modules),
                evidence='Executable header inventory only; content, compatibility and gameplay are not qualified.')
