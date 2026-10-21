#!/usr/bin/env python3
"""Check-tracker data for a seed: rando/<seed>/tracker.json, read by runtime/ui/tracker_overlay.mm.

  nodes   LAS logic as the seed uses it (logic.json from generation, else base LAS logic + pots): name, type,
          region, condition (basic | advanced... per the seed's logic level), [pit]/[heavy] enemy variants
  checks  every item location: label, overworld map position (cell units: x = column 0..16, y = row 0..16),
          group (dungeon icon, -1 = its overworld screen), flag (GlobalFlags index that is set once collected,
          -1 = unknown), kind (the tracker's filter: chest / heart / shell / boss / shop / bush / dig / pot / other),
          area (LAS region), dungeon (D0..D8 it physically sits in now, '-') and its cell there ('05B', '-')
  groups  the dungeon icons: at the overworld door that leads into the dungeon (shuffle-dungeons follows LAS's
          dungeon-entrances), labelled by the dungeon reached there
  rooms   (dungeon view) each shuffled dungeon cell's room (dungeon_rooms.tsv from tools/rando/dungeon_gen.py) and
          the map page shift of its floor (dungeon_map.tsv); door jumps / one-way doorways / room-shuffle letters are
          door_jumps.tsv, which the runtime already reads (runtime/hle/doorjump.c)
  settings  seed settings as logic tokens: S = on, F = off (a condition naming an off setting can't be met)
Dungeon room shuffle: checks the layout leaves out (settings absent-locations) are not listed; a moved room's checks
count for the dungeon it sits in now (settings dungeon-of, room_logic.py). Conditions are the seed's final logic.
Each node also carries its condition with every logic level up to glitched ("trick logic": shown, never required).
The runtime evaluates reachability from the live save's inventory (never from placements: no spoilers).
"""
import glob
import json
import os
import re
import sys
import types

import underwater  # (the sunken seashells' flags)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
LAS = os.path.join(ROOT, 'third_party', 'LAS-Randomizer')
DUNGEONS = {'Lv01TailCave': 'D1', 'Lv02BottleGrotto': 'D2', 'Lv03KeyCavern': 'D3', 'Lv04AnglersTunnel': 'D4',
            'Lv05CatfishsMaw': 'D5', 'Lv06FaceShrine': 'D6', 'Lv07EagleTower': 'D7', 'Lv08TurtleRock': 'D8',
            'Lv10ClothesDungeon': 'D0'}
LEVELS = {'basic': ['basic'], 'advanced': ['basic', 'advanced'], 'glitched': ['basic', 'advanced', 'glitched'],
          'hell': ['basic', 'advanced', 'glitched', 'hell'], 'none': ['basic']}


def _las():
    sys.modules.setdefault('PySide6', types.ModuleType('PySide6'))
    sys.modules.setdefault('PySide6.QtCore', types.ModuleType('PySide6.QtCore'))
    if LAS not in sys.path:
        sys.path.insert(0, LAS)
    if HERE not in sys.path:
        sys.path.insert(0, HERE)


def cell_of_field(room):
    m = re.match(r'Field_(\d\d)([A-P])', room or '')
    return (ord(m.group(2)) - 65 + 0.5, int(m.group(1)) - 1 + 0.5) if m else None


TRICK_LEVELS = ['basic', 'advanced', 'glitched']
# the overworld door of each dungeon entrance (LAS data.DUNGEON_ENTRANCES): key -> (level, Field room)
ENTRANCE_KEYS = {'tail-cave': 'Lv01TailCave', 'bottle-grotto': 'Lv02BottleGrotto', 'key-cavern': 'Lv03KeyCavern',
                 'angler-tunnel': 'Lv04AnglersTunnel', 'catfish-maw': 'Lv05CatfishsMaw', 'face-shrine': 'Lv06FaceShrine',
                 'eagle-tower': 'Lv07EagleTower', 'turtle-rock': 'Lv08TurtleRock', 'color-dungeon': 'Lv10ClothesDungeon'}


