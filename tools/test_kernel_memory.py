"""Exercise the production page map, resource budgets, rollback and concurrency."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temporary:
    exe = Path(temporary) / ('memory.exe' if os.name == 'nt' else 'memory')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
        '-std=gnu11', '-O2', '-pthread', '-I' + str(ROOT / 'runtime'),
        str(ROOT / 'tools/test_kernel_memory.c'), str(ROOT / 'runtime/kernel_memory.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=45)
print('PASS: page mapping/query, overlap, subrange release, zero reuse, allocation rollback, budgets, reset and concurrency')
