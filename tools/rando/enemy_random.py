#!/usr/bin/env python3
"""Enemy randomizer, mode Random (settings 'enemy-mode': 'random'): enemies are re-rolled by the runtime every time an
area loads (runtime/hle/enemy_random.c: room file reads and actor spawns), instead of staying as the seed's level
files have them.

The seed still gets a normal (Seeded) roll - that is what the files hold, and what a load starts from. LAS's
enemies.shuffleEnemyActors records each actor it randomizes (mod_generator.enemy_random: room -> its initial
restriction list and its actors in order, with their key, terrain class and vanilla look). Exactly those are re-rolled:
never an Excluded_Actors entry or an enemy a kill-all trigger watches (enemies.kill_linked), never an actor the seeded
roll made a mini-boss (mini-bosses stay where the seed put them, with their kits), never a non-enemy.

Written last, after every other room edit (pots, entrances, dungeon room shuffle...), so record offsets are final:
each recorded actor is found in the seed's .leb files by its key (rooms moved by the dungeon room shuffle keep their
actors' keys). Output: <seed>/enemies_random.tsv (only in Random seeds - its presence turns the runtime on):
  pick   <class> <ids...>                    what each terrain class may become (enemies.py's lists)
  no2d <ids...> / group <name> <ids...>      kept out of 2D rooms (already in their files' lists) / one per room
                                             (groups fly, block, one: Shadow Link, the Ball-and-chain Soldier,
                                             Blaino - with Include mini-bosses)
  mark   <flag> <ids...>                     these get switch 2 = <flag> when rolled (the mini-boss marker:
                                             miniboss_guard.py - copies die without their arena's event)
  enemy  <id> <enemy flag> <8 params>        params: - (empty string), i:<int>, f:<float>, c:<int|->,<int|->... (one picked)
  sizes  1                                   (randomize-enemy-sizes: a random 0.5 - 1.5 scale per load)
  file   <romfs path> <restr ids...>         a room file, then its actors in order:
  slot   <offset> <seed type> <vanilla type> <class> <empty> <vanilla e> <vanilla rotY> <vanilla scale x y z>
         <8 vanilla params, or S: they hold a string - use the enemy table's>
<empty>: the record's own name offset + the name's length - its NUL, an empty string parameter.
"""
import os
import struct

CLASSES = ('land', 'air', 'air_field', 'water', 'water2D', 'water-shallow', 'tree', 'hole')


def _param(p):
    if isinstance(p, list):
        return 'c:' + ','.join('-' if x in ('', b'') else str(int(x)) for x in p)  # ('-': an empty string)
    if isinstance(p, bytes):
        return '-' if p == b'' else None
    if isinstance(p, str):
        return '-' if p == '' else None
    if isinstance(p, float):
        return f'f:{p!r}'
    return f'i:{int(p)}'


def _params(ps):
    out = [_param(p) for p in (list(ps) + [b''] * 8)[:8]]
    return None if None in out else out