def kind_of(name, v, shell_spots=()):
    """The tracker's filter kind of a check (one each). Shellsanity spots (shell_spots.tsv) count as seashells."""
    if name.startswith('bush-'):
        return 'bush'
    if re.match(r'dig-\d\d[A-P]-\d\d$', name):
        return 'dig'
    if name.startswith('pot-') or re.match(r'D\d-pot-', name):
        return 'pot'
    content, sub = v.get('content'), v.get('subtype')
    if content == 'seashell' or name in shell_spots or name.endswith('-seashell-reward'):
        return 'shell'
    if content == 'heart-piece':
        return 'heart'
    if sub == 'shop' or name.startswith('shop-') or name.startswith('trendy-prize'):
        return 'shop'
    cond = ' '.join(str(x) for k, x in v.items() if k.startswith('condition'))
    if sub == 'boss' or content == 'heart-container' or re.search(r'\bsoul-', cond):
        return 'boss'
    if sub == 'chest':
        return 'chest'
    return 'other'


def dungeon_entrances(seed_dir, settings):
    """overworld door key -> the dungeon key it leads into (identity without shuffle-dungeons)."""
    ent = settings.get('dungeon-entrances')
    if not ent and settings.get('shuffle-dungeons'):  # (seeds from before settings.json kept it: the spoiler)
        ent = {}
        sp = os.path.join(seed_dir, 'spoiler.txt')
        if os.path.exists(sp):
            on = False
            for ln in open(sp):
                if ln.startswith('dungeon-entrances:'):
                    on = True
                elif on:
                    m = re.match(r'\s+([a-z-]+) -> ([a-z-]+)\s*$', ln)
                    if not m:
                        break
                    ent[m.group(1)] = m.group(2)
    return {k: (ent or {}).get(k, k) for k in ENTRANCE_KEYS}


# the game's place names on the overworld map (datasheet UiFieldMapLocationName: mId = pane T_MapName_NN)
PLACE = ['Kanalet Castle', 'Rapids Ride', 'Southern Face Shrine', 'Animal Village', 'Yarna Desert', '', "Martha's Bay",
         'Pothole Field', '', '', 'Mabe Village', 'Mysterious Forest', 'Goponga Swamp', 'Koholint Prairie',
         'Tabahl Wasteland', 'Tal Tal Mountain Range', 'Mt. Tamaranch', 'Tal Tal Heights', 'Toronbo Shores',
         'Ukuku Prairie', 'Cemetery']


