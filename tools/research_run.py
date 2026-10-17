"""Run an isolated LAS HID/camera/timing experiment; never certifies deterministic replay."""
import argparse
import configparser
import csv
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import stat
import subprocess
from game_versions import game_version


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
    p.add_argument('--frames', type=int, default=5700)
    args = p.parse_args()
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
    with (game / 'exefs/main').open('rb') as f:
        build_id = f.read(0x60)[0x40:0x54].hex()
    version = game_version(build_id)
    settings = 'language = en-US\nfullscreen = 0\nmenu_tip_seen = 1\naspect_asked = 1\n'
    fingerprint = {'runtime': sha(exe), 'library': sha(library), 'build_id': build_id,
                   'save': tree(save), 'settings': hashlib.sha256(settings.encode()).hexdigest()}
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
               LARECOMP_WINDOW_SHOT=str(args.frames), LARECOMP_EXIT_AFTER_SHOT='1',
               LARECOMP_SHOT_PATH=str(output / 'scene.png'))
    if tape:
        env['LARECOMP_RESEARCH_REPLAY'] = str(tape)
    else:
        env['LARECOMP_RESEARCH_CAPTURE'] = '1'
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
              'source_unchanged': fingerprint['save'] == tree(save) == tree(output / 'source-save/save_0'),
              'passed': False, 'deterministic_replay': 'not_established'}
    log_text = (output / 'runtime.log').read_text(encoding='utf-8', errors='replace')
    report['present_observed'] = re.findall(r'\[vk\] swapchain [^\n]*, (fifo|mailbox|immediate)', log_text)
    if len(folders) == 1 and (folders[0] / 'summary.ini').is_file():
        report['capture'] = ini(folders[0] / 'summary.ini')
        with (folders[0] / 'observations.csv').open(newline='') as f:
            observations = list(csv.DictReader(f))
        report['gameplay_polls'] = sum(o['playing'] == '1' for o in observations)
        report['passed'] = (result.returncode == 0 and report['source_unchanged'] and
                            report['gameplay_polls'] > 0 and int(report['capture']['cameras']) > 0 and
                            report['capture']['truncated'] == '0' and (output / 'scene.png').is_file())
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2), flush=True)
    if not report['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
