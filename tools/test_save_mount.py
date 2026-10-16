"""Compile the production save-mount guard with an SDK stand-in; no game files."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temporary:
    exe = Path(temporary)/('save-mount.exe' if os.name == 'nt' else 'save-mount')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-O2', '-pthread', '-Iruntime', 'tools/test_save_mount.c', '-o', str(exe)],
                   cwd=root, check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
