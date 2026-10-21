#!/usr/bin/env python3
"""Secret Seashell spots as real checks: every place the original game hides a Secret Seashell - dug up with the
shovel (LAS subtype 'dig', 14), under a bush / rock / pot or in a tree Link dashes into ('hidden', 15), in a hole a
bomb goes into ('bomb-hole', 2) - holds a randomized item (LAS-Randomizer kept them vanilla: shuffler.py's
vanilla_locations). Logic: LAS's own conditions (shovel, bracelet, boots, bombs, ...).

In the game each one is the Secret Seashell actor (ItemSecretSeashell 0x87, parameter 0 = its seashell index):
  dig    placed in the room file, buried until the shovel digs its tile
  drop   spawned by the bush / rock / pot / tree / bomb hole through ItemDrop.gsheet's Seashell<n> entry
The Seashell Sensor rings for a buried shell and for a bush / rock / pot / tree / bomb hole whose drop is a shell not
yet owned. So:
  - a Secret Seashell placed at a spot stays the game's own shell there, with the seashell index LAS gave it (buried
    shell / bomb hole: that index; a host: its own ItemDrop entry LARShell<index>): the sensor rings until it is found
  - any other item: the buried shell is taken out (the runtime brings the item up when the spot is dug); a host drops
    ItemDrop LARShellSpot<n> - a Heart Piece tagged TAG + the spot's vanilla index; a bomb hole is left as it is (it
    only stays while the shell its index names isn't owned: the runtime keeps it on one that isn't, and takes its
    shell drop for the item), its sensor beacon switched off by the runtime. The runtime (runtime/hle/shell_spots.c) turns the tagged drop into the seed's item: a
    SinkingSword item actor (the actor LAS uses for standing items) with the item's model, where the shell would
    appear (out of a tree's crown / a bomb hole: it flies out on the shell's arc and comes down on a free tile beside
    it). Picking it up runs its entry in SinkingSword.bfevfl - LAS's item-get, the game's message box - and sets the
    spot's own flag.
A spot holding a shell gets its found flag from the runtime once that shell is owned (the tracker); so does a
digsanity tile holding one (digs.py makes it a buried shell: the shells argument).

rando/<seed>/shell_spots.tsv: location, seashell index (shell: the placed shell's), kind (dig / drop / land: lands at
x y z - trees / hole: a bomb hole, lands at x y z / shell: a Secret Seashell there), level, room, x, y, z, model path,
model name, flow entry, flag symbol, flag index, seashell (1/0), scale, turn (, a bomb hole's actor key: the runtime
switches its Seashell Sensor beacon off and keeps its index on a shell not owned).

Who owns a spot (one check per spot, never two): bushsanity the bushes hiding a shell, potsanity the Ghost House
pot, digsanity the shells buried on the Field - each such spot is that -sanity's own check (gold bush / pot, dig
tile), its LAS location taken out of the logic (absorb(): its seashell stays in the pool, its conditions ANDed in).
Shellsanity (setting 'shellsanity') makes every spot no -sanity took a check here; off, they stay vanilla shells
(apply(vanilla=True): only their 'shell' rows, so the tracker ticks them once their shell is found).

    apply(locations(logic_defs), mods, out_dir, romfs, leb, event_tools, oead_tools, item_get, data) -> spoiler lines
"""
import glob
import os
import re

import seed_tables

SUBTYPES = ('dig', 'hidden', 'bomb-hole')
T_SHELL = 0x87
T_SHELL_DROP_AREA = 0x233  # AreaShellDrop: the bomb holes (parameter 0 = the seashell index)
BURIED_LEVELS = ('Field', 'DogHouse')  # (levels whose rooms place a buried Secret Seashell)
LAWN = 0xb5  # ObjLawn: tall grass (an item may land in it)
TREES = (0xbb, 0xbc)  # ObjTree, ObjPalmTree: the shell falls out when Link dashes into one (Pegasus Boots)
TAG = 200  # a retagged drop: a Heart Piece (or a bomb hole's shell) with index TAG + the spot's vanilla seashell index
# where a bomb hole's item lands when the tile grid can't tell: the Tal Tal hole (02J) is the pit on top of a pillar
# below the plateau east of the rope bridge (Cucco House); the shell flies up and toward that plateau's south-west
# corner, the reachable ledge over the pit (the grid's free tiles beside the pit are the sand below the pillar)
HOLE_LANDING = {'Field_02J': (151.25, 25.5, 12.25)}
DROP_KEY = re.compile(rb'Seashell(\d+)$')  # a bush's / rock's / tree's / pot's ItemDrop key


def locations(logic_defs):
    """{location: seashell index} of the Secret Seashell spots."""
    return {k: int(v['index']) for k, v in logic_defs.items()
            if v.get('type') == 'item' and v.get('subtype') in SUBTYPES and v.get('content') == 'seashell'
            and 'index' in v}


