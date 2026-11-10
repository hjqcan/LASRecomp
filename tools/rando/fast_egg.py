#!/usr/bin/env python3
"""Fast Egg: the first drop in the Wind Fish's Egg lands Link at the boss, with no maze.

The Egg (Lv09WindFishsEgg) has two holes. The first, in 01A just past the entrance, drops into the maze
(07C_Fall); the one at the maze's end, in 05C, drops to the boss room (01D). Fast Egg points the first hole where
the second one goes, with the second one's parameters.

    apply(out_dir, romfs, leb) -> True when the room was changed
"""
import os

LEVEL = 'Lv09WindFishsEgg'
FIRST, LAST = 'Lv09WindFishsEgg_01A', 'Lv09WindFishsEgg_05C'


def _room(out_dir, romfs, leb, name):
    for base in (os.path.join(out_dir, 'romfs'), romfs):
        p = os.path.join(base, 'region_common', 'level', LEVEL, name + '.leb')
        if os.path.exists(p):
            with open(p, 'rb') as f:
                return leb.Room(f.read())
    return None


def _hole(room):
    holes = [a for a in room.actors if a.name.startswith(b'AreaHoleLowe')]
    return holes[0] if len(holes) == 1 else None


def apply(out_dir, romfs, leb):
    first, last = _room(out_dir, romfs, leb, FIRST), _room(out_dir, romfs, leb, LAST)
    a, b = first and _hole(first), last and _hole(last)
    if not a or not b:
        return False
    a.parameters[1] = b.parameters[1]  # (the boss room's spawn point, 'Lv09WindFishsEgg_01D')
    a.parameters[3] = b.parameters[3]
    p = os.path.join(out_dir, 'romfs', 'region_common', 'level', LEVEL, FIRST + '.leb')
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(first.repack())
    return True
