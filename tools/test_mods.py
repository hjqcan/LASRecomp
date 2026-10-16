import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary).resolve()
    exe = root/('test.exe' if os.name == 'nt' else 'test')
    subprocess.run(shlex.split(os.environ.get('CXX','clang++')) + ['-std=c++17','-Wall','-Wextra','-x','c++',
                   '-I'+str(ROOT/'runtime'), str(ROOT/'tools/test_mods.cpp'), str(ROOT/'runtime/ui/mod_store.mm'),
                   '-o',str(exe)],check=True)
    subprocess.run([str(exe),str(root/'fixtures')],check=True)
