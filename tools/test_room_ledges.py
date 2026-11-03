#!/usr/bin/env python3
"""Tests for the dungeon room shuffle's drop-down-only doorways (tools/rando/dungeon_gen.py INWARD_REVIEWED /
inward_only, room_logic.level_graph): raised doorways on a ledge Link can't climb from his room are one-way in logic.

  python3 tools/test_room_ledges.py            the reviewed list against the room catalog (no game files needed)
  <rando venv python> tools/test_room_ledges.py <romfs>
                                               + the tile grids give exactly the reviewed list, and the logic graph
                                                 of the reported layout (seed LuckyMoblin988: Turtle Rock 07E in Tail
                                                 Cave 07C, its north ledge jumping to 06F) only goes one way
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RANDO = os.path.join(HERE, 'rando')
FAILS = []


def fail(msg):
    FAILS.append(msg)
    print('FAIL', msg)


def reviewed():
    """INWARD_REVIEWED read from dungeon_gen.py's source (no packages needed)."""
    src = open(os.path.join(RANDO, 'dungeon_gen.py')).read()
    block = src[src.index('INWARD_REVIEWED = {'):]
    block = block[:block.index('}') + 1]
    return {(a, b, int(c)): d for a, b, c, d in re.findall(r"\('(\w+)', '([NSWE])', (\d+)\): '(\w+)'", block)}


def table_checks(rev):
    cat = json.load(open(os.path.join(RANDO, 'room_catalog.json')))
    if len(rev) < 10:
        fail(f'INWARD_REVIEWED: only {len(rev)} entries parsed')
    if ('Lv08TurtleRock_07E', 'N', 16) not in rev:
        fail("the reported ledge (Turtle Rock 07E north) isn't in INWARD_REVIEWED")
    for (room, side, slot), kind in sorted(rev.items()):
        v = cat.get(room)
        if v is None:
            fail(f'{room}: not in the room catalog')
            continue
        if slot not in v['openings'][side]:
            fail(f'{room} {side}{slot}: no such doorway in the catalog ({v["openings"][side]})')
            continue
        h = v['heights'][side][v['openings'][side].index(slot)]
        if h is None or h < 1.0:
            fail(f'{room} {side}{slot}: floor height {h} - not a raised doorway')
        if kind not in ('ledge', 'arrival'):
            fail(f'{room} {side}{slot}: kind {kind}')


def grid_checks(romfs, rev):
    sys.path.insert(0, RANDO)
    import dungeon_gen
    import room_logic
    found = dungeon_gen.inward_only(romfs, reviewed=False)
    for k in sorted(set(found) | set(rev)):
        if found.get(k) != rev.get(k):
            fail(f'{k}: tile grids say {found.get(k)}, reviewed list {rev.get(k)} - re-read the grid, update both')
    cat = json.load(open(os.path.join(RANDO, 'room_catalog.json')))
    level = 'Lv01TailCave'
    layout = {'07C': 'Lv08TurtleRock_07E', '06F': 'Lv01TailCave_05F'}
    rows = [(level, '07C', 'N', 16, 10, '06F', 'S', 2.25, 0.0), (level, '06F', 'S', 10, 16, '07C', 'N', 3.75, 0.0, 0.75)]
    edges, _ = room_logic.level_graph(cat, romfs, level, layout, rows, dungeon_gen.openings)
    out = {}  # (cell -> cells: the room graph's nodes are areas of cells)
    for a, ts in edges.items():
        out.setdefault(room_logic.cell_of(a), set()).update(
            room_logic.cell_of(t) for t, _ in ts if room_logic.cell_of(t) != room_logic.cell_of(a))
    if '06F' in out['07C']:
        fail('Tail Cave 07C (Turtle Rock 07E) -> 06F: the logic still leaves through the north ledge')
    if '07C' not in out['06F']:
        fail('Tail Cave 06F -> 07C: the way in (dropping off the ledge) is missing')
    if room_logic.inward_exits(cat, romfs, level, layout, rows):
        fail('inward_exits reports edges out through the ledge')
    # Turtle Rock 06E's south strip is left from the rooms its stairs come from, not from 06E
    tr = 'Lv08TurtleRock'
    lay = {f'{c}': f'{tr}_{c}' for c in ('05D', '05E', '05F', '06D', '06E', '06F', '07F')}
    lay['07E'] = f'{tr}_07E'
    edges, _ = room_logic.level_graph(cat, romfs, tr, lay, [], dungeon_gen.openings)
    if any(room_logic.cell_of(t) == '07E' for a, ts in edges.items() if room_logic.cell_of(a) == '06E' for t, _ in ts):
        fail('Turtle Rock 06E -> 07E: left from 06E itself (its south strip is only reached by the stairs)')


def main():
    rev = reviewed()
    table_checks(rev)
    romfs = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('LARECOMP_ROMFS')
    if romfs:
        grid_checks(os.path.abspath(romfs), rev)
    print(f'{len(rev)} drop-down-only doorways' + (' (tile grids checked)' if romfs else ''),
          'FAILED' if FAILS else 'ok')
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
