"""Exercise newer SDK event queries through the real codec, handlers and kernel poll."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
kernel = (ROOT / 'runtime/kernel.c').read_text(encoding='utf8')
constants = kernel[kernel.index('#undef R_OK'):kernel.index('#define WAITERS_BIT')]
wait = kernel[kernel.index('static uint32_t svc_wait_synchronization('):kernel.index('// ------------------------------------------------------------------ debug output')]
source = (ROOT / 'tools/test_am_events.c').read_text(encoding='utf8').replace('/* production wait */', constants + wait)
with tempfile.TemporaryDirectory(prefix='am-events-') as temporary:
    source_file = Path(temporary) / 'events.c'
    source_file.write_text(source, encoding='utf8')
    exe = Path(temporary) / ('events.exe' if os.name == 'nt' else 'events')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-O2', '-pthread', '-I' + str(ROOT / 'tools'), str(source_file), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
