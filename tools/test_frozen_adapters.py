"""Check the distributable helper using synthetic extracted modules only."""
import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
from game_catalog import GAMES, catalog, code_identity
from host_paths import io_path, logical_path
from test_game_fixtures import synthetic_nso
from test_cross_fragment_tables import adr
from package_multi_windows import validate_public_helper

helper = Path(sys.argv[1]).resolve()
validate_public_helper(helper)
actual = json.loads(subprocess.check_output([str(helper), '--game-setup', '--catalog'], text=True, encoding='utf-8'))
assert actual == json.loads(json.dumps(catalog())), 'Frozen helper contains stale adapter data'
with tempfile.TemporaryDirectory(prefix='frozen-adapters-', dir=io_path(tempfile.gettempdir())) as temporary:
    root = Path(temporary)
    for game in GAMES:
        release = next(r for r in game.releases if r.role == 'base')
        exefs = root / ('nested-' + 'x'*90) / ('candidate-' + 'y'*90) / game.id / 'exefs'
        exefs.mkdir(parents=True)
        for module in release.modules:
            (exefs / module.name).write_bytes(synthetic_nso(module.build_id))
        command = [str(helper), '--game-setup', '--inspect-exefs', str(logical_path(exefs))]
        inspected = subprocess.run(command, capture_output=True, encoding='utf-8')
        assert inspected.returncode == 0, inspected.stdout + inspected.stderr
        plan = json.loads(inspected.stdout)
        assert plan['game_id'] == game.id and plan['code_identity'] == code_identity(game, release)
        assert plan['installable'] == release.installable
    # Run the actual frozen recompiler on a tiny synthetic ARM64 function with a valid NSO/MOD0 layout.
    game = next(g for g in GAMES if g.generator_profile == 'generic'); release = game.releases[0]
    raw = bytearray(0x200); raw[:4] = b'NSO0'; raw[0x40:0x60] = bytes.fromhex(release.main_build_id)
    for i, address in enumerate((0, 0x100, 0x200)):
        struct.pack_into('<III', raw, 0x10+i*0x10, 0x100+i*0x40, address, 0x40)
        struct.pack_into('<I', raw, 0x60+i*4, 0x40)
    struct.pack_into('<I', raw, 0x38, 0x80)
    struct.pack_into('<I', raw, 0x68, 0x80)
    struct.pack_into('<16I', raw, 0x100, *([0xd65f03c0]*16))
    struct.pack_into('<II', raw, 0x100, 0x94000008, 0x100)
    # LDAXP x0,x1,[x2]; STLXP w3,x0,x1,[x2]; RET exercises the pair decoder
    # inside the shipped executable, not just the source Python interpreter.
    struct.pack_into('<III', raw, 0x120, 0xc87f8440, 0xc8238440, 0xd65f03c0)
    raw[0x140:0x144] = b'MOD0'
    struct.pack_into('<6i', raw, 0x144, 0x100, 0x140, 0x140, -0x100, -0x100, 0)
    # DT_RELR in rodata (memory 0x120), relocating a data pointer at 0x240.
    struct.pack_into('<Q', raw, 0x160, 0x240)
    for i, values in enumerate(((36,0x120),(35,8),(37,8),(0,0))):
        struct.pack_into('<QQ', raw, 0x180+i*16, *values)
    struct.pack_into('<Q', raw, 0x1c0, 0x20)
    source = root / 'synthetic-main'; source.write_bytes(raw)
    generated = root / 'generated'
    command = [str(helper), 'main', str(source), hex(release.modules[0].base), str(generated), '--profile', 'generic',
               '--adapter', game.id, '--release', release.version, '--adapter-id']
    mismatch = subprocess.run(command + ['0'*64], capture_output=True, encoding='utf-8')
    assert mismatch.returncode != 0 and 'different adapter/code-generation inputs' in mismatch.stderr
    compiled = subprocess.run(command + [code_identity(game, release)], capture_output=True, encoding='utf-8')
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    assert 'main_fn_count = 1' in (generated / 'main_table.c').read_text()
    assert release.main_build_id in (generated / 'main_table.c').read_text()
    emitted = (generated / 'main_000.c').read_text()
    assert 'ldxp64(c,' in emitted and 'stxp64(c,' in emitted and 'rt_udf(' not in emitted
    # A bounded table invokes Unicorn's native JIT. Straight-line generation
    # alone does not catch a frozen Windows helper missing the pe_no_cfg step.
    table_raw = bytearray(raw)
    base = release.modules[0].base
    struct.pack_into('<I', table_raw, 0x100, 0x94000002)  # BL text+8, skipping MOD0 offset
    table_code = [0x7100091F, 0x54000188, adr(9, base + 0x10, base + 0x250),
                  adr(10, base + 0x14, base + 0x30), 0x7868792B, 0x8B0B094A, 0xD61F0140]
    struct.pack_into('<7I', table_raw, 0x108, *table_code)
    struct.pack_into('<7I', table_raw, 0x124, *([0xD65F03C0] * 7))
    struct.pack_into('<QQ', table_raw, 0x180, 0, 0)  # no relocations in this fixture
    struct.pack_into('<3H', table_raw, 0x1D0, 0, 1, 2)
    source.write_bytes(table_raw)
    compiled = subprocess.run(command + [code_identity(game, release)], capture_output=True, encoding='utf-8')
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    emitted = (generated / 'main_000.c').read_text()
    for offset in (0x30, 0x34, 0x38):
        assert f'case 0x{base + offset:x}ull:' in emitted, emitted
    print('PASS: frozen catalog, extracted module plans, removed-command rejection, ARM64/JIT generation and identity checks')
