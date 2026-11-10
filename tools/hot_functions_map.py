"""Map tools/hot_functions.txt (made on the game's 1.0.0) onto another game version's build of the main program.

Usage (tools/venv/bin/python):
  hot_functions_map.py GAME_1_0_0 GAME_OTHER [--to v1_0_1]

GAME_1_0_0 / GAME_OTHER: folders with the player's own extracted game files (exefs/main, exefs/sdk). Writes
tools/hot_functions_<to>.txt: the same functions at the other build's addresses, mapped as tools/version_map.py maps the
address tables (a function identical in both builds moves with it; one in a changed function is left out, so it builds
with optnone there). Nothing from the game files is written: only addresses. The Android setup picks the list by the
game version (runtime/gamebuild.c).
"""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from version_map import MAIN_BASE, SDK_BASE, Mapper  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('old_game', type=Path)
    ap.add_argument('new_game', type=Path)
    ap.add_argument('--to', default='v1_0_1')
    args = ap.parse_args()
    here = Path(__file__).resolve().parent
    src = here / 'hot_functions.txt'
    m = Mapper(args.old_game, args.new_game)
    out, dropped = [], []
    for line in src.read_text().splitlines():
        if line.startswith('#') or not line.strip():
            continue
        mod, off = line.split()
        base = MAIN_BASE if mod == 'main' else SDK_BASE
        new, how = m.map(base + int(off, 16))
        if new is None:
            dropped.append(f'{line}: {how}')
            continue
        out.append(f'{mod} {new - base:x}')
    head = [l.replace(', unchanged below this header', ', mapped below') for l in src.read_text().splitlines() if l.startswith('#')]
    text = '\n'.join(head + [f'# Mapped onto {args.to} by tools/hot_functions_map.py ({len(out)} functions, {len(dropped)} left out: in '
                              'a changed function).'] + out) + '\n'
    dst = here / f'hot_functions_{args.to}.txt'
    dst.write_text(text)
    print(f'{dst.name}: {len(out)} functions, {len(dropped)} left out')
    for d in dropped:
        print('  left out:', d)


if __name__ == '__main__':
    main()
