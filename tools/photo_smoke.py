"""Run an isolated photo-mode smoke test against the player's compiled game.

Inspect the saved PNGs as well as the reported result. This checks an opening-room
scene, not all rendering paths or a complete game simulation pause.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New directory for isolated settings, saves and images')
    parser.add_argument('--save-copy', type=Path, help='Optional version-compatible save mount; copied, never modified')
    args = parser.parse_args()
    exe, game, output = args.exe.resolve(), args.game.resolve(), args.output.resolve()
    if not exe.is_file() or not (game/'exefs/main').is_file():
        parser.error('Expected a built runtime and an extracted game directory')
    if output.exists():
        parser.error('--output must be a new directory')
    if args.save_copy and not args.save_copy.is_dir():
        parser.error('--save-copy must be a save mount directory')
    output.mkdir(parents=True)
    (output/'build').mkdir()
    if args.save_copy:
        shutil.copytree(args.save_copy, output/'save/save_0')
    settings = output/'settings.ini'
    settings.write_text('language = en-US\n', encoding='utf-8')
    env = {k: v for k, v in os.environ.items() if not k.startswith('LARECOMP_')}
    env.update(LARECOMP_DATA_ROOT=str(output), LARECOMP_SETTINGS=str(settings),
               LARECOMP_SAVE_DIR=str(output/'save'), LARECOMP_NO_UPDATE_CHECK='1',
               LARECOMP_KBD_AUTO='Link', LARECOMP_PHOTO_AT='2300',
               LARECOMP_PHOTO_SHOT_AT='2350,2400', LARECOMP_PHOTO_EXIT_AT='2600',
               LARECOMP_WINDOW_SHOT='2700', LARECOMP_SHOT_PATH=str(output/'resumed.png'),
               LARECOMP_EXIT_AFTER_SHOT='1', NO_AUDIO='1', LARECOMP_NO_MOVIE_AUDIO='1',
               LARECOMP_INPUT='200:A:8,600:L:12,600:R:12,700:A:8,900:L:12,900:R:12,1000:A:8,'
                              '1300:A:8,1450:DOWN:8,1600:A:8,1900:A:8,2200:A:8')
    options = {}
    if os.name == 'nt':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = subprocess.SW_HIDE
        options['startupinfo'] = startup
    log = output/'runtime.log'
    with log.open('wb') as stream:
        run = subprocess.run([str(exe), str(game), '--play'], env=env, cwd=exe.parent,
                             stdout=stream, stderr=stream, timeout=240, **options)
    text = log.read_text(encoding='utf-8', errors='replace')
    photos = sorted((output/'photos').glob('*.png'))
    hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in photos]
    passed = (run.returncode == 0 and len(hashes) == 2 and len(set(hashes)) == 1 and
              '[photo] frozen at frame' in text and '[photo] resumed at frame' in text and
              'level MarinTarinHouse' in text and (output/'resumed.png').is_file())
    report = {'passed': passed, 'exit_code': run.returncode,
              'photos': [p.name for p in photos], 'sha256': hashes,
              'note': 'Inspect photos and resumed.png; opening-room smoke test only.'}
    (output/'result.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
    if not passed:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
