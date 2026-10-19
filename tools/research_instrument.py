"""Create a separate, partially instrumented game-code tree from local generated C.

Normal generated files are never edited. Sites identify original generated source
lines, not AArch64 instruction PCs. Indirect pointers, HLE writes, atomics and asm
are not covered by this static page-reference selection.
"""
import argparse
import bisect
import hashlib
import json
from pathlib import Path
import re
import shutil
from game_addresses import ADDRESS
from game_versions import game_version

FORMAT = 'las_generated_store_trace_v1'
FUNCTION = re.compile(r'^void f_([0-9a-f]+)\(Cpu \*c\) \{', re.M)
TOKENS = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|\bST(8|16|32|64|128)\s*\(')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def instrument(text, filename, page, sites, functions):
    """Select explicit references to the RNG's page, retaining honest partial coverage."""
    starts = list(FUNCTION.finditer(text))
    line_starts = [0, *[m.end() for m in re.finditer('\n', text)]]
    page_pattern = re.compile(r'\b0x'+f'{page:x}'+r'(?:u(?:ll)?|ull)?\b', re.I)
    pieces, previous = [], 0
    for i, match in enumerate(starts):
        end = starts[i+1].start() if i+1 < len(starts) else len(text)
        body = text[match.start():end]
        if not page_pattern.search(body):
            continue
        function = int(match[1], 16)
        before_count = len(sites)

        def replace(token):
            if not token[1]:
                return token[0]  # Strings/comments are not executable stores.
            offset = match.start()+token.start()
            line = bisect.bisect_right(line_starts, offset)
            start = line_starts[line-1]
            stop = text.find('\n', start)
            source_line = text[start:stop if stop >= 0 else len(text)]
            site = len(sites)+1
            sites.append(dict(id=site, function=hex(function), file=filename, line=line,
                              bits=int(token[1]), source_line_sha256=sha(source_line.encode())))
            return f'research_probe_st{token[1]}(c, {site}ull, 0x{function:x}ull, '

        changed = TOKENS.sub(replace, body)
        if len(sites) != before_count:
            functions.append(dict(function=hex(function), file=filename, stores=len(sites)-before_count,
                                  inline_assembly='__asm__' in body))
            pieces.extend((text[previous:match.start()], changed))
            previous = end
    if not pieces:
        return text
    pieces.append(text[previous:])
    return '#include "research_store_probe.h"\n'+''.join(pieces)


def prepare(source, game, output):
    source, game, output = source.resolve(), game.resolve(), output.resolve()
    if output.exists() or output == source or source in output.parents:
        raise ValueError('Use a new output directory outside the source tree')
    with (game/'exefs/main').open('rb') as stream:
        raw = stream.read(0x60)
    if raw[:4] != b'NSO0' or len(raw) != 0x60:
        raise ValueError('Expected an extracted main NSO')
    build = raw[0x40:0x60].rstrip(b'\0').hex()
    version = game_version(build)
    if not version:
        raise ValueError('Only identified LAS executables are supported')
    files = sorted(source.glob('main/*.c'))+sorted(source.glob('sdk/*.c'))
    if not files or len(files) > 4096 or sum(p.stat().st_size for p in files) > 2*1024**3:
        raise ValueError('Invalid generated source size')
    if any(p.is_symlink() or p.parent.is_symlink() for p in files):
        raise ValueError('Linked generated sources are unsupported')
    table = source/'main/main_table.c'
    if f'const char main_build_id[] = "{build}";' not in table.read_text():
        raise ValueError('Generated source and game Build-ID differ')
    address = ADDRESS.RESEARCH_RNG_STATE
    sites, functions, receipts, transformed = [], [], [], []
    for path in files:
        data = path.read_bytes()
        text = data.decode('utf-8')
        if 'research_probe_st' in text or 'lasgame_research_stores' in text:
            raise ValueError('Input is already instrumented')
        name = path.relative_to(source).as_posix()
        result = instrument(text, name, address & ~4095, sites, functions)
        receipts.append(dict(file=name, sha256=sha(data)))
        if result != text:
            transformed.append((path, result))
    if not sites or len(sites)>262144:
        raise ValueError('Expected 1-262144 explicit page-reference store sites')
    manifest = dict(format=FORMAT, build_id=build, version=version, target=hex(address),
                    coverage='static_page_reference_functions', complete_coverage=False,
                    excluded=['indirect pointer-only functions', 'runtime/HLE writes', 'atomic stores', 'inline assembly'],
                    site_meaning='original_generated_c_line_not_guest_instruction_pc',
                    sources=receipts, functions=functions, sites=sites)
    encoded = (json.dumps(manifest, indent=2)+'\n').encode()
    if len(encoded)>32*1024*1024:
        raise ValueError('Store manifest exceeds 32 MiB')
    identity = sha(encoded)
    output.mkdir(parents=True)
    for path in files:
        target = output/path.relative_to(source)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    for path, text in transformed:
        (output/path.relative_to(source)).write_text(text, encoding='utf-8', newline='\n')
    with (output/'main/main_table.c').open('a', encoding='utf-8', newline='\n') as stream:
        stream.write('\n#include "gamelib.h"\nGAMELIB_EXPORT const char *lasgame_research_stores(void) { return "'+identity+'"; }\n')
    (output/'research-stores.json').write_bytes(encoded)
    return dict(manifest=identity, files=len(files), changed_files=len(transformed), functions=len(functions), sites=len(sites), output=str(output))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generated', type=Path, required=True)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(prepare(args.generated, args.game, args.output), indent=2))
    except (ValueError, OSError, UnicodeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
