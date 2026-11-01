#!/usr/bin/env python3
"""Room crossing in the dungeon room logic (tools/rando/room_logic.py): the areas of a room Link walks between, the
stairs passages, pot doors. Seed RapidBowWow958 (Face Shrine room shuffle) was unbeatable: the logic walked across
Turtle Rock 07D's lava, up Face Shrine 04D's ledges, through 04D's pot door with bombs and down every staircase into
every passage.

usage: test_room_crossing.py <romfs>   (or LARECOMP_ROMFS)"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RANDO = os.path.join(HERE, 'rando')
sys.path.insert(0, RANDO)
sys.path.insert(0, os.path.join(HERE, '..', 'third_party', 'LAS-Randomizer'))
import room_logic as rl  # noqa: E402
import dungeon_gen  # noqa: E402

FAILS = []


def fail(msg):
    FAILS.append(msg)
    print('FAIL', msg)


def moves_between(romfs, room, a_tile, b_tile):
    """Alternatives for one move from the area at a_tile to the area at b_tile (None: no direct move)."""
    area, _, moves = rl.room_areas(romfs, room)
    a, b = area.get(a_tile), area.get(b_tile)
    if a is None or b is None:
        return 'no area'
    if a == b:
        return 'same'
    return next((alts for x, y, alts in moves if (x, y) == (a, b)), None)


def reach(edges, start, have):
    """Nodes reachable from start with the atoms in `have`."""
    seen, todo = {start}, [start]
    while todo:
        a = todo.pop()
        for b, r in edges[a]:
            if b not in seen and r <= have:
                seen.add(b)
                todo.append(b)
    return seen


def main():
    romfs = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.environ['LARECOMP_ROMFS'])
    cat = json.load(open(os.path.join(RANDO, 'room_catalog.json')))
    feather = rl.FEATHER
    if rl.JUMP != 'feather | (rooster & bad-pets)':
        fail(f'rs-jump: {rl.JUMP}')

    # Turtle Rock 07D: a lava strip (row 3) between the north doorway's floor and the rest - the feather or the
    # Rooster jumps it, and only that
    tr = 'Lv08TurtleRock_07D'
    got = moves_between(romfs, tr, (6, 4), (2, 4))
    if not got or set(got) != set(feather):
        fail(f'{tr}: south floor -> north doorway floor: {got}, wanted the feather / Rooster jump')
    # Face Shrine 04D: from the south doorway, the raised strips are ledges above - never climbed
    fs = 'Lv06FaceShrine_04D'
    for t in ((0, 1), (6, 2)):
        if moves_between(romfs, fs, (3, 6), t) is not None:
            fail(f'{fs}: climbs from the floor onto the raised strip at {t}')
    if moves_between(romfs, fs, (1, 1), (3, 1)) != [frozenset()]:
        fail(f'{fs}: the drop off the north-west strip is missing')
    # Tail Cave 05E: the raised platform's edge is a wall - over the pit with the feather / Rooster, not a walk off
    tc = 'Lv01TailCave_05E'
    if set(moves_between(romfs, tc, (2, 2), (2, 6)) or ()) != set(feather):
        fail(f'{tc}: platform -> east floor: {moves_between(romfs, tc, (2, 2), (2, 6))}, wanted the feather / Rooster')
    # pot doors: something thrown at them - an elephant needs the level 2 bracelet; bombs never do it
    if rl.throw_req(romfs, fs) != frozenset({'bracelet:2'}):
        fail(f'{fs}: pot door from this room: {rl.throw_req(romfs, fs)}, wanted bracelet:2')
    if rl.throw_req(romfs, 'Lv06FaceShrine_04A') != frozenset({'bracelet'}):
        fail('Face Shrine 04A: a pot to throw - the bracelet')
    if rl.door_terms(['potdoor'], 6, throw=None) is not None:
        fail('a pot door with nothing to throw is passable')

    # the hookshot: onto objects only (chests, pots, blocks, statues, hook posts), never a plain wall - unless the
    # seed has "hookshot anything"
    cm = 'Lv05CatfishsMaw_02E'
    if moves_between(romfs, cm, (1, 7), (6, 3)) != [frozenset({'hookshot'})]:
        fail(f'{cm}: the stairs ledge -> the chest floor by the hook post: {moves_between(romfs, cm, (1, 7), (6, 3))}')
    bg = 'Lv02BottleGrotto_03C'
    area, n, moves = rl.room_areas(romfs, bg)
    if any(frozenset({'hookshot'}) in alts for _, _, alts in moves):
        fail(f'{bg}: a hookshot crossing onto a plain wall')
    rl.HOOK_ANYTHING = True
    rl._areas_cache.clear()
    area, n, moves = rl.room_areas(romfs, bg)
    if not any(frozenset({'hookshot'}) in alts for _, _, alts in moves):
        fail(f'{bg}: hookshot anything: no crossing onto the wall')
    rl.HOOK_ANYTHING = False
    rl._areas_cache.clear()

    # side-view passages: by where their rooms sit (Face Shrine: three passages, not one), each needing the feather
    # (the Rooster doesn't come into side-view rooms)
    links = set(rl.stairs_links(romfs, 'Lv06FaceShrine', cat))
    want = {('Lv06FaceShrine_04F', 'Lv06FaceShrine_05D'), ('Lv06FaceShrine_05C', 'Lv06FaceShrine_08B'),
            ('Lv06FaceShrine_04B', 'Lv06FaceShrine_04H'), ('Lv06FaceShrine_04F', 'Lv06FaceShrine_06F')}
    if links != want:
        fail(f'Face Shrine stairs: {sorted(links)}')
    for a, _, b, _, alts in rl.stairs_ports(romfs, 'Lv06FaceShrine', cat):
        if {a, b} != {'Lv06FaceShrine_04F', 'Lv06FaceShrine_06F'} and set(alts) != {frozenset({'feather'})}:
            fail(f'Face Shrine passage {a} -> {b}: {alts}')

    # the vanilla Face Shrine graph: from the entrance with what the RapidBowWow958 player had (no feather, no
    # Rooster, bracelet level 1), 04B and 03B are out of reach; the feather opens the passage 05D -> 04F
    lv = 'Lv06FaceShrine'
    layout = {k.rsplit('_', 1)[1]: k for k in cat if k.startswith(lv + '_')}
    edges, _ = rl.level_graph(cat, romfs, lv, layout, [], dungeon_gen.openings)
    start = rl.entrance_node(cat, romfs, lv, layout)
    kit = {'sword', 'bombs', 'shoot-arrows', 'boomerang', 'bracelet', 'boots', 'break-pot', 'rs-shutter',
           'rs-switch-shutter', 'kill-pols-voice', 'kill-keese', 'kill-wizzrobe', 'kill-zol-red'}
    keys = {a for outs in edges.values() for _, r in outs for a in r if a.startswith('LOCK|')}
    cells = {rl.cell_of(x) for x in reach(edges, start, frozenset(kit | keys))}
    if '04F' in cells and not {'05F', '06F'} & cells:
        fail('Face Shrine: 04F reached from 05D without the feather')
    with_feather = {rl.cell_of(x) for x in reach(edges, start, frozenset(kit | keys | {'rs-jump', 'feather'}))}
    if not cells < with_feather:
        fail('Face Shrine: the feather reaches nothing more')

    print('room crossing', 'FAILED' if FAILS else 'ok')
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
