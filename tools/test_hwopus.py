"""Exercise the production hwopus service with generated audio and a libopus reference decoder."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
try:
    include = subprocess.check_output(['pkg-config', '--variable=includedir', 'opus'], text=True).strip()
    flags = ['-I'+include] + shlex.split(subprocess.check_output(['pkg-config', '--libs', 'opus'], text=True))
except (OSError, subprocess.CalledProcessError):
    if os.sys.platform == 'darwin':
        prefix = subprocess.check_output(['brew', '--prefix', 'opus'], text=True).strip()
        flags = ['-I'+prefix+'/include', '-L'+prefix+'/lib', '-lopus']
    else:
        flags = ['-lopus']
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / ('test-hwopus.exe' if os.name == 'nt' else 'test-hwopus')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
        '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread',
        str(ROOT/'tools/test_hwopus.c'), *flags, '-o', str(exe)], check=True)
    env = {k: v for k, v in os.environ.items() if k != 'LARECOMP_STRICT_IPC'}
    subprocess.run([str(exe)], env=env, check=True)
