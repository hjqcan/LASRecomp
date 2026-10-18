#!/usr/bin/env python3
"""Every stairs / ladder transition of a seed's generated dungeons (tools/stairs_test.sh): one row per
AreaUpstairs (a side-view passage's ladder top) and AreaDownstairs (a room's stairs down) of the levels in
rando/<seed>/dungeon_map.tsv, with where Link must end up.

    python3 stairs_cases.py <game romfs> <seed dir>
    -> level  kind(up/down)  from room  trigger x y z  target locator  target area x0 z0 x1 z1  gate flag  mode
       (gate flag: the event flag the trigger waits for - stairs that appear once a room is cleared - or -1;
       mode: how Link goes, e.g. LadderUp / LadderDown / DefaultDownStairs / WaterSideIn)

The trigger's room is warped into by its own start (a side-view room's 'LadderDown' start is at its ladder top);
the target area is the room named after the target locator, as the seed's level file has it.
Runs in the LAS venv.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'third_party', 'LAS-Randomizer'))

import fixedhash  # noqa: E402
from RandomizerCore.Tools import leb  # noqa: E402


def main(romfs, seed):
    levels = []
    for line in open(os.path.join(seed, 'dungeon_map.tsv')):
        lv = line.split('\t')[0]
        if lv not in levels:
            levels.append(lv)
    for lv in levels:
        def path(*p):
            s = os.path.join(seed, 'romfs', 'region_common', 'level', lv, *p)
            return s if os.path.exists(s) else os.path.join(romfs, 'region_common', 'level', lv, *p)
        fh = fixedhash.FixedHash(open(path(lv + '.lvb'), 'rb').read())
        areas = {e.name.decode(): struct.unpack_from('<6f', e.data, 0) for e in fh.get(b'area').data.entries}
        starts = {e.name.decode() for e in fh.get(b'tagPlayerStart').data.entries}
        for room in sorted(areas):
            r = leb.Room(open(path(room + '.leb'), 'rb').read())
            for a in r.actors:
                n = a.name.decode() if isinstance(a.name, bytes) else a.name
                kind = 'up' if n.startswith('AreaUpstairs') else 'down' if n.startswith('AreaDownstairs') else None
                if not kind:
                    continue
                tgt = a.parameters[0].decode() if isinstance(a.parameters[0], bytes) else str(a.parameters[0])
                if tgt not in starts:
                    print(f'# {lv} {room}: {kind} target {tgt} has no start in the level', file=sys.stderr)
                    continue
                area = tgt if tgt in areas else tgt.rsplit('_', 1)[0]
                if area not in areas:
                    print(f'# {lv} {room}: {kind} target {tgt}: no room of that name', file=sys.stderr)
                    continue
                x0, _, z0, x1, _, z1 = areas[area]
                gate = a.switches[0][1] if a.switches[0][0] == 1 else -1  # (switch type 1: a global event flag)
                print('\t'.join([lv, kind, room, '%.2f' % a.posX, '%.2f' % a.posY, '%.2f' % a.posZ, tgt,
                                 '%g' % x0, '%g' % z0, '%g' % x1, '%g' % z1, str(gate),
                                 (a.parameters[1].decode() if isinstance(a.parameters[1], bytes) else '') or '-']))


if __name__ == '__main__':
    main(*sys.argv[1:3])
