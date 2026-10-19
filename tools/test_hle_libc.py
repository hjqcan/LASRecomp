"""Run the shared SDK libc implementations without game assets."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='hle-libc-') as directory:
    exe = Path(directory) / ('test.exe' if os.name == 'nt' else 'test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
        '-std=gnu11', '-O2', '-Iruntime', 'tools/test_hle_libc.c', '-o', str(exe)], cwd=root, check=True)
    subprocess.run([str(exe)], check=True)
