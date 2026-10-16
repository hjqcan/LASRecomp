"""Exercise native challenge isolation, route capture and benchmark export with owned game files."""
import argparse
import configparser
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from game_versions import game_version


def tree_hash(path):
    return {p.relative_to(path).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in path.rglob('*') if p.is_file()}


def read_ini(path):
    parser = configparser.ConfigParser()
    parser.read_string('[data]\n' + path.read_text(encoding='utf-8'))
    return parser['data']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--save-copy', type=Path, required=True, help='Version-compatible save_0 folder; copied only')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--result', choices=('win', 'timeout'), default='timeout')
    parser.add_argument('--language', default='en-US')
    parser.add_argument('--view-route', action='store_true')
    parser.add_argument('--seconds', type=int, default=30, help='Timeout challenge duration (10-90)')
    parser.add_argument('--movement', action='store_true', help='Longer input script; checks that X/Z positions change')
    args = parser.parse_args()
    exe, game, library, output = (p.resolve() for p in (args.exe, args.game, args.library, args.output))
    if output.exists() or not exe.is_file() or not library.is_file() or not args.save_copy.is_dir():
        parser.error('Expected a new output directory and existing runtime, library and save copy')
    if not 10 <= args.seconds <= 90 or (args.movement and args.seconds < 60):
        parser.error('Use 10-90 seconds, or at least 60 for --movement')
    with (game / 'exefs/main').open('rb') as stream:
        version = game_version(stream.read(0x60)[0x40:0x54].hex())
    source_before = tree_hash(args.save_copy)
    output.mkdir(parents=True)
    (output / 'build').mkdir()
    source = output / 'source-save'
    shutil.copytree(args.save_copy, source / 'save_0')
    snapshot_before = tree_hash(source)
    goal = 'MarinTarinHouse' if args.result == 'win' else 'Lv09WindFishsEgg'
    preset = output / 'smoke.laschallenge'
    preset.write_text(f'format=1\nname=smoke-{args.result}\ngame_version={version}\ngoal_level={goal}\nseconds={args.seconds}\ndamage=2\nno_hit=0\nohko=0\n', encoding='utf-8')
    settings = output / 'settings.ini'
    if args.language not in ('en-US', 'zh-Hans', 'zh-Hant'):
        parser.error('--language must be en-US, zh-Hans or zh-Hant')
    settings.write_text(f'language = {args.language}\nfullscreen = 0\nmenu_tip_seen = 1\naspect_asked = 1\n', encoding='utf-8')
    env = {k: v for k, v in os.environ.items() if not k.startswith('LARECOMP_')}
    env.update(LARECOMP_DATA_ROOT=str(output), LARECOMP_SETTINGS=str(settings),
               LARECOMP_SAVE_DIR=str(source), LARECOMP_GAME_LIB=str(library),
               LARECOMP_CHALLENGE_PRESET=str(preset), LARECOMP_BENCHMARK_AT='1',
               LARECOMP_NO_UPDATE_CHECK='1', LARECOMP_KBD_AUTO='Link',
               LARECOMP_WINDOW_SHOT='4300', LARECOMP_SHOT_PATH=str(output / 'workshop.png'),
               LARECOMP_EXIT_AFTER_SHOT='1', LARECOMP_MENU_AT='4100', LARECOMP_MENU_TAB='12',
               LARECOMP_INPUT='200:A:8,600:L:12,600:R:12,700:A:8,900:L:12,900:R:12,1000:A:8,'
                              '1300:A:8,1450:DOWN:8,1600:A:8,1900:A:8,2200:A:8,2500:A:8,'
                              '2800:A:8,3000:A:8,3100:A:8,3200:A:8,3300:STICK_DOWN:180')
    if args.view_route:
        env.update(LARECOMP_WORKSHOP_ROUTE='@latest', LARECOMP_MENU_SCROLL='1')
    if args.seconds > 30:
        last = max(4300, args.seconds * 60 + 2400)
        env.update(LARECOMP_WINDOW_SHOT=str(last), LARECOMP_MENU_AT=str(last - 200))
    if args.movement:
        env['LARECOMP_INPUT'] += ',' + ','.join(f'{frame}:A:8' for frame in range(3400, 4400, 100))
        env['LARECOMP_INPUT'] += ',4400:STICK_DOWN:180,4700:STICK_RIGHT:180,5000:STICK_UP:180'
    options = {}
    if os.name == 'nt':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
        options['startupinfo'] = startup
    with (output / 'runtime.log').open('wb') as log:
        process = subprocess.run([str(exe), str(game), '--play'], env=env, cwd=exe.parent,
                                 stdout=log, stderr=log, timeout=300, **options)
    runs = list((output / 'workshop/runs').glob('run-*'))
    report = {'exit_code': process.returncode, 'version': version, 'expected': args.result,
              'source_unchanged': source_before == tree_hash(args.save_copy) and snapshot_before == tree_hash(source),
              'run_count': len(runs), 'passed': False}
    if len(runs) == 1 and (runs[0] / 'route.csv').is_file():
        result = dict(read_ini(runs[0] / 'result.ini'))
        with (runs[0] / 'route.csv').open(newline='') as stream:
            samples = list(csv.DictReader(stream))
        benchmark = list((output / 'benchmarks').glob('*/summary.ini'))
        report.update(result=result, samples=len(samples), position_samples=sum(s['position'] == '1' for s in samples),
                      benchmark_exports=len(benchmark))
        positions = {(s['level'], round(float(s['x']), 2), round(float(s['z']), 2)) for s in samples if s['position'] == '1'}
        report['distinct_positions'] = len(positions)
        expected_status = '3' if args.result == 'win' else '4'
        log = (output / 'runtime.log').read_text(encoding='utf-8', errors='replace')
        report['passed'] = (process.returncode == 0 and report['source_unchanged'] and result['status'] == expected_status
                            and bool(samples) and len(benchmark) == 1 and '[fs] mounted host directory ' in log
                            and '/workshop/runs/' in log and 'level MarinTarinHouse' in log
                            and (output / 'workshop.png').is_file())
        if args.result == 'timeout':
            report['passed'] &= result['reason'] == '1' and float(result['elapsed_seconds']) >= args.seconds
        if args.movement:
            report['passed'] &= len(positions) > 1
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
