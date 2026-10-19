"""Run multi-commit through the real IPC codec without game files or user saves."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='save-multicommit-') as temporary:
    temporary = Path(temporary)
    exe = temporary / ('multicommit.exe' if os.name == 'nt' else 'multicommit')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-O2', '-pthread', '-Iruntime', 'tools/test_save_multicommit.c', '-o', str(exe)],
                   cwd=root, check=True)
    save = temporary / 'save'
    save.mkdir()
    subprocess.run([str(exe), str(save)], check=True, timeout=30)
