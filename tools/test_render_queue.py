"""Run the actual NVN recorder/queue and fence-wait fixtures without a GPU."""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as tmp:
    for name in ('render_queue', 'nvn_sync'):
        exe=Path(tmp)/(name+('.exe' if os.name=='nt' else ''))
        subprocess.run([*shlex.split(os.environ.get('CC','clang')), '-std=gnu11', '-O1', '-pthread',
                        '-ffunction-sections', '-fdata-sections', 'tools/test_'+name+'.c',
                        '-Wl,-dead_strip' if sys.platform=='darwin' else '-Wl,--gc-sections',
                        '-o', str(exe)], cwd=root,check=True)
        subprocess.run([str(exe)],cwd=root,check=True,timeout=10)
