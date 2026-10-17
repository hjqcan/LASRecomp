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
        'runtime/ui/research_update.mm', 'runtime/research_gate.c',
        '-o', str(runtime)], cwd=ROOT, check=True)
    subprocess.run([str(runtime), str(root / 'runtime-fixtures')], check=True)
    update = root / ('update.exe' if os.name == 'nt' else 'update')
    subprocess.run(shlex.split(os.environ.get('CXX', 'clang++')) + [
        '-std=c++17', '-Wall', '-Wextra', '-Iruntime', '-x', 'c++',
        'tools/test_research_update.cpp', 'runtime/ui/research_tape.mm', 'runtime/ui/research_update.mm',
        'runtime/research_gate.c', 'runtime/research_sites.c', '-o', str(update)], cwd=ROOT, check=True)
    subprocess.run([str(update), str(root / 'update-fixtures')], check=True)
    actors = root / ('actors.exe' if os.name == 'nt' else 'actors')
    subprocess.run(shlex.split(os.environ.get('CXX', 'clang++')) + [
        '-std=c++17', '-Wall', '-Wextra', '-Iruntime', '-x', 'c++',
        'tools/test_research_actors.cpp', 'runtime/research_actors.c', '-o', str(actors)], cwd=ROOT, check=True)
    subprocess.run([str(actors)], check=True)
