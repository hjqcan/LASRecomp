"""Native settings regression checks; no game files or player settings are used."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    exe = directory / ('settings-test.exe' if os.name == 'nt' else 'settings-test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-Iruntime', 'runtime/settings.c', 'tools/test_settings.c', '-o', str(exe)],
                   cwd=ROOT, check=True)
    cases = [(setting, {}, code) for setting, code in
             [('zh-CN', 'zh-Hans'), ('zh-TW', 'zh-Hant'), ('zh-Hans', 'zh-Hans'),
              ('zh-Hant', 'zh-Hant'), ('en-US', 'en-US'), ('ja', 'ja'), ('unknown', 'en-US')]]
    if sys.platform.startswith('linux'):
        for locale in ['zh_CN.UTF-8', 'zh_SG.UTF-8', 'zh-Hans', 'zh-Hans-HK', 'zh_Hans_CN', 'zh']:
            cases.append(('auto', {'LC_ALL': locale}, 'zh-Hans'))
        for locale in ['zh_TW.UTF-8', 'zh_HK.UTF-8', 'zh_MO.UTF-8', 'zh-HK', 'zh-MO',
                       'zh-Hant', 'zh-Hant-TW', 'zh_Hant_CN', 'zh_hant', 'zh_hk.UTF-8']:
            cases.append(('auto', {'LC_ALL': locale}, 'zh-Hant'))
        cases += [('auto', {'LC_MESSAGES': 'zh_HK.UTF-8', 'LANG': 'zh_CN.UTF-8'}, 'zh-Hant'),
                  ('auto', {'LANG': 'zh_MO.UTF-8'}, 'zh-Hant'),
                  ('auto', {'LC_ALL': 'zh_CN.UTF-8', 'LC_MESSAGES': 'zh_HK.UTF-8'}, 'zh-Hans'),
                  ('auto', {'LC_ALL': 'ja_JP.UTF-8'}, 'ja'), ('auto', {'LC_ALL': 'C'}, 'en-US')]
    count = 0
    for newline in ['\n', '\r\n']:
        for setting, locale, code in cases:
            for seed in ['Regression seed', '']:
                ini = directory / 'test.ini'
                lines = [f'language = {setting}', 'aspect = original', 'upscaler = bilinear',
                         'prompts = xbox', 'volume = 1.25', 'volume =', 'language =', f'rando_seed = {seed}']
                ini.write_bytes((newline.join(lines) + newline).encode())
                env = dict(os.environ, LARECOMP_SETTINGS=str(ini), LC_ALL='', LC_MESSAGES='', LANG='C')
                env.update(locale)
                subprocess.run([str(exe), code, seed], env=env, check=True, capture_output=True)
                saved = ini.read_text()
                expected = 'auto' if setting in ['auto', 'unknown'] else code
                assert f'language = {expected}\n' in saved
                assert f'rando_seed = {seed}\n' in saved
                count += 1
    print(f'{count} parser, locale and roundtrip cases passed (LF/CRLF, empty/nonempty seeds)')
