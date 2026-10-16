"""Compile/run the production cache logic with synthetic inputs."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root=Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp).resolve()
    exe=tmp/('cache-test.exe' if os.name=='nt' else 'cache-test')
    cxx=shlex.split(os.environ.get('CXX','clang++'))
    subprocess.run(cxx+['-x','c++','-std=c++17','-pthread','-Iruntime',
                        'runtime/ui/build_cache.mm','tools/test_build_cache.cpp',
                        '-o',str(exe)],cwd=root,check=True)
    subprocess.run([str(exe),str(tmp/'data')],check=True)
