"""Exercise csrng's map-alias wire contract with the host OS entropy source."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / ('test-csrng.exe' if os.name == 'nt' else 'test-csrng')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
        '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
        str(root / 'tools/test_csrng.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True,
                   env={k: v for k, v in os.environ.items() if k != 'LARECOMP_STRICT_IPC'})
