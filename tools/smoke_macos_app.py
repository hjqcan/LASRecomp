#!/usr/bin/env python3
"""Start the finished .app without game files or a window; check dyld reached main."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

# FORCE_LAUNCHER enters main's NO_LAUNCHER guard, which exits 3 before UI/game startup.
GUARD = '[main] LARECOMP_NO_LAUNCHER is set: not showing the start screen - exiting'


def check(app):
    exe = Path(app).resolve() / 'Contents/MacOS/lasrecomp'
    with tempfile.TemporaryDirectory(prefix='las-macos-smoke-') as scratch:
        root = Path(scratch)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(('LARECOMP_', 'LASRECOMP_', 'DYLD_'))}
        env.update(HOME=scratch, LARECOMP_DATA_ROOT=scratch, LARECOMP_DATA_HOME=scratch,
                   LARECOMP_SETTINGS=str(root / 'settings.ini'), LARECOMP_SAVE_DIR=str(root / 'save'),
                   LARECOMP_NO_LAUNCHER='1', LARECOMP_FORCE_LAUNCHER='1',
                   LARECOMP_NO_UPDATE_CHECK='1', DYLD_BIND_AT_LAUNCH='1')
        result = subprocess.run([str(exe)], cwd=scratch, env=env, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                errors='replace', timeout=20)
        output = result.stdout
        log = root / 'lasrecomp.log'  # macOS redirects a bundled executable's output here.
        if log.is_file():
            output += log.read_text(errors='replace')
        print(output, end='')
        if result.returncode != 3 or GUARD not in output:
            raise RuntimeError(f'macOS app did not reach the headless startup guard (exit {result.returncode})')
    print('macOS app dynamic-library loading passed (headless startup guard reached).')


if __name__ == '__main__':
    try:
        check(sys.argv[1] if len(sys.argv) > 1 else 'build/Multi.app')
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as error:
        sys.exit(f'macOS app smoke test failed: {error}')
