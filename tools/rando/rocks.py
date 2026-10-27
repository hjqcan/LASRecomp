#!/usr/bin/env python3
"""Rocksanity (LAS Recomp addition, setting 'rocks'): every overworld rock Link lifts with the bracelet (ObjRock 0xb8,
Field) hides an item, like the bushes (bushes.py; pots.py does the writing). Each rock gets a "lifted" flag as its
switch 0: the game sets it when the rock is lifted, the item then appears where the rock stood (runtime/hle/pot_glow.c,
as for bushes), and the rock comes back gold once Link has left, until its item is taken.

Left out (vanilla rocks): the 5 rocks over stairs / Manbo warp holes (their switch 0 is a real flag), the rock beside
the Golden Leaf in Field_06I (an LAS check already), the 03E rock LAS's Classic D2 turns into a swamp flower and the
10E rock LAS's open-mabe removes (with those settings), and rocks on a pit tile (none today). The 6 rocks hiding a
Secret Seashell are their seashell spot's check (seashell_spots.absorb, as for bushes).

Cave rocks (ObjCaveRock 0xf6) aren't checks: they can't be lifted, they are push blocks (checked in game 2026-10-25;
parameters 0-3 say which ways one moves, all 0: never), pushing sets no flag, and an item hidden under one blocks Link
instead of being picked up once the rock is pushed off it.

Logic: LAS's break-rock (bracelet) AND the reviewed region of the rock's tile (field_regions.py), or SCREENS / BORDER
below: rocks often sit where two regions meet; with the bracelet either side reaches them, the side that needs more is
taken.
"""
import glob
import os

import bushes
import field_regions
import pots
import seashell_spots

ROCK_TYPE = 0xb8   # ObjRock
FLAG_BASE = 2048   # after the bushes' flags (pots.apply takes the next free index)
FLAG_LIMIT = 3800  # (Chaos map letters from 3800: dungeon_gen.py)
KEY_TAG = 0x7C     # the item actors' key tag (runtime/hle/pot_glow.c ROCK_KEY_TAG)
HIDE_SCALE = 0.4   # (the item only spawns once the rock is lifted: this scale is never seen)
GOLDEN_LEAF_ROCK = 0x8D0001005CAD67C4  # Field_06I, beside ItemGoldenLeaf (LAS location)
CLASSIC_D2_ROCK = 0x6A0012005D0C5FB9   # Field_03E: a swamp flower with LAS's classic-d2
JUNK_MIX = bushes.JUNK_MIX  # filler: what bushes give (hearts and rupees, a few refills)

# Field screens the region table has no entry for (no bush / dig there): the LAS seashell rock's region
SCREENS = {'Field_01M': ('taltal-east-1', 'feather | rooster'),   # taltal-east-bridge
           'Field_02F': ('taltal-west-0', None)}                  # taltal-west-rock
# rocks on a region border (location name -> region, condition): the side that needs more
BORDER = {
    'rock-02O-1': ('taltal-east-2', '(feather & hookshot) | rooster'),  # only the ledge side touches them
    'rock-02O-2': ('taltal-east-2', '(feather & hookshot) | rooster'),
    'rock-03E-2': ('wasteland', 'bracelet'),  # lifted from the wasteland ledge
    'rock-03E-3': ('wasteland', None), 'rock-03E-4': ('wasteland', None), 'rock-04E-2': ('wasteland', None),
    'rock-07E-1': ('ghost-grave', None),
    'rock-07F-1': ('wasteland', None),  # both open sides are wasteland
    'rock-10E-1': ('plains', None), 'rock-10E-2': ('plains', None),
    'rock-15F-1': ('ghost-house', None), 'rock-15F-2': ('ghost-house', None), 'rock-15F-3': ('ghost-house', None),
}


