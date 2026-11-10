"""Native power-up music policy checks, without game files or audio devices."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / ('music-test.exe' if os.name == 'nt' else 'music-test')
    subprocess.run([*shlex.split(os.environ.get('CC', 'clang')), '-std=gnu11', '-O2', '-Iruntime',
                    'tools/test_powerup_music.c', '-o', str(exe)], cwd=root, check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
