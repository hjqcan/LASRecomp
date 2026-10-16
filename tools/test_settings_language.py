"""Compile the settings harness, then test saved languages and Linux locales.

Requires a C compiler (CC, default clang; on Windows use the MSYS2 toolchain).
Only temporary configuration files are used; no game files are needed.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    exe = directory / ('settings-test.exe' if os.name == 'nt' else 'settings-test')
    cc = shlex.split(os.environ.get('CC', 'clang'))
    subprocess.run(cc + ['-std=gnu11', '-Iruntime', 'runtime/settings.c', 'tools/test_settings_language.c',
                         '-o', str(exe)], cwd=root, check=True)
    cases = [('zh-CN', '', 'zh-Hans', 'Chinese (Simplified)'),
             ('zh-TW', '', 'zh-Hant', 'Chinese (Traditional)'),
             ('zh-Hans', '', 'zh-Hans', 'Chinese (Simplified)'),
             ('zh-Hant', '', 'zh-Hant', 'Chinese (Traditional)'),
             ('en-US', '', 'en-US', 'English'), ('ja', '', 'ja', 'Japanese'),
             ('unknown', '', 'en-US', 'English')]
    if sys.platform.startswith('linux'):
        for locale in ('zh_CN.UTF-8', 'zh_SG.UTF-8', 'zh-Hans-CN'):
            cases.append(('auto', locale, 'zh-Hans', 'Chinese (Simplified)'))
        for locale in ('zh_TW.UTF-8', 'zh_HK.UTF-8', 'zh_MO.UTF-8', 'zh-Hant-TW'):
            cases.append(('auto', locale, 'zh-Hant', 'Chinese (Traditional)'))
    for newline in ('\n', '\r\n'):
        for setting, locale, code, name in cases:
            ini = directory / 'test.ini'
            ini.write_bytes(f'language = {setting}{newline}'.encode('utf-8'))
            env = dict(os.environ, LARECOMP_SETTINGS=str(ini), LC_ALL=locale, LC_MESSAGES='', LANG='C')
            subprocess.run([str(exe), code, name], env=env, check=True)
            saved = ini.read_text(encoding='utf-8')
            expected = 'auto' if setting in ('auto', 'unknown') else code
            assert f'language = {expected}\n' in saved, (setting, saved)
    print(f'{len(cases) * 2} language settings cases passed (LF and CRLF)')
