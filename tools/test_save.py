"""Exercise the production save handlers using temporary files, without game data (tools/test_save_*.c): saves
commit and reload with their flag sidecars, and a save that fails anywhere returns success to the game (whose SDK
aborts on fs errors) while the committed save stays as it was."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
# The fork's mount, backup and multi-commit harnesses have their own drivers and
# link dependencies. This runner covers the upstream host-filesystem harnesses.
for name in ('test_save_flags.c', 'test_save_errors.c', 'test_save_romfs_fds.c'):
    source = root/'tools'/name
    with tempfile.TemporaryDirectory(prefix='save-test-') as temporary:
        temporary = Path(temporary)
        exe = temporary/('test.exe' if os.name == 'nt' else 'test')
        subprocess.run([*shlex.split(os.environ.get('CC', 'clang')), '-std=gnu11', '-O2',
                        '-D_FILE_OFFSET_BITS=64', '-pthread', '-Iruntime', str(source), '-o', str(exe)],
                       cwd=root, check=True)
        save = temporary/'save'
        save.mkdir()
        subprocess.run([str(exe), str(save)], check=True, timeout=30)
