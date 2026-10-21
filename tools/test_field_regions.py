#!/usr/bin/env python3
"""Tests for tools/rando/field_regions.py: the reviewed logic region of every Field screen / tile.

  python3 tools/test_field_regions.py                  the table itself (no game files, no packages needed)
  <rando venv python> tools/test_field_regions.py <romfs> [seed dir ...]
                                                       + every bush / dig check of the game resolves to a reviewed
                                                         region, its logic entry, hint area and tracker place agree;
                                                         a generated seed's logic.json / tracker.tsv / hint_steps.json
                                                         agree with the table (LARECOMP_ROMFS works as <romfs> too)
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RANDO = os.path.join(HERE, 'rando')
LAS = os.path.normpath(os.path.join(HERE, '..', 'third_party', 'LAS-Randomizer'))
sys.path.insert(0, RANDO)
import field_regions as fr  # noqa: E402

FAILS = []


def fail(msg):
    FAILS.append(msg)
    print('FAIL', msg)


def logic_names():
    """{top-level LAS logic name: type} (logic.yml parsed by hand: no yaml package needed)."""
    out, cur = {}, None
    text = open(os.path.join(LAS, 'RandomizerCore', 'Data', 'logic.yml')).read()
    for line in text.splitlines():
        m = re.match(r'^([A-Za-z0-9:_-]+):\s*$', line)
        if m:
            cur = m.group(1)
            out[cur] = None
            continue
        m = re.match(r'^\s+type:\s*(\S+)', line)
        if m and cur and out[cur] is None:
            out[cur] = m.group(1)
    for c in re.findall(r'condition-\w+:\s*(.+)', text):  # (settings LAS's own conditions use: rooster, ...)
        for tok in re.findall(r'[A-Za-z0-9_-]+', c):
            out.setdefault(tok, 'used')
    for line in open(os.path.join(LAS, 'RandomizerCore', 'Data', 'items.yml')):  # (items are logic names too)
        m = re.match(r'^  ([a-z0-9-]+):\s*$', line)
        if m:
            out.setdefault(m.group(1), 'item')
    return out


# the hint / spoiler areas the Hint Sheet knows (hint_sheet.AREA)
AREAS = {'mabe-village', 'toronbo-shores', 'mysterious-woods', 'koholint-prairie', 'tabahl-wasteland', 'ukuku-prairie',
         'sign-maze', 'goponga-swamp', 'taltal-heights', 'marthas-bay', 'kanalet-castle', 'pothole-field',
         'animal-village', 'yarna-desert', 'ancient-ruins', 'rapids-ride', 'taltal-mountains-east',
         'taltal-mountains-west'}
# tracker.PLACE id (the game's map labels) -> the hint areas inside that place
PLACE_AREAS = {0: {'kanalet-castle'}, 1: {'rapids-ride'}, 2: {'ancient-ruins'}, 3: {'animal-village'},
               4: {'yarna-desert'}, 6: {'marthas-bay'}, 7: {'pothole-field'}, 10: {'mabe-village'},
               11: {'mysterious-woods'}, 12: {'goponga-swamp'}, 13: {'koholint-prairie'}, 14: {'tabahl-wasteland'},
               15: {'taltal-mountains-east', 'taltal-mountains-west'}, 16: {'taltal-heights'}, 17: {'taltal-heights'},
               18: {'toronbo-shores'}, 19: {'ukuku-prairie'}, 20: {'koholint-prairie'}}
# game places without a map label of their own (LocationName SignpostMaze, SouthVillage, EastBay, ShrineFalls,
# FaceShrine): the tracker names them after the nearest labelled place, the hints after the place itself
UNLABELLED_ZONES = {150, 140, 141, 142, 200, 130, 131, 134, 135}


def test_table():
    names = logic_names()
    regions = {k for k, t in names.items() if t == 'region'}
    rooms = {f'Field_{r:02d}{chr(65 + c)}' for r in range(1, 17) for c in range(16)}
    if set(fr.SCREENS) != rooms:
        fail(f'SCREENS: missing {sorted(rooms - set(fr.SCREENS))}, extra {sorted(set(fr.SCREENS) - rooms)}')

    def check_rc(room, rc):
        r, c = (rc, None) if isinstance(rc, str) else rc
        if r not in regions:
            fail(f'{room}: {r!r} is not a LAS region')
        for tok in re.findall(r'[A-Za-z0-9_-]+(?::\d+)?', c or ''):
            if tok.split(':')[0] not in names:
                fail(f'{room}: condition {c!r}: unknown logic name {tok!r}')

    for room, e in fr.SCREENS.items():
        if e is None:
            continue
        parts = []
        if isinstance(e, tuple) and isinstance(e[1], list):
            e, parts = e
        check_rc(room, e)
        seen = set()
        for tiles, rc in parts:
            check_rc(room, rc)
            bad = [t for t in tiles if not 0 <= t < 80]
            if bad or seen & set(tiles):
                fail(f'{room}: part tiles out of range {bad} / listed twice {sorted(seen & set(tiles))}')
            seen |= set(tiles)
    for z, a in fr.ZONE_AREA.items():
        if a not in AREAS:
            fail(f'ZONE_AREA[{z}] = {a!r}: not a hint area')
    for r, a in fr.REGION_AREA.items():
        if r not in regions or a not in AREAS:
            fail(f'REGION_AREA[{r!r}] = {a!r}')
    # a screen without a reviewed entry fails loudly
    try:
        fr.region_of('Field_01A', 0)
        fail('region_of on a None screen did not raise')
    except fr.NoRegion:
        pass
    # the case that started this table: the Bomb Bag dig in Tal Tal Heights (PlayAll3) was 'river' / Martha's Bay
    if fr.region_of('Field_03I', 42) != ('wasteland', 'bracelet'):
        fail(f"Field_03I tile 42: {fr.region_of('Field_03I', 42)}")
    # ... and the lane's last pocket past the rock at 03I tile 75 (above the Heights, no stairs down)
    for room, tile in (('Field_03I', 77), ('Field_04I', 9), ('Field_04J', 11)):
        if fr.region_of(room, tile) != ('wasteland', 'bracelet'):
            fail(f'{room} tile {tile}: {fr.region_of(room, tile)}')
    print('table:', sum(1 for e in fr.SCREENS.values() if e is not None), 'screens with checks,',
          sum(1 for e in fr.SCREENS.values() if e is None), 'without')


def test_game(romfs, seed_dirs):
    sys.path.insert(0, LAS)
    import yaml
    from RandomizerCore.Tools import leb
    import bushes
    import digs
    import tracker
    logic = yaml.safe_load(open(os.path.join(LAS, 'RandomizerCore', 'Data', 'logic.yml')).read().split('---', 1)[1])
    items = {}
    bl = bushes.add_to_logic(bushes.scan(romfs, leb), logic, items)  # (raises on a check without a reviewed region)
    dl = digs.add_to_logic(digs.scan(romfs, leb), logic, items)
    zones = fr.tile_zones(romfs)
    area_at, _, _ = tracker.field_areas(romfs)
    n = 0
    for name, c in list(bl.items()) + list(dl.items()):
        room, tile = c['room'], c['tile']
        region, cond = fr.region_of(room, tile)
        v = logic[name]
        zone = zones[room][tile]
        if v['region'] != region:
            fail(f'{name}: logic region {v["region"]}, table {region}')
        if cond and cond not in (v.get('condition-basic') or ''):
            fail(f'{name}: condition {v.get("condition-basic")!r} lacks the pocket {cond!r}')
        if v['spoiler-region'] != fr.area_of(region, zone):
            fail(f'{name}: hint area {v["spoiler-region"]}, zone {zone}')
        row, col = bushes._screen_pos(room)
        place = area_at(col * 15.0 + tile % 10 * 1.5 + 0.75, (row - 1) * 12.0 + tile // 10 * 1.5 + 0.75)
        if zone not in UNLABELLED_ZONES and v['spoiler-region'] not in PLACE_AREAS.get(place, ()):
            fail(f'{name}: hint area {v["spoiler-region"]} but the tracker place is {tracker.PLACE[place]!r}')
        n += 1
    print('game:', n, 'bush / dig checks resolve; logic region, hint area and tracker place agree')
    for sd in seed_dirs:
        test_seed(sd, bl, dl, zones)


def test_seed(seed_dir, bl, dl, zones):
    """A generated seed's files agree with the table (logic.json, tracker.tsv's check rows, hint_steps.json)."""
    logic = json.load(open(os.path.join(seed_dir, 'logic.json')))
    rows = {}
    for line in open(os.path.join(seed_dir, 'tracker.tsv')):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'C':
            rows[f[1]] = f
    steps = json.load(open(os.path.join(seed_dir, 'hint_steps.json')))
    hint = {}
    for s in (steps if isinstance(steps, list) else steps.get('steps', [])):
        if isinstance(s, dict) and s.get('loc'):
            hint[s['loc']] = s.get('region')
    n = 0
    for name, c in list(bl.items()) + list(dl.items()):
        if name not in logic:
            continue
        region, _ = fr.region_of(c['room'], c['tile'])
        if logic[name]['region'] != region:
            fail(f'{seed_dir}: {name} logic.json region {logic[name]["region"]}, table {region}')
        if name in rows and rows[name][7] != region:
            fail(f'{seed_dir}: {name} tracker.tsv region {rows[name][7]}, table {region}')
        area = fr.area_of(region, zones[c['room']][c['tile']])
        if name in hint and hint[name] != area:
            fail(f'{seed_dir}: {name} hint region {hint[name]}, table {area}')
        n += 1
    print(f'seed {seed_dir}: {n} bush / dig checks agree ({sum(1 for k in hint if k in logic)} hinted)')


if __name__ == '__main__':
    test_table()
    romfs = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('LARECOMP_ROMFS')
    if romfs:
        test_game(os.path.abspath(romfs), sys.argv[2:])
    else:
        print('(no romfs given: game checks skipped)')
    if FAILS:
        print(len(FAILS), 'failures')
        sys.exit(1)
    print('ok')
