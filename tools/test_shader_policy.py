"""Exercise the production Vulkan shader policy across titles and overrides."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / 'runtime/nvn/vulkan/exec.c').read_text(encoding='utf-8')
    start = source.index('static bool sync_shaders(void) {')
    end = source.index('\n}\n', start) + 3
    harness = '''
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
static bool las = true;
static bool title_is_las(void) { return las; }
'''
    harness += source[start:end]
    harness += '''
int main(void) {
    printf("%d ", sync_shaders());
    las = false;
    printf("%d ", sync_shaders());
    las = true;
    printf("%d\\n", sync_shaders());
}
'''
    with tempfile.TemporaryDirectory(prefix='las-shader-policy-') as temporary:
        folder = Path(temporary)
        src = folder / 'policy.c'
        exe = folder / ('policy.exe' if os.name == 'nt' else 'policy')
        src.write_text(harness, encoding='utf-8')
        subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11',
                        str(src), '-o', str(exe)], check=True)
        for value, expected in ((None, '0 0 0'), ('0', '0 0 0'),
                                ('1', '1 1 1'), ('2', '1 1 1')):
            env = dict(os.environ)
            env.pop('LARECOMP_SYNC_SHADERS', None)
            if value is not None:
                env['LARECOMP_SYNC_SHADERS'] = value
            result = subprocess.run([str(exe)], env=env, check=True,
                                    text=True, capture_output=True).stdout.strip()
            assert result == expected, (value, result, expected)
    print('Shader policy: background preparation for every title, switching and synchronous override passed')


if __name__ == '__main__':
    main()
