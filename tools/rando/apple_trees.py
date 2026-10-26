#!/usr/bin/env python3
"""Applesanity (setting 'apples'): the 6 overworld apple trees hold 3 checks each (18).

An apple tree is an ObjTree with parameter 1 set (1: ItemDrop AppleLow, 2: Applehigh). Each time it loads it rolls
that row 3 times and hangs up to 3 apples in its crown; dashing into it (Pegasus Boots) knocks them down. With
Applesanity both rows give a red apple every roll (they are only used by these trees), so a tree always hangs 3. The
runtime (runtime/hle/apple_trees.c) takes away the apples of slots already found, so the apples left show the checks
left, and each apple is swapped for its slot's item model hanging in the crown (a trap shows a random real item of
the seed). On a bonk each hanging item (a SinkingSword item actor, runtime/hle/shell_spots.c) flies out to its own
free tile beside the tree on that tree's screen. Picking it up runs its entry in SinkingSword.bfevfl
and sets the slot's flag.

Logic: the region of the tile Link dashes from (reviewed in game, STAND) and boots. The landing tiles are kept in that
region. Filler: 18 apples (14 red, 4 green), quiet pickups like a recovery heart (an apple heals 1 heart); with traps,
the share pots and bushes use.

rando/<seed>/apple_trees.tsv uses shell_spots.tsv's columns (kind 'apple', x y z: where the item lands), then the
tree's actor key (hex) and the slot (0-2).

    scan(romfs, leb) -> [tree]
    add_to_logic(trees, logic_defs, item_defs, traps, rng) -> {location: (tree, slot)}
    apply(locs, mods, out_dir, romfs, leb, event_tools, oead_tools, item_get, data) -> spoiler lines
"""
import glob
import os
import re

import field_regions
import pots
import seashell_spots
import seed_tables

T_TREE = 0xbb  # ObjTree
SLOTS = 3
# the tile Link dashes into each tree from (checked in game). Every apple tree stands on a screen edge except 16M;
# 03I's backs onto the cliff (only from the south), 05K's sand path is the bridge region on both screens
STAND = {'Field_03I': 11, 'Field_05K': 29, 'Field_10F': 19, 'Field_11J': 29, 'Field_12A': 68, 'Field_16M': 46}
FILLER = [('apple', 14), ('green-apple', 4)]
FILLER_DEFS = {
    'apple': {'item-key': 'Apple', 'model-path': 'ObjAppleRed.bfres', 'model-name': 'AppleRed', 'npc-key': 'ItemApple'},
    'green-apple': {'item-key': 'GreenApple', 'model-path': 'ObjAppleGreen.bfres', 'model-name': 'AppleGreen',
                    'npc-key': 'ItemGreenApple'},
}
ROWS = ('AppleLow', 'Applehigh')  # (ItemDrop: only these trees use them)
INDEX_BASE = 100  # the rows' index column (shell_spots.c: never a seashell index)


