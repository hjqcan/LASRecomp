"""Run an isolated LAS HID/camera/timing experiment; never certifies deterministic replay."""
import argparse
import configparser
import csv
import hashlib
import json
import math
import os
import re
from pathlib import Path
import shutil
import stat
import subprocess
from game_versions import game_version

SCENARIO_DIRECTIONS = ((1200, 0, -32767, 19), (1250, 32767, 0, 18),
                       (1300, 0, 32767, 17), (1350, -32767, 0, 16))
SCENARIOS = {'house': 'house_v5', 'house-exit': 'house_exit_v2'}
FIELD_DIRECTIONS = ((3600, 32767, 0, 18), (3700, 0, -32767, 19),
                    (3800, -32767, 0, 16), (3900, 0, 32767, 17))


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda: f.read(1024 * 1024), b''):
            h.update(data)
    return h.hexdigest()


def tree(path):
    paths = [path, *path.parents, *sorted(path.rglob('*'))]
    for p in paths:
        if p.is_symlink() or getattr(p.lstat(), 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0):
            raise ValueError('Save fixtures cannot contain linked/reparse paths')
    files = [p for p in paths if p != path and p.is_relative_to(path) and p.is_file()]
    if len(files) > 512 or sum(p.stat().st_size for p in files) > 64 * 1024 * 1024:
        raise ValueError('Save fixture exceeds 512 files or 64 MiB')
    return {p.relative_to(path).as_posix(): sha(p) for p in files}


def ini(path):
    p = configparser.ConfigParser()
    p.read_string('[data]\n' + path.read_text(encoding='utf-8'))
    return dict(p['data'])


def update_driver(path, build_id, count, scenario='house'):
    """A short, bounded opening-house script indexed by update dispatch, not host frames."""
    rows = []
    for step in range(count):
        buttons = 1 if step <= 847 and step % 60 < 8 else 0
        lx = ly = 0
        for start, x, y, bit in SCENARIO_DIRECTIONS:
            if start <= step < start + 25:
                lx, ly, buttons = x, y, buttons | (1 << bit)
        if scenario == 'house-exit':
            # Talk to Tarin to recover the shield before approaching the exit.
            # B advances dialogue without initiating another conversation afterwards.
            if 1700 <= step < 1708:
                buttons |= 1
            if 1760 <= step <= 2667 and (step - 1760) % 60 < 8:
                buttons |= 2
            route = ((1600, 32, 32767, 0, 18), (1640, 8, 0, -32767, 19),
                     (1660, 1, 32767, 0, 18), (2800, 18, -32767, 0, 16),
                     (2850, 140, 0, -32767, 19))
            for start, duration, x, y, bit in (*route, *((s, 40, x, y, b) for s, x, y, b in FIELD_DIRECTIONS)):
                if start <= step < start + duration:
                    lx, ly, buttons = x, y, buttons | (1 << bit)
        rows.append(f'{step},{buttons},{lx},{ly},0,0\n')
    path.write_text(f'LASINPUT2\nbuild_id={build_id}\nboundary=las_update_dispatch_v1\n'
                    'update,buttons,lx,ly,rx,ry\n' + ''.join(rows), encoding='utf-8')


def scenario_movement(rows):
    windows = []
    for (start, _, _, _), name in zip(SCENARIO_DIRECTIONS, ('down', 'right', 'up', 'left')):
        distances = []
        for step in range(start, min(start + 25, len(rows))):
            a, b = rows[step - 1], rows[step]
            if a['position'] == b['position'] == '1' and a['level'] == b['level']:
                distances.append(math.hypot(float(b['x']) - float(a['x']), float(b['z']) - float(a['z'])))
        windows.append({'direction': name, 'start_update': start, 'available_pairs': len(distances),
                        'changing_samples': sum(x > 1e-6 for x in distances), 'xz_path_length': sum(distances)})
    return {'windows': windows, 'all_windows_moved': all(w['changing_samples'] >= 5 for w in windows),
            'note': 'Movement coverage is separate from successful capture and does not certify deterministic replay.'}


