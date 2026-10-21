"""Exercise shared production frame pacing with a deterministic clock and title changes."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / ('test-pace.exe' if os.name == 'nt' else 'test-pace')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
        '-std=gnu11', '-O1', str(ROOT/'tools/test_frame_pacing.c'), '-o', str(exe)], check=True)
    for enabled in ('1', '0'):
        subprocess.run([str(exe)] + ([] if enabled == '1' else ['off']),
                       env=dict(os.environ, LARECOMP_PACE=enabled), check=True)