def write(out_dir, rooms, enemy_data, enemies_mod, sizes, leb, minibosses=False, marker=None):
    """rooms: mod_generator.enemy_random. minibosses: Include mini-bosses (Shadow Link, the Ball-and-chain Soldier and
    Blaino in the land pool). marker: (flag, ids) - miniboss_guard.py's marker and the types that carry it. Returns a
    short summary."""
    actors = enemy_data['Actors']
    ids = {k: [] for k in ('land', 'air', 'water', 'water2D', 'water-shallow', 'tree', 'hole')}
    table = []
    for v in actors.values():
        if v['type'] in ids and (minibosses or not v.get('minibosses-only')):
            ids[v['type']].append(v['id'])
        ps = _params(v.get('parameters', []))
        if ps is None:  # (strings in a pool enemy's own parameters: none today)
            raise ValueError(f'enemy {v["id"]:#x}: string parameters')
        table.append((v['id'], int(bool(v.get('enemy', False))), ps))
    no_vire = [x for x in ids['air'] if x != 0x26]
    picks = {'land': ids['land'], 'air': ids['air'], 'air_field': no_vire,
             'water': ids['water'] + ids['water-shallow'], 'water2D': ids['water2D'],
             'water-shallow': ids['water-shallow'], 'tree': ids['tree'], 'hole': ids['hole']}
    lines = ['# LAS Recomp: enemy randomizer, mode Random (tools/rando/enemy_random.py -> runtime/hle/enemy_random.c)']
    lines += ['pick\t%s\t%s' % (c, ' '.join('%#x' % x for x in picks[c])) for c in CLASSES]
    lines.append('no2d\t' + ' '.join('%#x' % x for x in enemies_mod.NO_2D))
    lines.append('group\tfly\t' + ' '.join('%#x' % x for x in enemies_mod.FLY_BOMBS))
    lines.append('group\tblock\t' + ' '.join('%#x' % x for x in enemies_mod.BLOCKING))
    if minibosses:
        lines.append('group\tone\t' + ' '.join('%#x' % x for x in enemies_mod.ONE_PER_ROOM))
    if minibosses and marker:
        lines.append('mark\t%d\t' % marker[0] + ' '.join('%#x' % x for x in sorted(marker[1])))
    lines += ['enemy\t%#x\t%d\t%s' % (i, e, ' '.join(ps)) for i, e, ps in table]
    if sizes:
        lines.append('sizes\t1')

    by_key = {}
    for room, r in rooms.items():
        for s in r['slots']:
            by_key[s['key']] = (room, s)
    lvl = os.path.join(out_dir, 'romfs', 'region_common', 'level')
    files = slots = 0
    missing = set(by_key)
    for level in sorted(os.listdir(lvl)) if os.path.isdir(lvl) else []:
        d = os.path.join(lvl, level)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if not f.endswith('.leb'):
                continue
            data = open(os.path.join(d, f), 'rb').read()
            found = []
            types = {a.key: a.type for a in leb.Room(data).actors}
            for key in types.keys() & by_key.keys():
                room, s = by_key[key]
                kb = struct.pack('<Q', key)
                at = data.find(kb)
                while at >= 0 and struct.unpack_from('<H', data, at + 0xC)[0] != types[key]:
                    at = data.find(kb, at + 1)  # (the record: the key, then its type at +0xC)
                if at < 0:
                    raise ValueError(f'{f}: actor {key:016x} has no record')
                found.append((at, room, s))
            if not found:
                continue
            found.sort()
            src_rooms = {room for _, room, _ in found}
            if len(src_rooms) != 1:
                raise ValueError(f'{f}: actors from {sorted(src_rooms)}')
            room = found[0][1]
            lines.append('file\tregion_common/level/%s/%s\t%s' % (
                level, f, ' '.join('%#x' % x for x in rooms[room]['restr'] if x >= 0) or '-'))
            for at, _, s in found:
                missing.discard(s['key'])
                stype = struct.unpack_from('<H', data, at + 0xC)[0]
                name_off = struct.unpack_from('<I', data, at + 0x8)[0]
                # (the name, NUL-terminated, is the record's first string: 'EnemyKeese-<key>')
                vtype, vparams, vrot, vscale, ve = s['vanilla']
                ps = _params(vparams)
                lines.append('slot\t%#x\t%#x\t%#x\t%s\t%d\t%d\t%r\t%r %r %r\t%s' % (
                    at, stype, vtype, s['cls'], name_off + s['name_len'], ve, vrot, *vscale,
                    ' '.join(ps) if ps else 'S'))
                slots += 1
            files += 1
    with open(os.path.join(out_dir, 'enemies_random.tsv'), 'w') as fo:
        fo.write('\n'.join(lines) + '\n')
    return f'{slots} enemies in {files} room files' + (f', {len(missing)} not found' if missing else '')
