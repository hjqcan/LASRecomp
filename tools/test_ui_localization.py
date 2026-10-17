"""Validate the catalog, then compile/run the real UI localization code without game files.

CXX defaults to clang++. Pass --font to also verify every translated character in
an installed CJK font (same font selection as the running application).
"""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parent.parent
catalog = root / 'runtime/ui/localization_strings.h'
keys = set()
printf = re.compile(r'%(?:[-+0#]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn]|%)')
for line in catalog.read_text(encoding='utf-8').splitlines():
    if not line.startswith('UI_TEXT('):
        continue
    en, hans, hant = json.loads('[' + line[len('UI_TEXT('):-1] + ']')
    assert en not in keys, f'Duplicate key: {en}'
    keys.add(en)
    for translated in (hans, hant):
        assert translated and '\ufffd' not in translated, en
        assert printf.findall(en) == printf.findall(translated), f'Printf mismatch: {en}'
        assert '###' not in translated or '###' in en, f'Unexpected ImGui ID: {en}'

with tempfile.TemporaryDirectory() as temporary:
    exe = Path(temporary) / ('ui-test.exe' if os.name == 'nt' else 'ui-test')
    cxx = shlex.split(os.environ.get('CXX', 'clang++'))
    imgui = root / 'third_party/imgui'
    sources = ['runtime/ui/localization.mm', 'tools/test_ui_localization.cpp']
    sources += [str(imgui / f'{name}.cpp') for name in ('imgui', 'imgui_draw', 'imgui_tables', 'imgui_widgets')]
    subprocess.run(cxx + ['-x', 'c++', '-std=c++17', '-Iruntime', '-I' + str(imgui),
                          *sources, '-o', str(exe)], cwd=root, check=True)
    subprocess.run([str(exe)] + (['--font'] if '--font' in sys.argv else []), cwd=root, check=True)
    if os.name == 'nt':
        # Windows may briefly retain the finished executable's image mapping.
        # Retry this one file inside our temporary directory before rmtree.
        assert exe.parent == Path(temporary)
        for attempt in range(60):
            try:
                exe.unlink()
                break
            except PermissionError:
                if attempt == 59:
                    raise
                time.sleep(0.05)
