"""Check the shared 1.0.0 table and patch output without any game files.

Optional: --compare-generated BEFORE AFTER compares two complete generation
directories made from the same player-provided exefs before/after the refactor.
"""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace

from game_addresses import ADDRESS, TABLE_PATH, addr_text, load_table
from patches import HOOKS, PATCHES

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compare-generated', type=Path, nargs=2, metavar=('BEFORE', 'AFTER'))
args = parser.parse_args()
values = load_table(TABLE_PATH)
assert len(values) == len(set(values.values())), 'Use the existing name for shared addresses'
fixture = json.loads((ROOT/'tools/tests/patches_v1_0_0.json').read_text())
assert set(PATCHES) == {int(a, 16) for a in fixture['patches']}
for address, expected in fixture['patches'].items():
    address = int(address, 16)
    gen = SimpleNamespace(base=address, nso=SimpleNamespace(image=struct.pack('<I', int(expected['word'], 16))))
    code = PATCHES[address](gen, address)
    assert hashlib.sha256(code.encode()).hexdigest() == expected['sha256'], f'Patch output changed at {address:x}'
assert hashlib.sha256(json.dumps(HOOKS, sort_keys=True).encode()).hexdigest() == fixture['hooks_sha256']
assert addr_text('f_@HAS_ITEM@(c);') == f'f_{ADDRESS.HAS_ITEM:x}(c);'
try:
    addr_text('@UNKNOWN_ADDRESS@')
except AttributeError:
    pass
else:
    raise AssertionError('An unknown address name was accepted')

# The native consumer must see exactly the same constants, including in switch
# labels and static initializers. This catches width/sign and parser drift.
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    source = tmp/'addresses.c'
    source.write_text('#include "game_addresses.h"\n#include <stdio.h>\n'
                      'static const unsigned long long addresses[] = {\n' +
                      ',\n'.join('GA_'+name for name in values) + '\n};\n'
                      'int main(void) {\n'
                      'for (unsigned i=0; i<sizeof addresses/sizeof addresses[0]; ++i) printf("%llx\\n", addresses[i]);\n'
                      'switch (addresses[0]) { case GA_ASPECT_LITERAL: return 0; default: return 1; }\n}\n')
    exe = tmp/('addresses.exe' if os.name == 'nt' else 'addresses')
    cc = shlex.split(os.environ.get('CC', 'clang'))
    subprocess.run(cc+['-std=c11', '-Iruntime', str(source), '-o', str(exe)], cwd=ROOT, check=True)
    observed = [int(line, 16) for line in subprocess.check_output([str(exe)]).splitlines()]
    assert observed == list(values.values())
    # Same resource location used by tools/lasgen.spec in the frozen helper.
    bundled = tmp/'bundle/runtime/game_addresses'
    bundled.mkdir(parents=True)
    shutil.copyfile(TABLE_PATH, bundled/'v1_0_0.h')
    check = ('import sys; sys._MEIPASS=sys.argv[1]; sys.path.insert(0,sys.argv[2]); '
             'from game_addresses import ADDRESS; print(ADDRESS.HAS_ITEM)')
    output = subprocess.check_output([sys.executable, '-c', check, str(tmp/'bundle'), str(ROOT/'tools')])
    assert int(output) == ADDRESS.HAS_ITEM
    bad = tmp/'bad.h'
    for content in ('', '#define GA_BAD (123)\n', '#define GA_DUP UINT64_C(0x123)\n'*2):
        bad.write_text(content)
        try:
            load_table(bad)
        except ValueError:
            pass
        else:
            raise AssertionError('Malformed table accepted')

# New hard-coded sites should not silently escape the shared table. Guest VM
# bases/range checks are runtime layout constants, not addresses of game objects.
strip_c = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"', re.S)
for path in (ROOT/'runtime').rglob('*'):
    if path.suffix not in ('.c', '.h', '.mm') or path.parent.name == 'game_addresses':
        continue
    code = strip_c.sub('', path.read_text(encoding='utf-8'))
    assert set(re.findall(r'\bGA_([A-Z][A-Z0-9_]*)\b', code)) <= values.keys(), str(path)
    for match in re.finditer(r'0x(700[0-9a-fA-F]{7})', code):
        assert int(match[1], 16) in (0x7000000000, 0x7004000000), str(path)
    assert not re.search(r'(?:MAIN_BASE|SDK_BASE)\s*\+\s*0x[0-9a-fA-F]+', code), str(path)
tree = ast.parse((ROOT/'tools/patches.py').read_text(encoding='utf-8'))
docstring = tree.body[0].value
for node in ast.walk(tree):
    if isinstance(node, ast.Constant) and node is not docstring:
        if isinstance(node.value, int):
            assert not 0x7000000000 < node.value < 0x7100000000
        elif isinstance(node.value, str):
            assert not re.search(r'(?:0x|f_|L_)700[0-9a-fA-F]{7}', node.value)

if args.compare_generated:
    def hashes(folder):
        result = {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in folder.rglob('*.c')}
        assert result, f'No generated C in {folder}'
        return result
    before, after = map(hashes, args.compare_generated)
    assert before == after, 'Generated file names or contents changed'
    print(f'{len(before)} generated C files are byte-identical')
print(f'{len(values)} shared addresses, {len(PATCHES)} patch outputs and {len(HOOKS)} hooks passed')
