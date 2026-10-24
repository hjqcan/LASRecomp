"""Frame pacing (runtime/nvn/pace.c) against simulated frames (tools/test_pace.c): each case's holds at 30 and returns
to 60. -v: the pacer's own [pace] log lines too."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
verbose = '-v' in sys.argv
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / ('pace-test.exe' if os.name == 'nt' else 'pace-test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-O1', '-DPACE_TEST', '-Iruntime', 'runtime/nvn/pace.c', 'tools/test_pace.c', '-lm',
                    '-o', str(exe)], cwd=ROOT, check=True)
    n = int(subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout)
    failed = 0
    for i in range(n):
        env = {k: v for k, v in os.environ.items() if not k.startswith('LARECOMP_')}
        r = subprocess.run([str(exe), str(i)], capture_output=True, text=True, env=env)
        print(r.stdout, end='')
        if verbose:
            for line in r.stderr.splitlines():
                if line.startswith('[pace]'):
                    print('   ', line)
            print()
        failed += r.returncode != 0
    print(f'{n - failed} of {n} frame pacing cases passed')
    sys.exit(1 if failed else 0)
