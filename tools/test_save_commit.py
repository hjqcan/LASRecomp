"""Check atomic save publication and fork sidecars/backups without game files."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temporary:
    temporary = Path(temporary)
    exe = temporary / ('save-commit.exe' if os.name == 'nt' else 'save-commit')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-O2', '-pthread', '-Iruntime', 'tools/test_save_commit.c',
                    'runtime/title_profile.c', '-o', str(exe)],
                   cwd=root, check=True)
    save = temporary / 'save'
    save.mkdir()
    subprocess.run([str(exe), str(save)], check=True, timeout=30)
