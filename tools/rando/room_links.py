"""Room links: actors in a room refer to each other by their index in it (relationship section 1 entries
[[p1, p2], index] and section 3 index lists - a kill-all trigger to its enemies, an NPC to its partner, a mini-boss to
its kit: Rolling Bones' spike bar, Smasher's iron ball). An edit that removes or reorders actors without renumbering
them leaves links past the room's actor list or at the wrong actor, and the game crashes building that actor when its
screen loads (Open Mabe's and the seashell spots' removals did, 2026-10-18). Checked over every room file the seed
writes (las_headless.py, before it finishes; seed_sweep.py's --full runs), so such a seed fails generation instead of
crashing in game:
  - every link inside the room's actor list
  - a mini-boss kit actor linked back from its mini-boss
  - with the game's own files (romfs): an actor the game has (same key) links only to actors it links to there, or to
    actors the seed added - a link moved onto another of the game's actors is a renumbering gone wrong

    check(out_dir, leb, romfs=None) -> list of problems ('<room>: ...'), empty when every link is sound
"""
import os

KIT_KEY_TAG = 0x6B  # (RandomizerCore/Randomizers/enemies.py: keys of the kit actors copied next to a mini-boss)


def _links(a):
    return [s[1] for s in a.relationships.section_1] + list(a.relationships.section_3)


def check_room(name, room, vanilla=None):
    """vanilla: {actor key: set of the keys it links to} over the game's own rooms (or None)."""
    out = []
    n = len(room.actors)
    for i, a in enumerate(room.actors):
        bad = [j for j in _links(a) if not 0 <= j < n]
        if bad:
            out.append(f'{name}: actor {i} ({a.name.decode(errors="replace")}) links to {bad}, the room has {n} actors')
            continue
        if a.key >> 56 == KIT_KEY_TAG:  # (a kit actor: back to the mini-boss that links to it)
            for j in a.relationships.section_3:
                if not any(s[1] == i for s in room.actors[j].relationships.section_1):
                    out.append(f'{name}: kit actor {i} ({a.name.decode(errors="replace")}) points back to actor {j}, '
                               'which does not link to it')
        if vanilla is not None and a.key in vanilla:
            for j in _links(a):
                t = room.actors[j].key
                if t in vanilla and t not in vanilla[a.key]:
                    out.append(f'{name}: actor {i} ({a.name.decode(errors="replace")}) links to actor {j} '
                               f'({room.actors[j].name.decode(errors="replace")}), which it does not link to in the game')
    return out


def _rooms(base, leb, levels=None):
    for lv in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        d = os.path.join(base, lv)
        if not os.path.isdir(d) or (levels is not None and lv not in levels):
            continue
        for f in sorted(os.listdir(d)):
            if f.endswith('.leb'):
                with open(os.path.join(d, f), 'rb') as fh:
                    yield f[:-4], leb.Room(fh.read())


def check(out_dir, leb, romfs=None):
    levels = os.path.join(out_dir, 'romfs', 'region_common', 'level')
    rooms = list(_rooms(levels, leb))
    vanilla = None
    if romfs:  # (the game's links of every actor in the levels the seed writes: rooms may move between files)
        vanilla = {}
        names = {lv for lv in os.listdir(levels)} if os.path.isdir(levels) else set()
        for _, r in _rooms(os.path.join(romfs, 'region_common', 'level'), leb, names):
            for a in r.actors:
                vanilla.setdefault(a.key, set()).update(r.actors[j].key for j in _links(a) if 0 <= j < len(r.actors))
    problems = []
    for name, room in rooms:
        problems += check_room(name, room, vanilla)
    return problems