def scan(romfs, leb, classic_d2=False, open_mabe=False):
    """Every rock that becomes a check: dicts with level, room, key, n (index among the screen's rocks, counting every
    rock), tile, zone, shell (its Secret Seashell index or None)."""
    zones = field_regions.tile_zones(romfs)
    out = []
    for f in sorted(glob.glob(os.path.join(romfs, 'region_common', 'level', 'Field', 'Field_*.leb'))):
        room_name = os.path.basename(f)[:-4]
        with open(f, 'rb') as fh:
            room = leb.Room(fh.read())
        holes, (ox, oz) = bushes._pit_tiles(room)
        n = 0
        for a in room.actors:
            if a.type != ROCK_TYPE:
                continue
            pit = (int((a.posX - ox) // 1.5), int((a.posZ - oz) // 1.5)) in holes
            gone = (classic_d2 and a.key == CLASSIC_D2_ROCK) or (open_mabe and a.key in bushes.OPEN_MABE_REMOVED)
            if not (a.switches[0][0] == 1 or a.key == GOLDEN_LEAF_ROCK or gone or pit):
                tile = bushes._tile(room_name, a)
                out.append({'level': 'Field', 'room': room_name, 'key': a.key, 'n': n, 'tile': tile,
                            'zone': zones[room_name][tile] if room_name in zones else None,
                            'shell': seashell_spots.shell_index(a)})
            n += 1
    return out


def location_name(r):
    return f"rock-{r['room'].split('_')[-1]}-{r['n'] + 1}"


def _entry(name, r):
    fixed = BORDER.get(name) or SCREENS.get(r['room'])
    if not fixed:  # (raises field_regions.NoRegion for a screen / tile without a reviewed region)
        return field_regions.logic_entry(r['room'], r['tile'], r.get('zone'), 'break-rock')
    region, cond = fixed
    return {'type': 'item', 'subtype': 'standing', 'content': 'rupee-5', 'region': region,
            'spoiler-region': field_regions.area_of(region, r.get('zone')),
            'condition-basic': pots._and(cond, 'break-rock')}


def add_to_logic(rocks, logic_defs, item_defs, traps='none', rng=None):
    """A location per rock in LAS's logic + filler in the pool (bushes' mix). Returns {location: rock}. A rock hiding a
    Secret Seashell is that spot's check (seashell_spots.absorb: its seashell fills this check, no filler for it)."""
    locs, absorbed = {}, 0
    for r in rocks:
        name = location_name(r)
        loc = _entry(name, r)
        if r.get('shell') is not None and seashell_spots.absorb(logic_defs, r['shell'], loc):
            absorbed += 1
        logic_defs[name] = loc
        locs[name] = r
    for k, d in pots.FILLER_DEFS.items():
        item_defs.setdefault(k, dict(d, type='junk', quantity=0, **{'chest-type': 'junk'}))
    n = len(locs) - absorbed
    trap_kinds = [k for k in item_defs if k.endswith('-trap') and not k.startswith('zol')]
    n_traps = int(round(n * pots.POT_TRAPS.get(traps, 0.0))) if trap_kinds else 0
    if n_traps:
        import random
        rng = rng or random.Random(n)
        for _ in range(n_traps):
            item_defs[rng.choice(trap_kinds)]['quantity'] += 1
    left = n - n_traps
    m = left
    for i, (item, frac) in enumerate(JUNK_MIX):
        k = left if i == len(JUNK_MIX) - 1 else int(round(m * frac))
        item_defs.setdefault(item, {'quantity': 0})
        item_defs[item]['quantity'] += k
        left -= k
    return locs


def apply(locs, mods, out_dir, romfs, leb, event_tools, oead_tools, item_get, data):
    lines = pots.apply(locs, mods, out_dir, romfs, leb, event_tools, oead_tools, item_get, data, flag_prefix='LARRock',
                       flag_base=FLAG_BASE, flag_limit=FLAG_LIMIT, key_tag=KEY_TAG, hide_scale=HIDE_SCALE,
                       cut_flag=True, drop_param=0)
    with open(os.path.join(out_dir, 'flags_ext'), 'a') as f:
        f.write('rocksanity: event flags 2048+ (runtime/hle/flags_ext.c)\n')
    return lines


def write_tsv(seed_dir, leb):
    return pots.write_tsv(seed_dir, leb, flag_prefix='LARRock', name='rocks.tsv', hide_scale=HIDE_SCALE)