def _rooms(romfs, leb, levels=None):
    pat = os.path.join(romfs, 'region_common', 'level', '*', '*.leb')
    for f in sorted(glob.glob(pat)):
        level = os.path.basename(os.path.dirname(f))
        if levels and level not in levels:
            continue
        with open(f, 'rb') as fh:
            yield level, os.path.basename(f)[:-4], leb.Room(fh.read())


def _landing(romfs, leb, room, host, tree):
    """Where the item of a tree (ObjTree / ObjPalmTree: the shell flies out of its crown) or a bomb hole (the shell pops
    out of it) lands - the runtime flies it there on the shell's arc, not where the shell itself would come down (a
    wall, water or the tree): the nearest free tile around (a tree stands on the corner of 2 x 2 tiles, a hole is
    one), in front - toward the camera - first, at most 2 units above / below the host. None: none."""
    with open(os.path.join(romfs, 'region_common', 'level', 'Field', room + '.leb'), 'rb') as fh:
        r = leb.Room(fh.read(), edit_grid=True)
    m = re.match(r'Field_(\d\d)([A-P])$', room)
    ox, oz = (ord(m.group(2)) - 65) * 15.0, (int(m.group(1)) - 1) * 12.0
    if tree:  # (tiles x0..x1, z0..z1 under the host)
        cx, cz = round((host.posX - ox) / 1.5), round((host.posZ - oz) / 1.5)
        x0, x1, z0, z1 = cx - 1, cx, cz - 1, cz
    else:
        x0 = x1 = int((host.posX - ox) // 1.5)
        z0 = z1 = int((host.posZ - oz) // 1.5)
    taken = {(int((a.posX - ox) // 1.5), int((a.posZ - oz) // 1.5)) for a in r.actors
             if a is not host and a.type != LAWN and not a.name.startswith((b'Enemy', b'Tag', b'Area', b'Item'))}
    cands = []
    for ring in (1, 2):
        for tz in range(z0 - ring, z1 + ring + 1):
            for tx in range(x0 - ring, x1 + ring + 1):
                if max(x0 - tx, tx - x1, z0 - tz, tz - z1) == ring:
                    dx, dz = tx - (x0 + x1) / 2, tz - (z0 + z1) / 2
                    cands.append((ring, -dz, abs(dx), tx, tz))  # (nearest ring, then south, then centred)
    for _, _, _, tx, tz in sorted(cands):
        if not (0 <= tx < 10 and 0 <= tz < 8) or (tx, tz) in taken:
            continue
        t = r.grid.tilesdata[tz * 10 + tx]
        if t.flags1['containscollision'] or t.flags3['iswaterlava'] or t.flags1['deepwaterlava'] \
                or abs(t.elevation - host.posY) > 2.0:
            continue
        return ox + tx * 1.5 + 0.75, t.elevation, oz + tz * 1.5 + 0.75
    return None


def scan(romfs, leb):
    """{seashell index: (kind, level, room, x, y, z, actor key, parameter)}: the buried shells (dig: where), what drops
    the others (drop: the host's position; the item appears where the shell would), the trees (land) and bomb
    holes (hole): where the item lands; the actor holding the shell and which of its parameters is the index / ItemDrop key."""
    out = {}
    for level, room, r in _rooms(romfs, leb):
        for a in r.actors:
            if a.type == T_SHELL and level in BURIED_LEVELS and isinstance(a.parameters[0], int):
                out[a.parameters[0]] = ('dig', level, room, a.posX, a.posY, a.posZ, a.key, 0)
            elif a.type == T_SHELL_DROP_AREA and isinstance(a.parameters[0], int):
                spot = HOLE_LANDING.get(room) or _landing(romfs, leb, room, a, False)
                if not spot:
                    raise RuntimeError(f'{room}: no free tile beside the bomb hole {a.name}')
                out.setdefault(a.parameters[0], ('hole', level, room) + spot + (a.key, 0))
            else:
                for k, p in enumerate(a.parameters[:2]):
                    m = isinstance(p, bytes) and DROP_KEY.match(p)
                    if not m:
                        continue
                    spot = a.type in TREES and level == 'Field' and _landing(romfs, leb, room, a, True)
                    if a.type in TREES and not spot:
                        raise RuntimeError(f'{room}: no free tile beside the tree {a.name}')
                    out.setdefault(int(m.group(1)), (('land', level, room) + spot if spot else
                                                     ('drop', level, room, a.posX, a.posY, a.posZ)) + (a.key, k))
    return out


def holds_shell(actor):
    """A bush / pot whose drop is a Secret Seashell."""
    return shell_index(actor) is not None


def shell_index(actor):
    """The seashell index a bush / pot / rock / tree drops (its ItemDrop key Seashell<n>), None: no shell."""
    for p in actor.parameters[:2]:
        m = isinstance(p, bytes) and DROP_KEY.match(p)
        if m:
            return int(m.group(1))
    return None


def clear_drop(actor):
    """A host taken over by a -sanity check: no Secret Seashell drop any more (ItemDrop 'None')."""
    for k, p in enumerate(actor.parameters[:2]):
        if isinstance(p, bytes) and DROP_KEY.match(p):
            actor.parameters[k] = b'None'


def _and(*conds):
    parts = [c for c in conds if c]
    return ' & '.join(f'({c})' for c in parts) if parts else None


def absorb(logic_defs, index, loc):
    """A -sanity check (bush / pot / dig tile, its new logic entry `loc`) standing on the Secret Seashell spot with
    seashell index `index`: that spot's LAS location leaves the logic - its seashell stays in the item pool and fills
    the check's place, so the caller adds no filler for it - and its conditions (and region) are ANDed into `loc`'s.
    Returns the LAS location taken, None: no such spot (already gone)."""
    las = next((n for n, i in locations(logic_defs).items() if i == index), None)
    if las is None:
        return None
    v = logic_defs.pop(las)
    base = _and(loc.get('condition-basic'), v['region'] if v.get('region') != loc.get('region') else None)
    loc['condition-basic'] = _and(base, v.get('condition-basic'))
    for k, c in v.items():
        if k.startswith('condition-') and k != 'condition-basic':
            loc[k] = _and(base, c)
    return las


def _tag_drop(oead_tools, sheet, key, index):
    """ItemDrop entry <key>: the Seashell<index> entry's copy dropping a Heart Piece tagged TAG + index (the runtime
    turns it into the spot's item; a dropped Secret Seashell would make the Seashell Sensor ring)."""
    base = next(v for v in sheet['values'] if v['mKey'] == f'Seashell{index}')
    if not any(v['mKey'] == key for v in sheet['values']):
        entry = oead_tools.parseStruct(base)
        entry['mKey'] = key
        entry['mLotTable'] = [{'mType': 'HeartPiece', 'mCookie': TAG + index, 'mWeight': 1}]
        sheet['values'].append(oead_tools.dictToStruct(entry))


def apply(locs, mods, out_dir, romfs, leb, event_tools, oead_tools, item_get, data, shells=(), vanilla=False):
    """locs: locations(logic_defs) (the spots no -sanity absorbed). A Secret Seashell placed at a spot stays the game's
    own shell there, with its seashell index (the Seashell Sensor rings for it until it is found, as for any shell);
    any other item: its own flow entry, the shell taken out (buried) / the drop retagged (ItemDrop LARShellSpot<n>).
    vanilla (Shellsanity off): the spots keep their own shells, untouched - only their 'shell' rows (tracker).
    shells: more spots holding a Secret Seashell [(location, seashell index, found flag)] (digsanity tiles: digs.py) -
    shell_spots.tsv rows the runtime sets the flag of once that shell is found (the tracker)."""
    common_out = os.path.join(out_dir, 'romfs', 'region_common')
    common_in = os.path.join(romfs, 'region_common')

    def path(rel):
        p = os.path.join(common_out, rel)
        return p if os.path.exists(p) else os.path.join(common_in, rel)

    def out(rel):
        p = os.path.join(common_out, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p

    spots = scan(romfs, leb)
    missing = [k for k, i in locs.items() if i not in spots]
    if missing:
        raise RuntimeError(f'Secret Seashell spots not found in the rooms: {missing}')

    sheet = oead_tools.readSheet(path('datasheets/GlobalFlags.gsheet'))
    names = {str(v['symbol']): int(v['index']) for v in sheet['values']}
    drops = oead_tools.readSheet(path('datasheets/ItemDrop.gsheet'))
    flow = event_tools.readFlow(path('event/SinkingSword.bfevfl'))
    rooms = {}

    def room_of(level, room):
        if (level, room) not in rooms:
            with open(path(f'level/{level}/{room}.leb'), 'rb') as fh:
                rooms[(level, room)] = leb.Room(fh.read())
        return rooms[(level, room)]

    rows = ['# location\tindex\tkind\tlevel\troom\tx\ty\tz\tmodel\tname\tentry\tflag\tflag index\tshell\tscale\tturn'
            '  (tools/rando/seashell_spots.py)\n']
    checks, spoiler = [], []
    # a bomb hole holding something else keeps its vanilla index and ItemDrop entries: the hole only stays while the
    # shell its index names isn't owned (LAS placed that shell somewhere), so the runtime keeps it on an index whose
    # shell isn't owned and takes the hole's own shell drop for the item (runtime/hle/shell_spots.c hole_of)
    for name, index in sorted(locs.items(), key=lambda kv: kv[1]):
        if not vanilla and name not in mods.placements:
            continue
        flag = f'LARShellSpot{index}'
        if flag not in names:
            names[flag] = seed_tables.next_flag(sheet, out_dir)
            sheet['values'].append({'symbol': flag, 'index': names[flag]})
        kind, level, room, x, y, z, key, k = spots[index]
        checks.append(f'{name}\t{names[flag]}\n')
        if vanilla:  # (its own shell, its own index: nothing to edit)
            rows.append(f'{name}\t{index}\tshell\t{level}\t{room}\t-\t-\t-\t-\t-\t-\t{flag}\t{names[flag]}\t1\t1\t0\n')
            continue
        item_key, item_index, model_path, model_name = mods.getItemInfo(name, mods.trap_models)
        host = next(a for a in room_of(level, room).actors if a.key == key)
        spoiler.append(f'{name}: {mods.placements[name]}')
        hole = kind == 'hole'
        if mods.placements[name] == 'seashell':
            # the game's own shell, with the index LAS gave it (a host's own ItemDrop entry: Seashell<n> may be retagged)
            if isinstance(host.parameters[k], int):
                host.parameters[k] = item_index
            else:
                host.parameters[k] = b'LARShell%d' % item_index
                if not any(v['mKey'] == f'LARShell{item_index}' for v in drops['values']):
                    e = oead_tools.parseStruct(next(v for v in drops['values'] if v['mKey'] == 'Seashell0'))
                    e['mKey'] = f'LARShell{item_index}'
                    e['mLotTable'] = [{'mType': 'Seashell', 'mCookie': item_index, 'mWeight': 1}]
                    drops['values'].append(oead_tools.dictToStruct(e))
            rows.append(f'{name}\t{item_index}\tshell\t{level}\t{room}\t-\t-\t-\t-\t-\t-\t{flag}\t{names[flag]}'
                        f'\t1\t1\t0\n')
            continue
        if kind == 'dig':  # (no buried shell: the runtime brings the item up when the spot is dug)
            r = room_of(level, room)  # (with the room's links renumbered: actors refer to each other by index)
            from RandomizerCore.Randomizers import enemies
            enemies.remove_actors(r, [r.actors.index(host)])
        elif hole:  # (the runtime keeps its sensor beacon off and its index on a shell not owned)
            host.parameters[k] = index
        else:
            host.parameters[k] = b'LARShellSpot%d' % index
            _tag_drop(oead_tools, drops, f'LARShellSpot{index}', index)
        if item_key[:3] == 'Rup':
            get_anim = event_tools.createActionEvent(flow.flowchart, 'Inventory', 'AddItemByKey',
                                                     {'itemKey': item_key, 'count': 1, 'index': item_index,
                                                      'autoEquip': False})
        else:
            get_anim = item_get.insertItemGetAnimation(flow.flowchart, item_key, item_index)
        event_tools.addEntryPoint(flow.flowchart, name)
        event_tools.createActionChain(flow.flowchart, name, [  # (the flag first: found, whatever the get does)
            ('EventFlags', 'SetFlag', {'symbol': flag, 'value': True}),
            ('SinkingSword', 'Destroy', {})], get_anim)
        rows.append('\t'.join(str(v) for v in (
            name, index, kind, level, room, f'{x:g}', f'{y:g}', f'{z:g}', model_path, model_name, name, flag,
            names[flag], 1 if item_key == 'Seashell' else 0, f'{data.MODEL_SIZES.get(model_name, 1.0):g}',
            f'{data.MODEL_ROTATIONS.get(model_name, 0):g}')) + (f'\t{key:x}' if hole else '') + '\n')
    for name, index, flag_index in shells:
        rows.append(f'{name}\t{index}\tshell\t-\t-\t-\t-\t-\t-\t-\t-\t-\t{flag_index}\t1\t1\t0\n')
    for (level, room), r in rooms.items():
        with open(out(f'level/{level}/{room}.leb'), 'wb') as fh:
            fh.write(r.repack())
    oead_tools.writeSheet(out('datasheets/GlobalFlags.gsheet'), sheet)
    oead_tools.writeSheet(out('datasheets/ItemDrop.gsheet'), drops)
    event_tools.writeFlow(out('event/SinkingSword.bfevfl'), flow)
    with open(os.path.join(out_dir, 'shell_spots.tsv'), 'w') as f:
        f.writelines(rows)
    with open(os.path.join(out_dir, 'check_flags.tsv'), 'a') as f:  # tracker.py: location -> found flag
        f.writelines(checks)
    with open(os.path.join(out_dir, 'flags_ext'), 'a') as f:  # (the runtime sets found flags: ours, any index)
        f.write('seashell spots: found flags set by the runtime (runtime/hle/shell_spots.c)\n')
    return spoiler