def scan(romfs, leb):
    """The overworld apple trees: [{room, key, x, y, z, tile, zone, region, landings}]"""
    zones = field_regions.tile_zones(romfs)
    out = []
    for f in sorted(glob.glob(os.path.join(romfs, 'region_common', 'level', 'Field', 'Field_*.leb'))):
        room = os.path.basename(f)[:-4]
        with open(f, 'rb') as fh:
            r = leb.Room(fh.read())
        for a in r.actors:
            if a.type != T_TREE or len(a.parameters) < 2 or not a.parameters[1]:
                continue
            if room not in STAND:
                raise RuntimeError(f'{room}: an apple tree with no reviewed STAND tile')
            tile = STAND[room]
            region = field_regions.region_of(room, tile)
            same = lambda t, room=room, region=region: field_regions.region_of(room, t) == region
            # (in front of the trunk first, then beside it, behind it last: the crown hides an item there)
            z1 = round((a.posZ - (int(room[-3:-1]) - 1) * 12.0) / 1.5)  # (the trunk's front row)
            spots = []
            for row in (z1, z1 - 1, -99):
                spots = seashell_spots.landings(romfs, leb, room, a, SLOTS,
                                                lambda t, row=row: same(t) and t // 10 >= row, spots)
            if len(spots) < SLOTS:
                raise RuntimeError(f'{room}: only {len(spots)} free tiles beside the apple tree')
            out.append({'room': room, 'key': a.key, 'x': a.posX, 'y': a.posY, 'z': a.posZ, 'tile': tile,
                        'zone': zones[room][tile] if room in zones else None, 'landings': spots})
    return out


def location_name(tree, slot):
    return f"apple-tree-{tree['room'][-3:]}-{slot + 1}"


def add_to_logic(trees, logic_defs, item_defs, traps='none', rng=None):
    """A location per apple slot (the tree's region & boots) + 18 apples in the pool (traps by the trap setting)."""
    locs = {}
    for t in trees:
        for k in range(SLOTS):
            name = location_name(t, k)
            logic_defs[name] = field_regions.logic_entry(t['room'], t['tile'], t['zone'], 'boots')
            locs[name] = (t, k)
    for k, d in FILLER_DEFS.items():
        item_defs.setdefault(k, dict(d, type='junk', quantity=0, **{'chest-type': 'junk'}))
    n = len(locs)
    trap_kinds = [k for k in item_defs if k.endswith('-trap') and not k.startswith('zol')]
    n_traps = int(round(n * pots.POT_TRAPS.get(traps, 0.0))) if trap_kinds else 0
    if n_traps:
        import random
        rng = rng or random.Random(n)
        for _ in range(n_traps):
            item_defs[rng.choice(trap_kinds)]['quantity'] += 1
    left = n - n_traps
    for i, (item, k) in enumerate(FILLER):  # (traps take red apples' places first)
        k = left if i == len(FILLER) - 1 else max(0, k - n_traps)
        item_defs[item]['quantity'] += k
        left -= k
    return locs


def apply(locs, mods, out_dir, romfs, leb, event_tools, oead_tools, item_get, data):
    """Flags, flow entries, the red-apple ItemDrop rows and apple_trees.tsv. Returns spoiler lines."""
    common_out = os.path.join(out_dir, 'romfs', 'region_common')
    common_in = os.path.join(romfs, 'region_common')

    def path(rel):
        p = os.path.join(common_out, rel)
        return p if os.path.exists(p) else os.path.join(common_in, rel)

    def out(rel):
        p = os.path.join(common_out, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p

    sheet = oead_tools.readSheet(path('datasheets/GlobalFlags.gsheet'))
    drops = oead_tools.readSheet(path('datasheets/ItemDrop.gsheet'))
    for i, v in enumerate(drops['values']):  # (every roll a red apple: a tree always hangs 3)
        e = oead_tools.parseStruct(v)
        if e['mKey'] in ROWS:
            e['mLotTable'] = [{'mType': 'Apple', 'mCookie': 1, 'mWeight': 100}]
            drops['values'][i] = oead_tools.dictToStruct(e)
    flow = event_tools.readFlow(path('event/SinkingSword.bfevfl'))
    rows = ['# location\tindex\tkind\tlevel\troom\tx\ty\tz\tmodel\tname\tentry\tflag\tflag index\tshell\tscale\tturn'
            '\ttree\tslot  (tools/rando/apple_trees.py)\n']
    # a trap looks like a random real item of the seed (the item hangs in the tree for all to see)
    import random
    looks = sorted({(d['model-path'], d['model-name']) for it, d in mods.item_defs.items()
                    if it in set(v for v in mods.placements.values() if isinstance(v, str))
                    and d.get('model-path') and not d.get('item-key', '').endswith('Trap')})
    looks_rng = random.Random(f'{mods.seed}-apple-looks')
    checks, spoiler, tails = [], [], {}
    for n, (name, (t, k)) in enumerate(sorted(locs.items())):
        flag = f'LARApple{t["room"][-3:]}{k + 1}'
        index = seed_tables.next_flag(sheet, out_dir)
        sheet['values'].append({'symbol': flag, 'index': index})
        checks.append(f'{name}\t{index}\n')
        item_key, item_index, model_path, model_name = mods.getItemInfo(name, mods.trap_models)
        if item_key.endswith('Trap') and looks:
            model_path, model_name = looks_rng.choice(looks)
        quick = item_key[:3] == 'Rup' or mods.placements[name] in pots.QUICK
        tail_key = (item_key, item_index, quick)
        if tail_key not in tails:  # (one chain per item, shared: SinkingSword.bfevfl's buffer is small - pots.py)
            if quick:
                get_anim = event_tools.createActionEvent(flow.flowchart, 'Inventory', 'AddItemByKey',
                                                         {'itemKey': item_key, 'count': 1, 'index': item_index,
                                                          'autoEquip': False})
            else:
                get_anim = item_get.insertItemGetAnimation(flow.flowchart, item_key, item_index)
            tails[tail_key] = event_tools.createActionEvent(flow.flowchart, 'SinkingSword', 'Destroy', {}, get_anim)
        event_tools.addEntryPoint(flow.flowchart, name)
        set_flag = event_tools.createActionEvent(flow.flowchart, 'EventFlags', 'SetFlag',
                                                 {'symbol': flag, 'value': True}, tails[tail_key])
        event_tools.insertEventAfter(flow.flowchart, name, set_flag)
        x, y, z = t['landings'][k]
        rows.append('\t'.join(str(v) for v in (
            name, INDEX_BASE + n, 'apple', 'Field', t['room'], f'{x:g}', f'{y:g}', f'{z:g}', model_path, model_name,
            name, flag, index, 1 if item_key == 'Seashell' else 0, f'{data.MODEL_SIZES.get(model_name, 1.0):g}',
            f'{data.MODEL_ROTATIONS.get(model_name, 0):g}', f'{t["key"]:x}', k)) + '\n')
        spoiler.append(f'{name}: {mods.placements[name]}')
    oead_tools.writeSheet(out('datasheets/GlobalFlags.gsheet'), sheet)
    oead_tools.writeSheet(out('datasheets/ItemDrop.gsheet'), drops)
    event_tools.writeFlow(out('event/SinkingSword.bfevfl'), flow)
    with open(os.path.join(out_dir, 'apple_trees.tsv'), 'w') as f:
        f.writelines(rows)
    with open(os.path.join(out_dir, 'check_flags.tsv'), 'a') as f:  # tracker.py: location -> found flag
        f.writelines(checks)
    return spoiler
