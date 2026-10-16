"""Compile and test the actual portable backup store without game assets.

Optionally pass --save-copy PATH to roundtrip an isolated copy of a real save.
The supplied directory is never modified.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--save-copy', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp).resolve()  # macOS /var is a symlink; exercise a real directory under /private/var
    exe = tmp / ('save-test.exe' if os.name == 'nt' else 'save-test')
    cxx = shlex.split(os.environ.get('CXX', 'clang++'))
    subprocess.run(cxx + ['-x', 'c++', '-std=c++17', '-pthread', '-Iruntime',
                          'runtime/ui/save_backup_store.mm', 'tools/test_save_backups.cpp',
                          '-o', str(exe)], cwd=root, check=True)
    subprocess.run([str(exe), str(tmp / 'save_0')], check=True)
    if args.save_copy:
        def hashes(path):
            return {str(p.relative_to(path)): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in path.rglob('*') if p.is_file()}
        before = hashes(args.save_copy)
        copy = tmp / 'real-save-copy'
        shutil.copytree(args.save_copy, copy)
        subprocess.run([str(exe), str(copy), '--roundtrip'], check=True)
        assert hashes(copy) == before == hashes(args.save_copy)
