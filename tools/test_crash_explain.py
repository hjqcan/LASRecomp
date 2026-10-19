"""Exercise production crash messages with adapter-specific content counts."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='crash-explain-') as directory:
    exe = Path(directory) / ('test.exe' if os.name == 'nt' else 'test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
        '-std=gnu11', '-O2', '-Iruntime', 'tools/test_crash_explain.c', '-o', str(exe)], cwd=root, check=True)
    subprocess.run([str(exe)], check=True)