def scenario_transition(rows):
    sequence = []
    field = []
    for row in rows:
        if row['observed'] != '1' or row['playing'] != '1' or not row['level']:
            continue
        if not sequence or sequence[-1]['level'] != row['level']:
            sequence.append({'update': int(row['update']), 'level': row['level']})
        if row['level'] == 'Field' and row['position'] == '1':
            field.append(row)
    movement = []
    for (start, _, _, _), name in zip(FIELD_DIRECTIONS, ('right', 'down', 'left', 'up')):
        distances = []
        for step in range(start, min(start + 40, len(rows))):
            a, b = rows[step-1], rows[step]
            if a['level'] == b['level'] == 'Field' and a['position'] == b['position'] == '1':
                distances.append(math.hypot(float(b['x'])-float(a['x']), float(b['z'])-float(a['z'])))
        movement.append({'direction': name, 'start_update': start, 'available_pairs': len(distances),
                         'changing_samples': sum(d > 1e-6 for d in distances), 'xz_path_length': sum(distances)})
    return {'level_sequence': sequence, 'field_position_samples': len(field), 'field_movement': movement,
            'all_field_windows_moved': all(w['changing_samples'] >= 5 for w in movement),
            'house_to_field_observed': any(a['level'] == 'MarinTarinHouse' and b['level'] == 'Field'
                                          for a, b in zip(sequence, sequence[1:])),
            'note': 'Observed level names and available positions; does not certify completed loading or combat.'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exe', type=Path, required=True)
    p.add_argument('--game', type=Path, required=True)
    p.add_argument('--library', type=Path, required=True)
    p.add_argument('--save-copy', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--replay', type=Path, help='Previous research_run output directory; fixture identities must match')
    p.add_argument('--hz', type=int, choices=(30, 60, 120), default=60)
    p.add_argument('--present', choices=('mailbox', 'fifo', 'immediate'), default='mailbox', help='Requested Vulkan mode; actual fallback is reported')
    p.add_argument('--pace', choices=('on', 'off'), default='on', help='Existing Vulkan frame-pacing heuristic')
    p.add_argument('--phases', action='store_true', help='Observe verified LAS update/input/task/timer callbacks')
    p.add_argument('--semantics', action='store_true', help='Read the guarded global RNG state and selected clock/entropy returns (update mode)')
    p.add_argument('--boundary', choices=('hid', 'update'), default='hid')
    p.add_argument('--scenario', choices=tuple(SCENARIOS), default='house', help='Update-indexed input driver; house-exit adds an exit and field movement')
    p.add_argument('--updates', type=int, help='Exact captured updates; default 2400 for house, 4800 for house-exit')
    p.add_argument('--start-level', default='MarinTarinHouse')
    p.add_argument('--warmup', type=int, default=240, help='Minimum consecutive valid updates at the start gate')
    p.add_argument('--stable', type=int, default=60, help='Consecutive equal player-position/health observations')
    p.add_argument('--frames', type=int, help='Presentation fallback limit; default 5700 for house, 9000 for house-exit')
    args = p.parse_args()
    if args.updates is None:
        args.updates = 4800 if args.scenario == 'house-exit' else 2400
    if args.frames is None:
        args.frames = 9000 if args.scenario == 'house-exit' else 5700
    if args.semantics and args.boundary != 'update':
        p.error('--semantics requires --boundary update')
    if args.scenario != 'house' and args.boundary != 'update':
        p.error('--scenario requires --boundary update')
    if not args.save_copy.is_dir():
        p.error('--save-copy must be an existing save mount directory')
    try:
        tree(args.save_copy.absolute()) # Check links before resolving the source path.
    except (OSError, ValueError) as error:
        p.error(str(error))
    exe, game, library, save, output = (v.resolve() for v in (args.exe, args.game, args.library, args.save_copy, args.output))
    if output.exists() or not all(x.is_file() for x in (exe, library, game / 'exefs/main')) or not save.is_dir():
        p.error('Use existing runtime/library/game/save files and a new output directory')
    if not 500 <= args.frames <= 12000 or any(output.is_relative_to(x) for x in (game, save)):
        p.error('Use 500-12000 frames and an output outside the input game/save directories')
    if (not 1 <= args.updates <= 12000 or not 0 <= args.warmup <= 3600 or not 1 <= args.stable <= 3600 or
            not re.fullmatch(r'[A-Za-z0-9_]{1,63}', args.start_level)):
        p.error('Invalid update count or start-gate limits')
    with (game / 'exefs/main').open('rb') as f:
        build_id = f.read(0x60)[0x40:0x54].hex()
    version = game_version(build_id)
    settings = 'language = en-US\nfullscreen = 0\nmenu_tip_seen = 1\naspect_asked = 1\n'
    fingerprint = {'runtime': sha(exe), 'library': sha(library), 'build_id': build_id,
                   'save': tree(save), 'settings': hashlib.sha256(settings.encode()).hexdigest()}
    if args.boundary == 'update':
        fingerprint['update_probe'] = {'boundary': 'las_update_dispatch_v1', 'scenario': SCENARIOS[args.scenario],
                                       'updates': args.updates, 'start_level': args.start_level,
                                       'warmup': args.warmup, 'stable': args.stable}
        if args.semantics:
            fingerprint['semantic_probe'] = 'las_rng_actors_v1'
    tape = None
    if args.replay:
        previous = args.replay.resolve()
        if json.loads((previous / 'fixture.json').read_text()) != fingerprint:
            p.error('Runtime, library, executable, source save or settings differ from the recording')
        candidates = list(previous.glob('workshop/runs/*/research/inputs.lastape'))
        if len(candidates) != 1:
            p.error('The replay source must contain exactly one input tape')
        tape = candidates[0]
    output.mkdir(parents=True)
    (output / 'build').mkdir()
    shutil.copytree(save, output / 'source-save/save_0')
    (output / 'fixture.json').write_text(json.dumps(fingerprint, indent=2) + '\n')
    (output / 'settings.ini').write_text(settings)
    preset = output / 'probe.laschallenge'
    preset.write_text(f'format=1\nname=research\ngame_version={version}\ngoal_level=Lv09WindFishsEgg\nseconds=3600\ndamage=1\nno_hit=0\nohko=0\n')
    env = {k: v for k, v in os.environ.items() if not k.startswith('LARECOMP_')}
    env.update(LARECOMP_DATA_ROOT=str(output), LARECOMP_SETTINGS=str(output / 'settings.ini'),
               LARECOMP_SAVE_DIR=str(output / 'source-save'), LARECOMP_GAME_LIB=str(library),
               LARECOMP_CHALLENGE_PRESET=str(preset), LARECOMP_NO_UPDATE_CHECK='1',
               LARECOMP_RESEARCH_HZ=str(args.hz), LARECOMP_KBD_AUTO='Link', LARECOMP_NO_USER_INPUT='1',
               LARECOMP_VK_PRESENT=args.present,
               LARECOMP_PACE='1' if args.pace == 'on' else '0',
               LARECOMP_WINDOW_SHOT=str(args.frames), LARECOMP_EXIT_AFTER_SHOT='1',
               LARECOMP_SHOT_PATH=str(output / 'scene.png'))
    if args.phases:
        env['LARECOMP_RESEARCH_PHASES'] = '1'
    if args.semantics:
        env['LARECOMP_RESEARCH_SEMANTICS'] = '1'
    if args.boundary == 'update':
        env.update(LARECOMP_RESEARCH_BOUNDARY='update', LARECOMP_RESEARCH_START_LEVEL=args.start_level,
                   LARECOMP_RESEARCH_WARMUP=str(args.warmup), LARECOMP_RESEARCH_STABLE=str(args.stable),
                   LARECOMP_RESEARCH_UPDATES=str(args.updates))
        # Bootstrap only. The runtime suppresses this input once the selected level appears.
        env['LARECOMP_INPUT'] = '200:A:8,600:L:12,600:R:12,700:A:8,900:L:12,900:R:12,1000:A:8'
        if not tape:
            driver = output / 'scenario.lastape'
            update_driver(driver, build_id, args.updates, args.scenario)
            env['LARECOMP_RESEARCH_DRIVER'] = str(driver)
    if tape:
        env['LARECOMP_RESEARCH_REPLAY'] = str(tape)
    else:
        env['LARECOMP_RESEARCH_CAPTURE'] = '1'
        if args.boundary == 'hid':
            env['LARECOMP_INPUT'] = ('200:A:8,600:L:12,600:R:12,700:A:8,900:L:12,900:R:12,1000:A:8,'
                                '1300:A:8,1450:DOWN:8,1600:A:8,1900:A:8,2200:A:8,2500:A:8,2800:A:8,'
                                '3000:A:8,3100:A:8,3200:A:8,3300:STICK_DOWN:180,' +
                                ','.join(f'{f}:A:8' for f in range(3400, 4400, 100)) +
                                ',4400:STICK_DOWN:180,4700:STICK_RIGHT:180,5000:STICK_UP:180')
    options = {}
    if os.name == 'nt':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
        options['startupinfo'] = startup
    with (output / 'runtime.log').open('wb') as log:
        result = subprocess.run([str(exe), str(game), '--play'], cwd=exe.parent, env=env,
                                stdout=log, stderr=log, timeout=600, **options)
    folders = list(output.glob('workshop/runs/*/research'))
    report = {'exit_code': result.returncode, 'hz_requested': args.hz, 'version': version,
              'present_requested': args.present,
              'pacing_requested': args.pace, 'phases_requested': args.phases or args.boundary == 'update',
              'input_boundary': args.boundary,
              'semantics_requested': args.semantics,
              'source_unchanged': fingerprint['save'] == tree(save) == tree(output / 'source-save/save_0'),
              'passed': False, 'deterministic_replay': 'not_established'}
    log_text = (output / 'runtime.log').read_text(encoding='utf-8', errors='replace')
    report['export_completed'] = '[research] saved ' in log_text and '[research] export failed:' not in log_text
    report['present_observed'] = re.findall(r'\[vk\] swapchain [^\n]*, (fifo|mailbox|immediate)', log_text)
    if len(folders) == 1 and (folders[0] / 'summary.ini').is_file():
        report['capture'] = ini(folders[0] / 'summary.ini')
        with (folders[0] / 'observations.csv').open(newline='') as f:
            observations = list(csv.DictReader(f))
        report['gameplay_polls'] = sum(o['playing'] == '1' for o in observations)
        report['passed'] = (result.returncode == 0 and report['source_unchanged'] and report['export_completed'] and
                            report['gameplay_polls'] > 0 and int(report['capture']['cameras']) > 0 and
                            report['capture']['truncated'] == '0' and (output / 'scene.png').is_file())
        if args.phases or args.boundary == 'update':
            report['passed'] = (report['passed'] and int(report['capture'].get('phase_samples', 0)) > 0 and
                                report['capture'].get('phases_truncated') == '0')
        if args.boundary == 'update':
            report['updates'] = ini(folders[0] / 'update-summary.ini')
            update = report['updates']
            with (folders[0] / 'updates.csv').open(newline='') as stream:
                update_rows = list(csv.DictReader(stream))
            report['scenario_movement'] = scenario_movement(update_rows)
            report['scenario_transition'] = scenario_transition(update_rows)
            report['passed'] = (report['passed'] and update['gate_ready'] == update['completed'] == '1' and
                                update['integrity_error'] == '0' and
                                int(update['captured_updates']) == int(update['source_consumed']) == args.updates and
                                (folders[0] / 'inputs.lastape').is_file())
            if args.semantics:
                report['passed'] = (report['passed'] and report['capture'].get('semantics') == '1' and
                                    report['capture'].get('external_truncated') == '0' and
                                    len(update_rows) == args.updates and
                                    all(r.get('rng_before_observed') == r.get('rng_observed') == '1' for r in update_rows) and
                                    (folders[0] / 'external.csv').is_file() and (folders[0] / 'actor-samples.csv').is_file() and
                                    (folders[0] / 'actors.csv').is_file())
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
