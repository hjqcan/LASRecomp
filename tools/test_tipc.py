"""Compile and run the real IPC codec with synthetic clients and handles."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temporary:
    exe=Path(temporary)/('tipc.exe' if os.name=='nt' else 'tipc')
    subprocess.run(shlex.split(os.environ.get('CC','clang'))+['-std=gnu11','-O2','-Wno-incompatible-library-redeclaration',
                    str(ROOT/'tools/test_tipc.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: TIPC payloads, handles, A/B/W buffers, service registration, bounds and CMIF compatibility')