def field_areas(romfs):
    """The overworld split into the game's place names: (area_at(x, z) -> place id, screen_area {(row, col): id},
    boxes {id: (x0, z0, x1, z1)}). The Field level's grid (160 x 128 cells of 1.5 units, 8 bytes each: room CRC, ?,
    zone index) gives each spot its zone; datasheet LocationName names the zone, UiFieldMapLocationName the map label.
    Zones without a label of their own (south of the village, the east bay...) take their screen's, else the nearest."""
    import struct
    import fixedhash
    from RandomizerCore.Tools import oead_tools
    base = os.path.join(romfs, 'region_common')
    fh = fixedhash.FixedHash(open(os.path.join(base, 'level', 'Field', 'Field.lvb'), 'rb').read())
    zones = [struct.unpack_from('<I', z.data, 0)[0] for z in fh.get(b'zone').data.entries]
    grid = fh.get(b'grid').data
    gh, gw = struct.unpack_from('<HH', fh.get(b'gridConfig').data, 0)  # (rows, columns)
    sheets = os.path.join(base, 'datasheets')
    locs = [r for r in oead_tools.readSheet(os.path.join(sheets, 'LocationName.gsheet'))['values'] if r['mType'] == 0]
    label = {str(r['mNameLabel'])[:-4]: int(r['mId'])
             for r in oead_tools.readSheet(os.path.join(sheets, 'UiFieldMapLocationName.gsheet'))['values']
             if str(r['mNameLabel']).endswith('_map')}

    def zone_place(zid):
        for r in locs:
            if r['mMinZoneID'] <= zid <= r['mMaxZoneID']:
                return label.get(str(r['mNameLabel']), -1)
        return -1
    zp = [zone_place(z) for z in zones]
    cells = [[-1] * gw for _ in range(gh)]
    for r in range(gh):
        for c in range(gw):
            zi = grid[(r * gw + c) * 8 + 5]
            cells[r][c] = zp[zi] if zi < len(zp) else -1
    screen = {}
    for row in range(16):
        for col in range(16):
            n = {}
            for r in range(row * 8, row * 8 + 8):
                for c in range(col * 10, col * 10 + 10):
                    if r < gh and c < gw and cells[r][c] >= 0:
                        n[cells[r][c]] = n.get(cells[r][c], 0) + 1
            screen[(row, col)] = max(n, key=n.get) if n else -1
    for (row, col), a in list(screen.items()):  # (no labelled ground: the nearest labelled screen)
        if a < 0:
            best = min(((abs(r - row) + abs(c - col), b) for (r, c), b in screen.items() if b >= 0), default=(0, -1))
            screen[(row, col)] = best[1]
    boxes = {}
    for r in range(gh):
        for c in range(gw):
            a = cells[r][c] if cells[r][c] >= 0 else screen.get((r // 8, c // 10), -1)
            if a < 0:
                continue
            x0, z0, x1, z1 = boxes.get(a, (1e9, 1e9, -1e9, -1e9))
            boxes[a] = (min(x0, c * 1.5), min(z0, r * 1.5), max(x1, c * 1.5 + 1.5), max(z1, r * 1.5 + 1.5))

    def area_at(x, z):
        c, r = int(x / 1.5), int(z / 1.5)
        if 0 <= r < gh and 0 <= c < gw and cells[r][c] >= 0:
            return cells[r][c]
        return screen.get((min(15, max(0, int(z // 12))), min(15, max(0, int(x // 15)))), -1)
    return area_at, screen, boxes


def name_checks(checks, romfs):
    """Each overworld check's place ('ar': place id), readable name ('dn') and spot inside the place ('sp')."""
    from check_names import display_name, direction, house
    try:
        area_at, screen, boxes = field_areas(romfs)
    except Exception as e:  # (no level data: the runtime falls back to the nearest place name)
        print('tracker: no place names:', e)
        return
    seen = {}
    for c in checks:
        if c['g'] >= 0:  # (dungeon checks: their dungeon's list)
            m = re.match(r'D\d-pot-(\d\d[A-P])-', c['n'])
            c['dn'] = f"Pot ({c.get('dc') or m.group(1)})" if m else display_name(c['n'], c['k'])
            n = seen[c['dn']] = seen.get(c['dn'], 0) + 1
            if n > 1:
                c['dn'] += f' {n}'
            continue
        if 'wx' in c:
            x, z = c['wx'], c['wz']
            a = area_at(x, z)
        else:
            x, z = c['x'] * 15.0, c['y'] * 12.0  # (the screen's centre)
            a = screen.get((min(15, int(c['y'])), min(15, int(c['x']))), -1)
        c['ar'] = a
        place = PLACE[a] if 0 <= a < len(PLACE) else ''
        b = boxes.get(a)
        where = direction((x - b[0]) / max(1.0, b[2] - b[0]), (z - b[1]) / max(1.0, b[3] - b[1])) if b else ''
        numbered = c['k'] in ('bush', 'dig', 'pot') and re.match(r'(bush|dig|pot)-', c['n'])
        if numbered and c['k'] == 'pot' and c.get('lv') not in (None, 'Field'):
            name = f"Pot in {house(c['lv'])}"
        elif numbered:
            name = display_name(c['n'], c['k'], place, where)
        else:
            name = display_name(c['n'], c['k'])
        n = seen[name] = seen.get(name, 0) + 1
        c['dn'] = name if n == 1 else f'{name} {n}'
        c['sp'] = ('inside, ' if c.get('in') else '') + (where if where != 'centre' else 'middle')
    for c in checks:  # (the first of a numbered run gets its 1 too)
        if c.get('dn') and seen.get(c['dn'], 0) > 1:
            c['dn'] += ' 1'


def spell_level(lv):
    """A level id as words: 'MarinTarinHouse' -> 'Marin Tarin House'."""
    return re.sub(r'(?<=[a-z])(?=[A-Z0-9])', ' ', lv)


def condition(node, levels):
    parts = [node.get('condition-' + lv) for lv in levels if node.get('condition-' + lv)]
    return ' | '.join(f'({p})' for p in parts) if parts else ''


def build(seed_dir, romfs):
    _las()
    import yaml
    from RandomizerCore.Tools import leb, oead_tools
    from RandomizerCore.Randomizers import data as las_data
    import pots as pots_mod
    settings = json.load(open(os.path.join(seed_dir, 'settings.json')))
    levels = LEVELS.get(settings.get('logic', 'basic'), ['basic'])
    lpath = os.path.join(seed_dir, 'logic.json')
    if os.path.exists(lpath):
        logic = json.load(open(lpath))
    else:  # seeds generated before logic.json existed: base LAS logic (+ pots)
        logic = yaml.safe_load(open(os.path.join(LAS, 'RandomizerCore', 'Data', 'logic.yml')).read().split('---', 1)[1])
        if settings.get('pots'):
            ed = yaml.safe_load(open(os.path.join(HERE, 'entrance_data.yml')))
            items = {k: {'quantity': 0} for k in ('rupee-5', 'rupee-20', 'rupee-50', 'rupee-100')}
            pots_mod.add_to_logic(pots_mod.scan(romfs, leb), logic, items, ed)
    ed = yaml.safe_load(open(os.path.join(HERE, 'entrance_data.yml')))
    loc_rooms = json.load(open(os.path.join(HERE, 'location_rooms.json')))

    sheet_path = os.path.join(seed_dir, 'romfs', 'region_common', 'datasheets', 'GlobalFlags.gsheet')
    if not os.path.exists(sheet_path):
        sheet_path = os.path.join(romfs, 'region_common', 'datasheets', 'GlobalFlags.gsheet')
    flags = {str(v['symbol']): int(v['index']) for v in oead_tools.readSheet(sheet_path)['values']}
    check_flags = {}  # shopsanity / trendsanity checks: location -> done flag (shopsanity.py, trendsanity.py)
    cf = os.path.join(seed_dir, 'check_flags.tsv')
    if os.path.exists(cf):
        check_flags = {ln.split('\t')[0]: int(ln.split('\t')[1]) for ln in open(cf) if '\t' in ln}

    def room_file(room):
        lv = re.match(r'(.+)_\d\d[A-Z]', room).group(1)
        for base in (os.path.join(seed_dir, 'romfs', 'region_common', 'level'), os.path.join(romfs, 'region_common', 'level')):
            p = os.path.join(base, lv, room + '.leb')
            if os.path.exists(p):
                return leb.Room(open(p, 'rb').read())
        return None

    # where interiors are: level -> door cell, and LAS location -> door (entrance_data `inside`)
    level_cell, loc_door_cell = {}, {}
    for d in ed['doors']:
        c = cell_of_field(d['field_room'])
        level_cell.setdefault(d['dest_level'], c)
        for it in d.get('inside') or []:
            loc_door_cell[it['location']] = c
    nodes = []
    for name, v in logic.items():
        t = v.get('type')
        if t not in ('region', 'item', 'room', 'interaction', 'enemy', 'trick', 'follower'):
            continue
        n = {'n': name, 't': t, 'r': v.get('region', ''), 'c': condition(v, levels)}
        tl = condition(v, TRICK_LEVELS if len(levels) < len(TRICK_LEVELS) else levels)
        if tl != n['c']:
            n['trick'] = tl
        for x in ('pit', 'heavy'):
            if v.get('condition-' + x):
                n[x] = v['condition-' + x]
        nodes.append(n)

    groups, checks = [], []
    gidx = {}
    # dungeon icons: dungeon key (D1..D8, D0) -> the overworld cell of the door that leads into it now
    dkey = {lv: DUNGEONS[lv] for lv in ENTRANCE_KEYS.values()}
    door_of = {}
    for door, inside in dungeon_entrances(seed_dir, settings).items():
        lv_door, lv_in = ENTRANCE_KEYS.get(door), ENTRANCE_KEYS.get(inside)
        if lv_door and lv_in and level_cell.get(lv_door):
            door_of[dkey[lv_in]] = level_cell[lv_door]

    def group(dk):
        if dk not in gidx:
            cell = door_of.get(dk) or level_cell.get(next(k for k, x in DUNGEONS.items() if x == dk)) or (8.0, 8.0)
            gidx[dk] = len(groups)
            groups.append({'label': dk, 'x': cell[0], 'y': cell[1]})
        return gidx[dk]

    # shuffled dungeon layouts: (level, cell) -> room, and room -> [(level, cell)] (dungeon_gen.py dungeon_rooms.tsv)
    layout, rooms_at = {}, {}
    p = os.path.join(seed_dir, 'dungeon_rooms.tsv')
    if os.path.exists(p):
        for ln in open(p):
            c = ln.rstrip('\n').split('\t')
            if len(c) >= 3:
                layout[(c[0], c[1])] = c[2]
                rooms_at.setdefault(c[2], []).append((c[0], c[1]))
    shifts = {}
    p = os.path.join(seed_dir, 'dungeon_map.tsv')
    if os.path.exists(p):
        for ln in open(p):
            c = ln.rstrip('\n').split('\t')
            if len(c) >= 4:
                shifts[(c[0], c[1])] = (float(c[2]), float(c[3]))
    dungeon_of = settings.get('dungeon-of') or {}
    shuffled_levels = {lv for lv, _ in layout}

    def where(name, room, level):
        """(dungeon key, cell) a dungeon check sits in now, else (None, None)."""
        dk = dungeon_of.get(name)
        if room and room in rooms_at:
            places = rooms_at[room]
            pl = next((p for p in places if dk and DUNGEONS.get(p[0]) == dk), places[0])
            return DUNGEONS.get(pl[0]), pl[1]
        if level in DUNGEONS and level not in shuffled_levels:  # (vanilla layout: the room's own cell)
            return dk or DUNGEONS[level], room.rsplit('_', 1)[1] if room else None
        m = re.match(r'(D\d)-', name)
        return dk or (m.group(1) if m else DUNGEONS.get(level)), None

    region_cells = {}
    pos = {}
    shell_rooms, shell_flags, shell_pos = {}, {}, {}
    sp = os.path.join(seed_dir, 'shell_spots.tsv')
    if os.path.exists(sp):
        rows = [c for c in (ln.rstrip('\n').split('\t') for ln in open(sp)) if len(c) > 4 and not c[0].startswith('#')]
        shell_rooms = {c[0]: (c[3], c[4]) for c in rows}
        shell_pos = {c[0]: (float(c[5]), float(c[7])) for c in rows
                     if c[3] == 'Field' and len(c) > 7 and re.match(r'-?[\d.]+$', c[5]) and re.match(r'-?[\d.]+$', c[7])}
        shell_flags = {c[0]: int(c[12]) for c in rows if len(c) > 12 and c[12].lstrip('-').isdigit()}
    absent = set(settings.get('absent-locations', ()))  # (rooms the dungeon shuffle left out: not in the game)
    for name, v in logic.items():
        if v.get('type') != 'item' or name.startswith('starting-') or name in absent:
            continue
        lr = loc_rooms.get(name, {})
        room, level = lr.get('room'), lr.get('level')
        if not room and name in shell_rooms:  # (Secret Seashell spots: their room, tools/rando/seashell_spots.py)
            level, room = shell_rooms[name]
        g = -1
        cell = None
        dg = (None, None)
        if name.startswith('pot-') or re.match(r'D\d-pot-', name) or name.startswith('bush-'):
            continue  # pots and bushes come from pots.tsv / bushes.tsv below
        m = re.match(r'dig-(\d\d)([A-P])-(\d\d)$', name)
        if m:  # digsanity: on its screen; collected = flag 24576 + tile id (tools/rando/digs.py)
            row, col, tile = int(m.group(1)), ord(m.group(2)) - 65, int(m.group(3))
            cell = cell_of_field(f'Field_{m.group(1)}{m.group(2)}')
            checks.append({'n': name, 'x': cell[0], 'y': cell[1], 'g': -1, 'k': 'dig', 'a': v.get('region') or '',
                           'wx': col * 15.0 + tile % 10 * 1.5 + 0.75, 'wz': (row - 1) * 12.0 + tile // 10 * 1.5 + 0.75,
                           'flag': 24576 + ((row - 1) * 16 + col) * 80 + tile})
            continue
        if level in DUNGEONS or name in dungeon_of or (re.match(r'D(\d)-', name) and not room):
            dg = where(name, room, level)
            if dg[0]:
                g = group(dg[0])
                cell = (groups[g]['x'], groups[g]['y'])
        elif room and room.startswith('Field'):
            cell = cell_of_field(room)
        elif name in loc_door_cell:
            cell = loc_door_cell[name]
        elif level and level in level_cell:
            cell = level_cell[level]
        if cell and g < 0:
            region_cells.setdefault(v.get('region'), []).append(cell)
        pos[name] = (cell, g, room, dg)
    centroid = {r: (sum(c[0] for c in cs) / len(cs), sum(c[1] for c in cs) / len(cs)) for r, cs in region_cells.items()}
    for name, (cell, g, room, dg) in pos.items():
        if not cell:
            cell = centroid.get(logic[name].get('region'))
        if not cell:
            continue
        flag = check_flags.get(name, -1)
        sub = logic[name].get('subtype')
        try:
            if name in check_flags:
                pass
            elif name in getattr(las_data, 'HEART_FLAGS', {}):
                flag = flags.get(las_data.HEART_FLAGS[name], -1)
            elif name in underwater.SHELL_FLAGS:  # (the sunken seashells' own flags: tools/rando/underwater.py)
                flag = flags.get(underwater.SHELL_FLAGS[name], -1)
            elif name in shell_flags:  # (Secret Seashell spots: their own found flag)
                flag = shell_flags[name]
            elif room and sub in ('chest', 'standing', 'drop'):
                r = room_file(room)
                boxes = [a for a in r.actors if a.type == 0xf7] if r else []
                keys = [a for a in r.actors if a.type == 0xa9] if r else []
                if sub == 'chest' and len(boxes) == 1 and boxes[0].switches[1][0] == 1:
                    flag = boxes[0].switches[1][1]
                elif len(keys) == 1 and keys[0].switches[1][0] == 1:
                    flag = keys[0].switches[1][1]
        except Exception:
            pass
        checks.append({'n': name, 'x': cell[0], 'y': cell[1], 'g': g, 'flag': flag,
                       'k': kind_of(name, logic[name], shell_rooms), 'a': logic[name].get('region') or '',
                       'd': dg[0], 'dc': dg[1], 'in': name in loc_door_cell})
        if name in shell_pos:
            checks[-1]['wx'], checks[-1]['wz'] = shell_pos[name]
    # item pots (pots.tsv: level x y z flag) -> logic node name via the same naming as pots.py
    tsv = os.path.join(seed_dir, 'pots.tsv')
    if os.path.exists(tsv):
        by_level = {}
        for p in pots_mod.scan(romfs, leb):
            by_level.setdefault(p['level'], []).append(p)
        names = {}
        for lv, ps in by_level.items():
            for p in ps:
                r = room_file(p['room'])
                a = next((a for a in r.actors if a.key == p['key']), None) if r else None
                if a:
                    names[(lv, round(a.posX, 2), round(a.posZ, 2))] = (pots_mod.location_name(p), p['room'])
        for line in open(tsv):
            lv, x, y, z, fl = line.split()[:5]
            n, proom = names.get((lv, round(float(x), 2), round(float(z), 2)), (None, None))
            if not n or n not in logic or n in absent:
                continue
            g, dg = -1, (None, None)
            if lv in DUNGEONS or n in dungeon_of:
                dg = where(n, proom, lv)
                if not dg[0]:
                    continue
                g = group(dg[0])
                cell = (groups[g]['x'], groups[g]['y'])
            else:
                cell = level_cell.get(lv)
            if not cell:
                continue
            checks.append({'n': n, 'x': cell[0], 'y': cell[1], 'g': g, 'flag': int(fl), 'k': 'pot',
                           'a': logic[n].get('region') or '', 'd': dg[0], 'dc': dg[1], 'lv': lv})
            if lv == 'Field':
                checks[-1]['wx'], checks[-1]['wz'] = float(x), float(z)
    # bushsanity (bushes.tsv: level x y z flag): on the bush's Field screen (bush-<screen>-<n>)
    tsv = os.path.join(seed_dir, 'bushes.tsv')
    if os.path.exists(tsv):
        import bushes as bushes_mod
        names = {}
        for b in bushes_mod.scan(romfs, leb, pits=True):  # (older seeds hid items over pits too)
            r = room_file(b['room'])
            a = next((a for a in r.actors if a.key == b['key']), None) if r else None
            if a:
                names[(round(a.posX, 2), round(a.posZ, 2))] = bushes_mod.location_name(b)
        for line in open(tsv):
            lv, x, y, z, fl = line.split()[:5]  # (bushes.tsv rows may carry more: pots.write_tsv)
            n = names.get((round(float(x), 2), round(float(z), 2)))
            cell = n and cell_of_field('Field_' + n.split('-')[1])
            if n in logic and cell:
                checks.append({'n': n, 'x': cell[0], 'y': cell[1], 'g': -1, 'flag': int(fl), 'k': 'bush',
                               'a': logic[n].get('region') or '', 'wx': float(x), 'wz': float(z)})
    for dk in sorted(set(DUNGEONS.values())):
        if any(c.get('d') == dk for c in checks):
            group(dk)
    name_checks(checks, romfs)
    # the dungeon cells holding checks (vanilla layouts too): their floor page's shift, so the dungeon map draws a
    # room's bullet only on its own floor
    import dungeon_gen
    lv_of = {v: k for k, v in DUNGEONS.items()}
    for c in checks:
        lv = lv_of.get(c.get('d') or '')
        if lv and c.get('dc') and (lv, c['dc']) not in layout:
            layout[(lv, c['dc'])] = f"{lv}_{c['dc']}"
    for key in layout:
        if key not in shifts:
            try:
                shifts[key] = dungeon_gen.map_shift(romfs, key[0], f'{key[0]}_{key[1]}')
            except Exception:
                shifts[key] = (0.0, 0.0)
    rooms = [{'level': lv, 'cell': c, 'room': r, 'shift': shifts.get((lv, c), (0, 0))} for (lv, c), r in sorted(layout.items())]
    out = {'settings': {k: 1 for k, v in settings.items() if v is True}, 'nodes': nodes, 'checks': checks,
           'groups': groups, 'rooms': rooms, 'logic': settings.get('logic', 'basic'),
           'off': sorted(k for k, v in settings.items() if v is False)}
    with open(os.path.join(seed_dir, 'tracker.json'), 'w') as f:
        json.dump(out, f)
    # same data as tab-separated lines for the runtime (no JSON parser there)
    with open(os.path.join(seed_dir, 'tracker.tsv'), 'w') as f:
        f.write('V\t2\n')
        for k in out['settings']:
            f.write(f'S\t{k}\n')
        for k in out['off']:
            f.write(f'F\t{k}\n')
        rs = settings.get('dungeon-room-shuffle') or 'off'  # (las_headless.ROOM_MODES: older names read as the new)
        f.write(f"O\tdungeon-room-shuffle\t{ {'chaos': 'coupled', 'madness': 'decoupled'}.get(rs, rs)}\n")
        for n in nodes:
            f.write(f"N\t{n['t']}\t{n['n']}\t{n['r'] or '-'}\t{n['c'] or '-'}\t{n.get('pit', '-')}\t{n.get('heavy', '-')}"
                    f"\t{n.get('trick', '-') or '-'}\n")
        for g in groups:
            f.write(f"G\t{g['label']}\t{g['x']:.3f}\t{g['y']:.3f}\n")
        for c in checks:
            f.write(f"C\t{c['n']}\t{c['x']:.3f}\t{c['y']:.3f}\t{c['g']}\t{c['flag']}\t{c['k']}\t{c['a'] or '-'}"
                    f"\t{c.get('d') or '-'}\t{c.get('dc') or '-'}\t{c.get('ar', -1)}\t{c.get('dn') or '-'}\t{c.get('sp') or '-'}\n")
        for r in rooms:
            f.write(f"R\t{r['level']}\t{r['cell']}\t{r['room']}\t{r['shift'][0]:g}\t{r['shift'][1]:g}\n")
    return len(checks), len(groups)


if __name__ == '__main__':
    for d in sys.argv[1:]:
        print(d, build(os.path.abspath(d), os.path.join(ROOT, 'extracted', 'romfs')))
