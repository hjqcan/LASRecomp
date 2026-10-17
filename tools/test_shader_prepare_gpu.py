"""Run the synthetic Vulkan preparation test on an available Vulkan 1.3 GPU."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='las-shader-gpu-') as folder:
    exe=Path(folder)/('shader-gpu.exe' if os.name=='nt' else 'shader-gpu')
    if os.name=='nt':
        libs=['-lvulkan-1','-lshaderc_shared']
    else:
        try:
            shaderc=shlex.split(subprocess.check_output(['pkg-config','--libs','shaderc'],text=True))
        except (FileNotFoundError,subprocess.CalledProcessError):
            shaderc=['-lshaderc']
        libs=['-lvulkan',*shaderc]
    subprocess.run([*shlex.split(os.environ.get('CC','clang')),'-std=gnu11','-O2','-pthread',
                    str(root/'tools/test_shader_prepare_gpu.c'),str(root/'runtime/nvn/vulkan/shaders.c'),
                    str(root/'runtime/nvn/vulkan/shader_jobs.c'),*libs,'-o',str(exe)],check=True)
    subprocess.run([str(exe)],cwd=folder,check=True,timeout=60)
