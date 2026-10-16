"""Optional local two-version gameplay regression. Uses only isolated settings/saves.

python tools/compatibility_smoke.py --exe /path/to/lasrecomp --base-root /path/to/v1.0.0 \
    --updated-root /path/to/v1.1.0 --output /path/to/new-results

Each root needs its own compatible compiled game library. No game files, saves,
keys or screenshots are uploaded. Inspect the resulting images as well as JSON.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time

from game_versions import BASE_ID, UPDATE_ID

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--exe', type=Path, required=True)
parser.add_argument('--base-root', type=Path)
parser.add_argument('--updated-root', type=Path)
parser.add_argument('--output', type=Path, required=True, help='New, empty results directory')
parser.add_argument('--save-copy', type=Path, help='Optional save mount copied into each isolated test, never modified')
parser.add_argument('--frames', type=int, default=2400)
parser.add_argument('--menu', action='store_true', help='Capture the Controls overlay near the end of each run')
args = parser.parse_args()
if not (args.base_root or args.updated_root) or not 1000 <= args.frames <= 20000:
    parser.error('Choose a game root and 1000..20000 frames')
exe = args.exe.resolve(strict=True)
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
results = []
for version, root, build in [('1.0.0', args.base_root, BASE_ID), ('1.1.0', args.updated_root, UPDATE_ID)]:
    if not root:
        continue
    root = root.resolve(strict=True)
    with (root/'exefs/main').open('rb') as f:
        header = f.read(0x60)
    if header[:4] != b'NSO0' or header[0x40:0x54].hex() != build:
        raise ValueError(f'{version}: unexpected main executable')
    case = output/version
    (case/'build').mkdir(parents=True)
    (case/'save').mkdir()
    if args.save_copy:
        shutil.copytree(args.save_copy.resolve(strict=True), case/'save/save_0')
    (case/'larecomp.ini').write_text('language = zh-Hans\n', encoding='utf-8')
    env = {k: v for k, v in os.environ.items() if not k.startswith('LARECOMP_')}
    env.update(LARECOMP_DATA_ROOT=str(case), LARECOMP_SAVE_DIR=str(case/'save'),
               LARECOMP_SETTINGS=str(case/'larecomp.ini'), LARECOMP_NO_UPDATE_CHECK='1',
               LARECOMP_WINDOW_SHOT=str(args.frames), LARECOMP_EXIT_AFTER_SHOT='1',
               LARECOMP_KBD_AUTO='Link', NO_AUDIO='1', NO_MOVIE_AUDIO='1',
               LARECOMP_INPUT='200:A:8,600:L:12,600:R:12,700:A:8,900:L:12,900:R:12,1000:A:8,'
                               '1300:A:8,1450:DOWN:8,1600:A:8,1900:A:8,2200:A:8')
    if args.menu:
        env.update(LARECOMP_MENU_AT=str(args.frames-60), LARECOMP_MENU_TAB='8')
    startup = None
    if os.name == 'nt':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
    started = time.monotonic()
    with (case/'stdout.log').open('wb') as out, (case/'stderr.log').open('wb') as err:
        process = subprocess.Popen([str(exe), str(root), '--play'], cwd=exe.parent, env=env,
                                   stdout=out, stderr=err, startupinfo=startup,
                                   start_new_session=os.name != 'nt')
        try:
            code = process.wait(timeout=max(180, args.frames/15+60))
        except subprocess.TimeoutExpired:
            if os.name == 'nt':
                subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], check=False)
            else:
                os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise RuntimeError(f'{version}: gameplay timed out') from None
    log = (case/'stderr.log').read_text(encoding='utf-8', errors='replace')
    screenshot = case/'build/window.png'
    fps = re.findall(r'([\d.]+) fps', log)
    passed = code == 0 and screenshot.is_file() and screenshot.stat().st_size > 1000 and f'[game] version {version}' in log
    result = dict(version=version, build_id=build, passed=passed, exit_code=code,
                  seconds=round(time.monotonic()-started, 1), frames=args.frames,
                  last_fps=float(fps[-1]) if fps else None,
                  screenshot=str(screenshot.relative_to(output)),
                  screenshot_sha256=hashlib.sha256(screenshot.read_bytes()).hexdigest() if screenshot.is_file() else None)
    results.append(result)
    (output/'report.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(json.dumps(result), flush=True)
    if not passed:
        raise RuntimeError(f'{version}: inspect the captured logs')
