"""Exit failures must survive the renderer's atexit -> OS termination path."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
platform = 'windows' if os.name == 'nt' else 'linux' if sys.platform.startswith('linux') else None
terminate = ''
if platform:
    source = (root / f'runtime/platform/{platform}.c').read_text(encoding='utf-8')
    terminate = re.search(r'void plat_terminate\(int status\) \{[^}]+\}', source).group()
harness = '''
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
''' + terminate + '''
static void teardown(void) {
    puts("cleanup ran"); fflush(stdout);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate(plat_exit_status());
#endif
}
int main(int argc, char **argv) {
    if (argc != 2) return 99;
    atexit(teardown);
    plat_exit(atoi(argv[1]));
}
'''
with tempfile.TemporaryDirectory(prefix='exit-status-') as temp:
    temp = Path(temp); source = temp / 'exit.c'; source.write_text(harness, encoding='utf-8')
    exe = temp / ('exit.exe' if os.name == 'nt' else 'exit')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + ['-std=c11', '-Iruntime', str(source),
                   'runtime/process_exit.c', '-o', str(exe)], cwd=root, check=True)
    for status in (0, 1, 7):
        result = subprocess.run([str(exe), str(status)], capture_output=True, text=True, timeout=10)
        assert result.returncode == status, result
        assert result.stdout.strip() == 'cleanup ran'
print('PASS: success and failure statuses survive cleanup and immediate OS termination')
