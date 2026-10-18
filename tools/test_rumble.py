"""Run the production HID bridge and an SDL virtual-controller rumble test."""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
sdl = shlex.split(os.environ.get('SDL_FLAGS') or subprocess.check_output(
    ['pkg-config', '--cflags', '--libs', 'sdl3'], text=True))
cc = shlex.split(os.environ.get('CC', 'clang'))
gc = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
with tempfile.TemporaryDirectory(prefix='las-rumble-') as directory:
    for name in ('test_rumble', 'test_sdl_rumble'):
        exe = Path(directory) / (name + ('.exe' if os.name == 'nt' else ''))
        subprocess.run([*cc, '-std=gnu11', '-O2', '-ffunction-sections', '-fdata-sections',
                        str(root / 'tools' / (name + '.c')), *sdl, gc, '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=15)
