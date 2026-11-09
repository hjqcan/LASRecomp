"""The Shadows setting (issue #602): tools/test_shadow_map.c, against the production translator and scale code."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / ('shadow-map-test.exe' if os.name == 'nt' else 'shadow-map-test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-O1', '-w', '-Iruntime', 'runtime/nvn/output_scale.c', 'runtime/nvn/maxwell/translate.c',
                    'tools/test_shadow_map.c', '-lm', '-o', str(exe)], cwd=ROOT, check=True)
    env = dict(os.environ)
    env.pop('LARECOMP_SHADOW_SCALE', None)
    subprocess.run([str(exe)], env=env, check=True)
