"""Run production NVN presentation entry points against a deterministic host clock."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / ('test-present.exe' if os.name == 'nt' else 'test-present')
    command = shlex.split(os.environ.get('CC', 'clang')) + ['-std=gnu11', '-O1',
        str(ROOT/'tools/test_nvn_present_interval.c'), '-o', str(exe)]
    subprocess.run(command, check=True)
    subprocess.run([str(exe)], check=True)
