"""Dynamic resolution's controller (runtime/nvn/output_scale.c) against a simulated GPU (tools/test_dynres.c): each
case's factor path. -v: the controller's own [dynres] log lines (holds, looking for 60 again) too."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
verbose = '-v' in sys.argv
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / ('dynres-test.exe' if os.name == 'nt' else 'dynres-test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-O1', '-Iruntime', 'runtime/nvn/output_scale.c', 'tools/test_dynres.c', '-lm',
                    '-o', str(exe)], cwd=ROOT, check=True)
    n = int(subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout)
    failed = 0
    for i in range(n):
        env = dict(os.environ)
        env.pop('LARECOMP_LOG_DYNRES', None)
        r = subprocess.run([str(exe), str(i)], capture_output=True, text=True, env=env)
        print(r.stdout, end='')
        if verbose:
            for line in r.stderr.splitlines():
                if any(k in line for k in ('reach', 'looking', 'ended', 'misses', 'would fit', 'meet')):
                    print('   ', line)
            print()
        failed += r.returncode != 0
    print(f'{n - failed} of {n} dynamic resolution cases passed')
    sys.exit(1 if failed else 0)
