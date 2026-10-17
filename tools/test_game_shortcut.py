"""Windows shell-link integration test; no files are placed on the real desktop."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

if os.name != 'nt':
    raise SystemExit('This integration test requires Windows.')
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='shortcut test ') as temp:
    executable = Path(temp) / 'shortcut-test.exe'
    compiler = shlex.split(os.environ.get('CXX', 'clang++'))
    subprocess.run(compiler + ['-std=c++17', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
                              str(root / 'tools/test_game_shortcut.cpp'), '-lole32', '-lshell32', '-luuid',
                              '-o', str(executable)], check=True)
    subprocess.run([str(executable), temp], check=True)
