"""Check the distributable helper's adapter data and imports using synthetic NSZs."""
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
from game_catalog import GAMES, catalog, code_identity
from host_paths import io_path, logical_path
from test_import_game import pfs_files, synthetic_nso, synthetic_romfs, ncz

helper = Path(sys.argv[1]).resolve()
actual = json.loads(subprocess.check_output([str(helper), '--import-nsz', '--catalog'], text=True, encoding='utf-8'))
assert actual == json.loads(json.dumps(catalog())), 'Frozen helper contains stale adapter data'
with tempfile.TemporaryDirectory(prefix='frozen-adapters-', dir=io_path(tempfile.gettempdir())) as temporary:
    root = Path(temporary)
    for game in GAMES:
        release = next(r for r in game.releases if r.role == 'base')
        files = {m.name: synthetic_nso(m.build_id) for m in release.modules}
        files.update({'main.npdm': b'fixture metadata', 'rtld': b'fixture loader'})
        exefs = pfs_files(files); romfs = synthetic_romfs(release.romfs_roots)
        archive = root / (game.id + '.nsz')
        # NCZ counter ranges are not limited to the number of NCA filesystems.
        # Real update packages can have more than the former 64-section limit.
        sections = [(0x4000 + i * 256, 256) for i in range(168)]
        sections += [(0x4000 + 168 * 256, len(romfs) - 168 * 256),
                     (0x4000 + len(romfs), len(exefs))]
        archive.write_bytes(pfs_files({'program.ncz': ncz(bytes(romfs) + exefs, sections=sections)}))
        command = [str(helper), '--import-nsz', '--base', str(archive)]
        inspected = subprocess.run(command + ['--inspect', '--gui-summary'], capture_output=True, encoding='utf-8')
        assert inspected.returncode == 0, inspected.stdout + inspected.stderr
        assert game.id in inspected.stdout
        output = root / ('nested-' + 'x'*90) / ('candidate-' + 'y'*90) / game.id
        assert len(str(output / 'romfs' / game.romfs_roots[0] / 'example.bin')) > 260
        imported = subprocess.run(command + ['--output', str(logical_path(output)), '--allow-development'], capture_output=True, encoding='utf-8')
        assert imported.returncode == 0, imported.stdout + imported.stderr
        manifest = json.loads((output / 'import-manifest.json').read_text())
        assert manifest['game_id'] == game.id and manifest['code_identity'] == code_identity(game, release)
        for module in manifest['modules']:
            assert hashlib.sha256((output / 'exefs' / module['name']).read_bytes()).hexdigest() == module['sha256']
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
    print('PASS: frozen catalog, all adapter imports, full receipts, real ARM64 generation and mismatched-helper rejection')
