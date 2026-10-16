"""Synthetic native tests; no game files are needed."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary).resolve()
    cc = shlex.split(os.environ.get('CC', 'clang'))
    cxx = shlex.split(os.environ.get('CXX', 'clang++'))
    objects = []
    for name in ('workshop', 'performance'):
        obj = root / (name + '.o')
        subprocess.run(cc + ['-std=c11', '-Wall', '-Wextra', '-Iruntime', '-c',
                            'runtime/' + name + '.c', '-o', str(obj)], cwd=ROOT, check=True)
        objects.append(str(obj))
    exe = root / ('test.exe' if os.name == 'nt' else 'test')
    subprocess.run(cxx + ['-std=c++17', '-Wall', '-Wextra', '-Iruntime', '-x', 'c++',
                         'tools/test_workshop.cpp', 'runtime/ui/workshop_store.mm', '-x', 'none',
                         *objects, '-o', str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe), str(root / 'fixtures')], check=True)
