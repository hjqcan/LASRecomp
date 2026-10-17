from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temp:
    temporary_root = Path(temp).resolve()
    executable = temporary_root / ('profiles.exe' if os.name == 'nt' else 'profiles')
    compiler = shlex.split(os.environ.get('CXX', 'clang++'))
    subprocess.run(compiler + ['-x', 'c++', '-std=c++17', str(root / 'tools/test_game_profiles.cpp'),
                              str(root / 'runtime/ui/game_profiles.mm'), str(root / 'runtime/ui/game_install.mm'), '-o', str(executable)], check=True)
    subprocess.run([str(executable), str(temporary_root / 'data')], check=True)
