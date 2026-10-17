"""Inventory local executable headers without approving an adapter or installing a game."""
from contextlib import ExitStack
import re
import struct
import tempfile

from containers import View, open_container, pfs_entries, require
from game_catalog import game_for_build
from host_paths import io_path, logical_path
from import_game import CHUNK, decompress_ncz, package_info, verify_content
from nca import Keyset

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


def executable_candidates(view, regions, *, counter_ranges=False):
    found, seen = [], set()
    for start, length in regions:
        data, pos = view.read(start, min(length, 32 << 20)), 0
        while (pos := data.find(b'PFS0', pos)) >= 0:
            offset = start + pos
            pos += 4
            if offset in seen: continue
            seen.add(offset)
            try:
                # NCA files stay inside one filesystem section. NCZ entries are
                # encryption-counter ranges, so an ExeFS may span several of them.
                lower = regions[0][0] if counter_ranges else start
                extent = view.size - lower if counter_ranges else length
                section = view.sub(lower, extent)
                entries = pfs_entries(section, offset - lower)
            except (ValueError, UnicodeError, struct.error):
                continue
            names = {name for name, _, _ in entries}
            if not {'main', 'main.npdm'} <= names: continue
            modules = []
            for name, location, size in entries:
                if MODULE.fullmatch(name):
                    modules.append(dict(name=name, **nso_header(section, location, size)))
            modules.sort(key=lambda module: (module['name'] != 'main', module['name']))
            require(0 < len(modules) <= 13, 'Unexpected executable module count')
            found.append((section, entries, modules))
    return found


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


def probe_archive(source, keys=None, exefs=None):
    source = io_path(source)
    destination = io_path(exefs) if exefs is not None else None
    require(destination is None or not destination.exists(), 'Executable output directory already exists')
    keys = keys or Keyset()
    # Expanded content is private temporary data; only explicitly requested ExeFS
    # copies survive. This path never touches managed installations or the catalog.
    with tempfile.TemporaryDirectory(prefix='adapter-probe-') as temporary, ExitStack() as stack:
        scratch = io_path(temporary)
        file = stack.enter_context(source.open('rb'))
        package, entries, fmt = open_container(View(file))
        metadata, ncas, _ = package_info(package, entries, keys, validate_adapter=False)
        content = None
        if metadata and metadata['metadata_source'] == 'CNMT':
            programs = [entry for entry in metadata['contents'] if entry['type'] == 1]
            require(len(programs) == 1 and programs[0]['index'] == 0,
                    'No unique main program in content metadata')
            content = programs[0]
        candidates = []
        for i, (name, offset, size) in enumerate(entries):
            if content and name.lower() not in (content['id'] + '.nca', content['id'] + '.ncz'): continue
            counter_ranges = name.lower().endswith('.ncz')
            if counter_ranges:
                output = scratch / f'{i}.sections'
                regions = decompress_ncz(package.sub(offset, size), output)
                view = View(stack.enter_context(output.open('rb')))
            elif name.casefold() in ncas and ncas[name.casefold()].content_type == 0:
                view = ncas[name.casefold()]
                if content: verify_content(view.raw, content)
                if metadata:
                    require(view.title_id & ~0x800 == int(metadata['application_id'], 16),
                            'Program NCA belongs to a different application')
                for section in view.sections:
                    if section.header[2:4] == b'\x01\x02': section.pfs()
                regions = view.regions
            else:
                continue
            for section, files, modules in executable_candidates(view, regions, counter_ranges=counter_ranges):
                candidates.append((name, section, files, modules))
        require(len(candidates) == 1, 'No unique executable program found; no adapter or installation was created')
        name, section, files, modules = candidates[0]
        result = dict(schema=1, format=fmt, archive=str(logical_path(source)), program_entry=name,
                      metadata={k: v for k, v in metadata.items() if k != 'contents'} if metadata else None,
                      modules=modules, adapter=adapter_match(modules),
                      evidence='Executable header inventory only; content, compatibility and gameplay are not qualified.')
        if destination is not None:
            destination.mkdir(parents=True, exist_ok=False)
            for filename, offset, size in files:
                if not MODULE.fullmatch(filename) and filename != 'main.npdm': continue
                with (destination / filename).open('xb') as target:
                    for start in range(0, size, CHUNK):
                        target.write(section.read(offset + start, min(CHUNK, size - start)))
            result['exefs'] = str(logical_path(destination))
        return result
