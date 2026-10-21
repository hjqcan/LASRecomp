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
    subprocess.run(cc + ['-std=gnu11', '-Iruntime', 'runtime/settings.c', 'runtime/cosmetic_colour.c', 'tools/test_settings_language.c',
                         '-lm', '-o', str(exe)], cwd=root, check=True)
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
    for newline in ('\n', '\r\n'):
        for profile, expected in [('existing', 0), ('1.0.0', 1), ('1.1.0', 2), ('acnh/1.0.0', 3), ('../outside', 0)]:
            ini.write_bytes(f'language = en-US{newline}game_profile = {profile}{newline}'.encode())
            subprocess.run([str(exe), 'en-US', 'English', str(expected)], env=env, check=True)
            saved_profile = ('existing', '1.0.0', '1.1.0', 'acnh/1.0.0')[expected]
            assert f'game_profile = {saved_profile}\n' in ini.read_text()
    print('10 installation selection load/save cases passed (LF and CRLF)')
