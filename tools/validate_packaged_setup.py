"""Opt-in Windows integration check using the player's local archive and a fresh data folder.

Runs the actual packaged launcher/importer/compiler. It does not certify gameplay,
modify an existing installation, or write shortcuts to the user's desktop.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import time

from game_catalog import GAMES, inspect_exefs
from host_paths import io_path


def run_phase(app, data, game, phase, options, timeout):
    # A developer's external generator/compiler/library overrides must not turn
    # a packaged test into a source-tree test, or route writes to personal saves.
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('LARECOMP_', 'ACNH_'))}
    env.update(LARECOMP_NO_UPDATE_CHECK='1', LARECOMP_NO_USER_INPUT='1',
               LARECOMP_SHORTCUT_DIR=str(data / 'test-desktop'))
    env.update(options)
    log = data / f'{phase}.log'
    started = time.monotonic()
    with log.open('wb') as stream:
        child = subprocess.Popen([str(app / 'lasrecomp.exe'), '--data-root', str(data), '--game', game],
                                 cwd=data, env=env, stdout=stream, stderr=stream,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
        print(f'{phase}: PID {child.pid}; log {log}', flush=True)
        try:
            status = child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            # Only this launched runtime. Its per-helper jobs close with it.
            child.terminate(); child.wait(timeout=30)
            raise RuntimeError(f'{phase} exceeded {timeout}s; candidate kept at {data}')
    with log.open('rb') as stream:
        stream.seek(max(0, log.stat().st_size - 65536))
        tail = stream.read().decode('utf-8', errors='replace')
    summaries = re.findall(r'^\[launcher\] setup: (.+)$', tail, re.M)
    result = dict(phase=phase, exit_code=status, seconds=round(time.monotonic()-started, 2),
                  log=log.name, summary=summaries[-1].strip() if summaries else None)
    print(json.dumps(result), flush=True)
    return result


def published(data, game, version):
    slot = data / version.installation['directory']
    meta = slot / '.installations'
    pointer = (meta / 'current').read_text(encoding='utf-8').splitlines()
    if len(pointer) != 2 or pointer[0] != 'LASINSTALL2' or not re.fullmatch(r'build-[0-9a-f-]{1,73}', pointer[1]):
        raise ValueError('Invalid published installation pointer')
    root = io_path(meta / pointer[1] / 'content')
    plan = inspect_exefs(root / 'exefs')
    if plan['game_id'] != game.id or plan['version'] != version.version:
        raise ValueError('Published modules belong to another game/version')
    receipt = (root / 'installation-receipt').read_text(encoding='utf-8').splitlines()
    if len(receipt) != 3 or receipt[:2] != ['LASRECEIPT1', game.id + '/' + version.version] or not re.fullmatch('[0-9a-f]{64}', receipt[2]):
        raise ValueError('Missing or mismatched publication receipt')
    identity = (root / 'lasgame.dll.id').read_text(encoding='utf-8').splitlines()
    if len(identity) != 3 or identity[1:] != [plan['modules'][0]['build_id'], plan['code_identity']]:
        raise ValueError('Published library identities differ from the current adapter')
    if not (root / 'lasgame.dll').is_file() or (meta / 'pending').exists():
        raise ValueError('Installation was not fully published')
    shortcuts = sorted((data / 'test-desktop').glob('*.lnk'))
    if len(shortcuts) != 1:
        raise ValueError('Expected exactly one game shortcut on the scratch desktop')
    manifest = json.loads((root / 'import-manifest.json').read_text(encoding='utf-8'))
    return dict(generation=pointer[1], launch_id=game.id+'/'+version.version, core_id=identity[0],
                code_identity=identity[2], content_receipt=receipt[2], romfs_files=manifest['romfs_files'],
                shortcut=shortcuts[0].name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True, type=Path, help='Unpacked Windows package directory')
    parser.add_argument('--game', required=True, choices=[g.id+'/'+r.version for g in GAMES for r in g.releases])
    parser.add_argument('--base', required=True, type=Path)
    parser.add_argument('--update', type=Path)
    parser.add_argument('--keys', type=Path)
    parser.add_argument('--title-keys', type=Path)
    parser.add_argument('--data-root', required=True, type=Path, help='A new, isolated directory; must not exist')
    parser.add_argument('--cancel-after', type=float, help='Cancel first build after this many seconds, then resume')
    parser.add_argument('--minimized', action='store_true', help='Also exercise setup with a minimized window')
    parser.add_argument('--timeout', type=int, default=3600, help='Maximum seconds per setup attempt')
    args = parser.parse_args()
    if os.name != 'nt': parser.error('This check requires Windows')
    if args.timeout <= 0 or (args.cancel_after is not None and (not math.isfinite(args.cancel_after) or args.cancel_after <= 0)):
        parser.error('Timeout and cancellation delay must be positive')
    app = args.app.resolve(strict=True)
    executable = app / 'lasrecomp.exe'
    if not executable.is_file(): parser.error('The package has no lasrecomp.exe')
    game_id, version_id = args.game.split('/', 1)
    game = next(g for g in GAMES if g.id == game_id)
    version = next(r for r in game.releases if r.version == version_id)
    if not version.installable: parser.error('This release is still gated as development; use the port workflow')
    options = dict(LARECOMP_LAUNCHER_EXTRACT=str(args.base.resolve(strict=True)))
    for value, name in ((args.update, 'LARECOMP_LAUNCHER_UPDATE_NSZ'), (args.keys, 'LARECOMP_IMPORT_KEYS'),
                        (args.title_keys, 'LARECOMP_IMPORT_TITLE_KEYS')):
        if value: options[name] = str(value.resolve(strict=True))
    common = {'LARECOMP_LAUNCHER_MINIMIZED': '1'} if args.minimized else {}
    if args.cancel_after is not None: options['LARECOMP_BUILD_CANCEL_AFTER'] = str(args.cancel_after)
    data = args.data_root.resolve()
    data.mkdir(parents=True, exist_ok=False)
    (data / 'test-desktop').mkdir()
    report = dict(schema=1, game=args.game, runtime_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                  minimized=args.minimized, attempts=[], result='incomplete', gameplay='not tested', save_reopen='not tested')
    try:
        result = run_phase(app, data, args.game, 'setup', common | options, args.timeout)
        report['attempts'].append(result)
        if args.cancel_after is not None:
            if result['exit_code'] != 1 or not (result['summary'] or '').startswith('Compilation cancelled.'):
                raise ValueError('Setup did not reach the requested cancellation; inspect setup.log')
            meta = data / version.installation['directory'] / '.installations'
            if (meta / 'current').exists() or not (meta / 'pending').is_file() or list((data / 'test-desktop').iterdir()):
                raise ValueError('Cancelled first install was published or did not preserve a candidate')
            result = run_phase(app, data, args.game, 'resume', common | {'LARECOMP_LAUNCHER_BUILD_ONLY': '1'}, args.timeout)
            report['attempts'].append(result)
        if result['exit_code'] != 0 or not (result['summary'] or '').endswith(' -> READY'):
            raise ValueError('The packaged setup failed; inspect the recorded log')
        report['installation'] = published(data, game, version)
        report['result'] = 'passed'
    except (OSError, ValueError, RuntimeError) as error:
        report['result'] = 'failed'; report['error'] = str(error)
    finally:
        (data / 'setup-validation.json').write_text(json.dumps(report, indent=2, ensure_ascii=False)+'\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report['result'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
