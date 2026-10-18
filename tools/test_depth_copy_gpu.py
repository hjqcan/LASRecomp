"""Check actual depth/stencil copies with synthetic images on a Vulkan 1.3 GPU."""
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='las-depth-copy-') as folder:
    exe=Path(folder)/('depth-copy.exe' if os.name=='nt' else 'depth-copy')
    libs=['-lvulkan-1','-lshaderc_shared'] if os.name=='nt' else ['-lvulkan',*shlex.split(subprocess.check_output(['pkg-config','--libs','shaderc'],text=True)),'-lm']
    subprocess.run([*shlex.split(os.environ.get('CC','clang')),'-std=gnu11','-O2',str(root/'tools/test_depth_copy_gpu.c'),*libs,'-o',str(exe)],check=True)
    subprocess.run([str(exe)],cwd=folder,check=True,timeout=60)
