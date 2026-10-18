"""Exercise actual randomizer hooks with a tiny synthetic room and seed."""
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='enemy-random-') as directory:
    folder = Path(directory)
    room = folder / 'romfs/Room/test.leb'
    room.parent.mkdir(parents=True)
    record = bytearray(0x88)
    struct.pack_into('<Q', record, 0, 0x123456789)
    struct.pack_into('<H', record, 0xC, 1)
    room.write_bytes(record)
    (folder / 'enemies_random.tsv').write_text(
        'pick\tland\t0x2\n'
        'enemy\t0x2\t1\ti:7 i:0 i:0 i:0 i:0 i:0 i:0 i:0\n'
        'file\tRoom/test.leb\t-\n'
        'slot\t0\t0x1\t0x1\tland\t0\t1\t0\t1 1 1\tS\n', encoding='utf8')
    exe = folder / ('test.exe' if os.name == 'nt' else 'test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
        '-std=gnu11', '-O1', '-pthread', '-Iruntime', 'tools/test_enemy_random.c', '-o', str(exe)],
        cwd=root, check=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('LARECOMP_ENEMY_RANDOM')}
    env['LARECOMP_ENEMY_RANDOM_SEED'] = '1'
    subprocess.run([str(exe), str(folder)], env=env, check=True)
