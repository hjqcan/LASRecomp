#!/usr/bin/env python3
"""Field walkability (LAS Recomp test tool, issue #144): which outdoor logic regions a Field tile can be walked to from.

Bushsanity / digsanity give each bush and dig tile the reviewed logic region of its tile (field_regions.py). This
module is an independent second opinion, from the game's own tile grids (used to build that table):

- every Field screen's 10 x 8 grid joined into one map (1.5-unit tiles, screen (row, col) at tile (col*10, (row-1)*8));
- a tile is walkable unless it has collision, is a pit / deep water, or something solid stands on it (rocks, statues,
  signs, trees ...; bushes and grass count as walkable: they can be cut);
- neighbours connect unless a wall bit sits between them or the floor heights differ by more than ELEV;
- each house / cave door (entrance_data.yml, outer_region) anchors its region to the area its doorway opens on.

So an area's anchors are the regions it can be walked to from without jumping pits, lifting rocks or swimming:
"(anchor1 | anchor2 ...)" is a lower bound of what reaching a tile there needs. The grid doesn't hold everything
(3D walls, one-way ledges, water currents), so this is a check for the logic - not the logic itself.

usage: field_reach.py <romfs> [rooms...]   (prints each listed screen's areas, or every mismatch with the logic)
"""
import collections
import glob
import os
import re
import sys

import yaml

import bushes
import digs

HERE = os.path.dirname(os.path.abspath(__file__))
ELEV = 1.0
# actors that don't block walking (the rest - rocks, statues, signs, trees, NPCs - do)
PASSABLE = ('Enemy', 'Tag', 'Area', 'Item', 'ObjButterfly', 'Map', 'ObjGrass', 'ObjLawn', 'ObjFieldTip')


def _tiles(romfs, leb):
    out = {}
    for f in sorted(glob.glob(os.path.join(romfs, 'region_common', 'level', 'Field', 'Field_*.leb'))):
        name = os.path.basename(f)[:-4]
        pos = bushes._screen_pos(name)
        if pos is None:
            continue
        with open(f, 'rb') as fh:
            room = leb.Room(fh.read(), edit_grid=True)
        if room.grid is None or room.grid.info.room_height != 8:
            continue
        row, col = pos
        solid = {bushes._tile(name, a) for a in room.actors if not a.name.decode('latin1').startswith(PASSABLE)}
        for i, t in enumerate(room.grid.tilesdata):
            f1 = t.flags1
            out[(col * 10 + i % 10, (row - 1) * 8 + i // 10)] = (
                not f1['containscollision'] and not f1['deepwaterlava'] and i not in solid,
                f1['northcollision'], f1['southcollision'], f1['eastcollision'], t.flags2['westcollision'], t.elevation)
    return out


def _neighbours(tiles, p):
    x, z = p
    t = tiles[p]
    for q, mine, theirs in (((x + 1, z), 3, 4), ((x - 1, z), 4, 3), ((x, z + 1), 2, 1), ((x, z - 1), 1, 2)):
        u = tiles.get(q)
        if u and u[0] and not t[mine] and not u[theirs] and abs(u[5] - t[5]) <= ELEV:
            yield q


def global_tile(room, tile):
    row, col = bushes._screen_pos(room)
    return col * 10 + tile % 10, (row - 1) * 8 + tile // 10


def areas(romfs, leb):
    """-> ({global tile: area id}, {area id: set of anchor regions})"""
    tiles = _tiles(romfs, leb)
    area = {}
    n = 0
    for p, t in tiles.items():
        if not t[0] or p in area:
            continue
        n += 1
        area[p] = n
        todo = [p]
        while todo:
            for q in _neighbours(tiles, todo.pop()):
                if q not in area:
                    area[q] = n
                    todo.append(q)
    anchors = collections.defaultdict(set)
    with open(os.path.join(HERE, 'entrance_data.yml')) as f:
        doors = yaml.safe_load(f)['doors']
    for d in doors:
        if not d.get('pos_xz') or not d.get('outer_region') or not d.get('field_room'):
            continue
        p = (int(d['pos_xz'][0] // 1.5), int(d['pos_xz'][1] // 1.5))
        near = [p] if p in area else [(p[0], p[1] + 1), (p[0], p[1] - 1), (p[0] + 1, p[1]), (p[0] - 1, p[1])]
        for q in near:
            if q in area:
                anchors[area[q]].add(d['outer_region'])
    return area, anchors


def tile_regions(romfs, leb):
    """{(Field room, tile): set of anchor regions} for every tile of an area with a door (else not listed)."""
    area, anchors = areas(romfs, leb)
    out = {}
    for (gx, gz), a in area.items():
        if anchors.get(a):
            out[(f'Field_{gz // 8 + 1:02d}{chr(65 + gx // 10)}', (gz % 8) * 10 + gx % 10)] = anchors[a]
    return out


def location_tiles(romfs, leb):
    """{bushsanity / digsanity location: (Field room, tile)}"""
    out = {}
    for b in bushes.scan(romfs, leb, pits=True):
        if b.get('tile') is not None:
            out[bushes.location_name(b)] = (b['room'], b['tile'])
    for d in digs.scan(romfs, leb):
        out[digs.location_name(d)] = (d['room'], d['tile'])
    return out


def main():
    romfs = os.path.abspath(sys.argv[1])
    sys.path.insert(0, os.path.normpath(os.path.join(HERE, '..', '..', 'third_party', 'LAS-Randomizer')))
    from RandomizerCore.Tools import leb
    area, anchors = areas(romfs, leb)
    if len(sys.argv) > 2:
        for r in sys.argv[2:]:
            row, col = int(r[-3:-1]), ord(r[-1]) - 65
            ids = sorted({area.get((col * 10 + x, (row - 1) * 8 + z)) for z in range(8) for x in range(10)} - {None})
            sym = {a: chr(97 + i) for i, a in enumerate(ids)}
            print(r, ', '.join(f'{sym[a]}: {sorted(anchors.get(a, ())) or "-"}' for a in ids))
            for z in range(8):
                print('   ', ' '.join(sym.get(area.get((col * 10 + x, (row - 1) * 8 + z)), '#') for x in range(10)))
        return
    import field_regions
    tr = tile_regions(romfs, leb)
    for name, (room, tile) in sorted(location_tiles(romfs, leb).items()):
        a = tr.get((room, tile))
        try:
            region, cond = field_regions.region_of(room, tile)
        except field_regions.NoRegion as e:
            print(f'{name:14} {e}')
            continue
        if a and not a & ({region} | set(re.findall(r'[a-z0-9-]+', cond or ''))):
            print(f'{name:14} walks from {sorted(a)}, logic {region}' + (f' + {cond}' if cond else ''))


if __name__ == '__main__':
    main()
