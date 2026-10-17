import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp).resolve()
    exe = root / ('research.exe' if os.name == 'nt' else 'research')
    subprocess.run(shlex.split(os.environ.get('CXX', 'clang++')) + [
        '-std=c++17', '-Wall', '-Wextra', '-Iruntime', '-x', 'c++',
        'tools/test_research.cpp', 'runtime/ui/research_tape.mm', '-o', str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe), str(root / 'fixtures')], check=True)
    runtime = root / ('runtime.exe' if os.name == 'nt' else 'runtime')
    subprocess.run(shlex.split(os.environ.get('CXX', 'clang++')) + [
        '-std=c++17', '-Wall', '-Wextra', '-Iruntime', '-x', 'c++',
        'tools/test_research_runtime.cpp', 'runtime/ui/research_tape.mm', 'runtime/ui/research.mm',
        '-o', str(runtime)], cwd=ROOT, check=True)
    subprocess.run([str(runtime), str(root / 'runtime-fixtures')], check=True)
