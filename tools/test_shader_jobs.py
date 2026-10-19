"""Exercise the production compilation queue without game assets or a GPU."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='las-shader-jobs-') as folder:
    exe = Path(folder) / ('jobs.exe' if os.name == 'nt' else 'jobs')
    flags = shlex.split(os.environ.get('SHADER_JOB_CFLAGS', ''))
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-O2', '-pthread',
                    *flags, str(root/'tools/test_shader_jobs.c'),
                    str(root/'runtime/nvn/vulkan/shader_jobs.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
