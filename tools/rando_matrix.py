"""Generate an isolated, reproducible randomizer matrix from the user's game files.

Generation and cross-version logic equality are reported separately from gameplay.
This tool never marks a seed as playthrough-verified or copies personal saves.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

from game_versions import BASE_ID, UPDATE_ID

CASES = {
    'default': {},
    'keys': {'keysanity': 'all'},
    'shops': {'shopsanity': 'random', 'trendsanity': True},
    'digs': {'digs': True},
    'souls': {'boss-souls': True, 'miniboss-souls': True},
    'minibosses': {'randomize-enemies': True, 'enemy-minibosses': True},
    'entrances': {'entrance-tier': 2, 'shuffle-dungeons': True},
    'entrances-decoupled': {'entrance-tier': 2, 'entrance-decoupled': True, 'shuffle-dungeons': True},
    'rooms-coherent': {'dungeon-room-shuffle': 'coherent', 'shuffle-dungeons': True},
    'rooms-chaos': {'dungeon-room-shuffle': 'chaos'},
    'rooms-madness': {'dungeon-room-shuffle': 'madness'},
    'combined': {'keysanity': 'all', 'shopsanity': 'random', 'digs': True,
                 'boss-souls': True, 'miniboss-souls': True, 'randomize-enemies': True,
                 'enemy-minibosses': True, 'hookshot-anything': True, 'shuffle-dungeons': True},
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generator', type=Path, required=True, help='Built rando-gen executable')
    parser.add_argument('--game100', type=Path, required=True)
    parser.add_argument('--game110', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New directory outside the source installations')
    parser.add_argument('--case', action='append', choices=CASES)
    parser.add_argument('--seed-prefix', default='EnhancedWorkshop')
    parser.add_argument('--jobs', type=int, choices=range(1, 5), default=2)
    parser.add_argument('--timeout', type=int, default=1200)
    args = parser.parse_args()
    generator = args.generator.resolve()
    games = {'1.0.0': args.game100.resolve(), '1.1.0': args.game110.resolve()}
    output = args.output.resolve()
    if not generator.is_file() or output.exists() or args.timeout < 1:
        parser.error('Expected an existing generator, positive timeout and a new output directory')
    if not args.seed_prefix or len(args.seed_prefix) > 40 or not all(c.isascii() and (c.isalnum() or c in '-_') for c in args.seed_prefix):
        parser.error('--seed-prefix must use at most 40 ASCII letters, digits, - or _')
    for (version, game), expected in zip(games.items(), (BASE_ID, UPDATE_ID)):
        with (game / 'exefs/main').open('rb') as stream:
            header = stream.read(0x60)
        if header[:4] != b'NSO0' or header[0x40:0x54].hex() != expected or not (game / 'romfs').is_dir():
            parser.error(f'{version}: incorrect executable Build ID or missing RomFS')
        if output.is_relative_to(game):
            parser.error('--output must be outside the source installations')
    output.mkdir(parents=True)
    cases = list(dict.fromkeys(args.case or CASES))
    report = {'format': 1, 'generator_sha256': hashlib.sha256(generator.read_bytes()).hexdigest(),
              'seed_prefix': args.seed_prefix, 'expected_cases': len(cases) * 2,
              'coverage': 'generation and logic parity only; gameplay and full playthrough not tested',
              'results': [], 'logic_parity': {}}

    def run(version, name):
        folder = output / version / name
        folder.mkdir(parents=True)
        settings = {'seed': args.seed_prefix + '_' + name, 'game-version': version, **CASES[name]}
        config = folder / 'request.json'
        config.write_text(json.dumps(settings, indent=2) + '\n', encoding='utf-8')
        seed = folder / 'seed'
        started = time.monotonic()
        result = {'version': version, 'case': name, 'passed': False, 'gameplay': 'not_run'}
        try:
            options = {}
            if os.name == 'nt':
                startup = subprocess.STARTUPINFO()
                startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                startup.wShowWindow = subprocess.SW_HIDE
                options['startupinfo'] = startup
            with (folder / 'generation.log').open('wb') as log:
                process = subprocess.run([str(generator), str(games[version] / 'romfs'), str(seed), str(config)],
                                         stdout=log, stderr=log, timeout=args.timeout,
                                         env=dict(os.environ, PYTHONUTF8='1'), **options)
            result['exit_code'] = process.returncode
            required = ['game-version.txt', 'settings.json', 'logic.json', 'spheres.json', 'tracker.json', 'tracker.tsv', 'spoiler.txt']
            missing = [p for p in required if not (seed / p).is_file() or not (seed / p).stat().st_size]
            result['missing'] = missing
            if not missing and process.returncode == 0:
                marker = (seed / 'game-version.txt').read_text().splitlines()
                actual = json.loads((seed / 'settings.json').read_text(encoding='utf-8'))
                result['options_match'] = all(actual.get(key) == value for key, value in settings.items() if key != 'game-version')
                result['passed'] = marker == ['LASRANDO1', version] and result['options_match']
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            result['error'] = str(error)
        result['seconds'] = round(time.monotonic() - started, 2)
        return result

    def save_report():
        temporary = output / 'report.writing'
        temporary.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        temporary.replace(output / 'report.json')

    save_report()
    with ThreadPoolExecutor(args.jobs) as pool:
        futures = [pool.submit(run, version, name) for name in cases for version in games]
        for future in as_completed(futures):
            result = future.result()
            report['results'].append(result)
            print(json.dumps(result), flush=True)
            save_report()
    for name in cases:
        pair = [r for r in report['results'] if r['case'] == name]
        if not all(r['passed'] for r in pair):
            report['logic_parity'][name] = 'unavailable'
            continue
        equal = {}
        for filename in ('logic.json', 'spheres.json'):
            contents = [json.loads((output / v / name / 'seed' / filename).read_text(encoding='utf-8')) for v in games]
            equal[filename] = contents[0] == contents[1]
        report['logic_parity'][name] = equal
    report['generation_passed'] = all(r['passed'] for r in report['results'])
    report['parity_passed'] = all(isinstance(p, dict) and all(p.values()) for p in report['logic_parity'].values())
    save_report()
    print(json.dumps({'generation_passed': report['generation_passed'], 'parity_passed': report['parity_passed'], 'report': str(output / 'report.json')}))
    if not report['generation_passed'] or not report['parity_passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
