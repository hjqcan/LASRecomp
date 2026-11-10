#!/usr/bin/env python3
"""Chest looks match contents (Randomizer setting 'chest-looks', default off).

Three looks, colour only (every chest keeps its size):
  - vanilla chest: anything that helps progression at some point (progression items, pets, souls, Triforce pieces,
    trade items / seashells when they are in logic)
  - key chest (blue with silver trim): small keys, Nightmare Keys
  - plain chest (dull brown): everything else (rupees, hearts, refills, maps, ...)
Traps copy the look of a random real item from the seed's pool.

The two new looks are recolours of the player's own chest texture (MI_dungeonTreasureBox_01_alb of
actor/ObjTreasureBox.bfres), made when the seed is made, written as actor/ObjTreasureBox{Key,Plain}.bfres. Each chest
names its model in actor parameters 6 / 7 and the chest's Npc row reads its graphics from there (LAS 'camc' way).

    python3 chest_looks.py <romfs> <out.png>   preview: vanilla / key / plain textures side by side
"""
import os
import random
import re
import struct
import sys
from io import BytesIO

from PIL import Image, ImageChops, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'third_party', 'LAS-Randomizer'))

TEXTURE = 'MI_dungeonTreasureBox_01_alb'
VANILLA = 'ObjTreasureBox.bfres'
FILES = {'key': 'ObjTreasureBoxKey.bfres', 'plain': 'ObjTreasureBoxPlain.bfres'}
# look: (body (hue 0-255 or None = keep, saturation factor, value factor), gold trim (same))
LOOKS = {'key': ((150, 1.0, 1.25), (None, 0.08, 0.85)), 'plain': ((16, 0.6, 0.62), (22, 0.45, 0.48))}
KEYS = ('key-', 'nightmare-key-')


def _clamp(x):
    return 0 if x < 0 else 255 if x > 255 else int(x)


def recolour(src, look):
    """src: RGB chest texture -> the look's recolour (RGB)."""
    h, s, v = src.convert('HSV').split()
    # the gold trim: saturated yellow / orange, and its bright red specks (the wood is darker)
    gold = ImageChops.multiply(h.point(lambda x: 255 if 16 <= x <= 52 else 0), s.point(lambda x: 255 if x > 70 else 0))
    specks = ImageChops.multiply(h.point(lambda x: 255 if x <= 16 or x >= 250 else 0), v.point(lambda x: 255 if x > 175 else 0))
    gold = ImageChops.lighter(gold, specks).filter(ImageFilter.GaussianBlur(0.8))

    def part(hue, sf, vf):
        hh = h.point(lambda x: hue) if hue is not None else h
        return Image.merge('HSV', (hh, s.point(lambda x: _clamp(x * sf)), v.point(lambda x: _clamp(x * vf)))).convert('RGB')
    body, trim = LOOKS[look]
    return Image.composite(part(*trim), part(*body), gold)


def _tokens(cond, settings):
    """A logic condition -> True (always met), False (never) or the set of names it can need. Settings that are on
    count as met (an 'open-bridge' route makes the bananas beside it unneeded); negated names need nothing."""
    toks = re.findall(r'[()&|!]|[a-zA-Z0-9\-]+(?:\[[a-z]+\])?(?::\d+)?', cond)
    pos = [0]

    def atom():
        t = toks[pos[0]]
        pos[0] += 1
        if t == '(':
            v = expr()
            pos[0] += 1  # ')'
            return v
        if t == '!':
            v = atom()
            return (not v) if isinstance(v, bool) else set()
        name = t.split(':')[0]
        v = settings.get(name.split('[')[0])
        return v if isinstance(v, bool) else {name}

    def join(vals, op):
        if (True if op == '|' else False) in vals:
            return op == '|'
        sets = [v for v in vals if not isinstance(v, bool)]
        if not sets:
            return op == '&'
        return set().union(*sets)

    def expr():
        ors, ands = [], [atom()]
        while pos[0] < len(toks) and toks[pos[0]] != ')':
            op = toks[pos[0]]
            pos[0] += 1
            if op == '&':
                ands.append(atom())
            else:
                ors.append(join(ands, '&'))
                ands = [atom()]
        ors.append(join(ands, '&'))
        return join(ors, '|')
    return expr() if toks else True


def needed_items(logic_defs, item_defs, settings, skip=()):
    """Items some check's logic can need (excluded / absent locations don't count) or the goal needs."""
    roots = [k for k, v in logic_defs.items() if v.get('type') in ('item', 'follower') and k not in skip]
    roots.append('kill-nightmare')
    seen, items, todo = set(), set(), roots
    while todo:
        name = todo.pop()
        base, _, param = name.partition('[')
        if name in seen or base not in logic_defs:
            if base in item_defs:
                items.add(base)
            continue
        seen.add(name)
        node = logic_defs[base]
        conds = [node.get('condition-basic')] + ([node.get('condition-' + param[:-1])] if param else [])
        if node.get('region'):
            todo.append(node['region'])
        for c in conds:
            v = _tokens(c, settings) if isinstance(c, str) else True
            if not isinstance(v, bool):
                todo.extend(v)
    return items


def look_of(item, item_defs, needed):
    """'vanilla' / 'key' / 'plain' for a (non-trap) item."""
    if item.startswith(KEYS):
        return 'key'
    if item_defs.get(item, {}).get('type') in ('important', 'triforce') or item in needed:
        return 'vanilla'
    return 'plain'


def is_trap(item):
    return item.endswith('-trap')


def install(placements, item_defs, logic_defs, settings):
    """Sets LAS's chest hook (mod_generator.CHEST_LOOK): location -> model file. Returns {look: chest count}."""
    from RandomizerCore import mod_generator, randomizer_data as rd
    from RandomizerCore.Randomizers import data
    skip = set().union(*(settings.get(k, ()) for k in ('excluded-locations', 'absent-locations', 'junk-locations')))
    needed = needed_items(logic_defs, item_defs, settings, skip)
    rng = random.Random(f"{settings['seed']}-chest-looks")
    real = sorted(v for k, v in placements.items() if isinstance(v, str) and k not in ('settings',) and not is_trap(v)
                  and v in item_defs)
    looks = {}
    for loc in sorted(data.CHEST_ROOMS):
        item = placements.get(loc)
        if item is None:
            continue
        if is_trap(item):  # a trap hides behind the look of a random real item
            item = rng.choice(real)
        looks[loc] = look_of(item, item_defs, needed)
    # Dampe's panel chests give his rewards: they stand out when one of them does
    dampe = [placements.get(loc) for loc in rd.DAMPE_REWARDS]
    panel = 'vanilla' if any(i and not is_trap(i) and look_of(i, item_defs, needed) != 'plain' for i in dampe) \
        else 'plain'
    mod_generator.CHEST_LOOK = lambda loc: FILES.get(panel if loc is None else looks[loc], VANILLA)
    count = {}
    for v in looks.values():
        count[v] = count.get(v, 0) + 1
    return count


def write_models(out_dir, romfs):
    """Writes actor/ObjTreasureBox{Key,Plain}.bfres: only the texture's pixel bytes change (as cosmetics.py)."""
    import cosmetics
    from RandomizerCore.Tools.bntx_editor import bntx_editor
    src = os.path.join(romfs, 'region_common', 'actor', VANILLA)
    data = open(src, 'rb').read()
    base = data.find(b'BNTX\0\0\0\0')
    if base < 0:
        raise RuntimeError('no BNTX in ObjTreasureBox.bfres')
    size = struct.unpack_from('<I', data, base + 0x1C)[0]
    for look, name in FILES.items():
        e = bntx_editor.BNTXEditor()
        e.openFile(data[base:base + size])
        i = next(i for i, t in enumerate(e.bntx.textures) if t.name == TEXTURE)
        tex = e.bntx.textures[i]
        img = Image.open(BytesIO(e.exportTexByIndex(i))).convert('RGB')
        fmt, size_, mips = tex.format_, tex.imageSize, list(tex.mipOffsets)
        off = base + struct.unpack_from('<Q', data, base + tex.ptrsAddr)[0]
        assert data[off:off + size_] == bytes(tex.data), 'texture data not where expected'
        new = e.bntx.replace(tex, tex.tileMode, True, False, False, True, BytesIO(cosmetics._dds(recolour(img, look))))
        if not new or new.format_ != fmt or len(new.data) != size_ or list(new.mipOffsets) != mips:
            raise RuntimeError('recoloured chest does not match the original layout')
        out = bytearray(data)
        out[off:off + size_] = new.data
        dst = os.path.join(out_dir, 'romfs', 'region_common', 'actor', name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(out)
    return len(FILES)


def sweep(out_dir, romfs, leb):
    """(After every room edit) any chest the seed's rooms hold without a model gets the vanilla one, so no chest is
    left without graphics. Returns the number fixed."""
    from RandomizerCore.Randomizers import data
    lvl_out = os.path.join(out_dir, 'romfs', 'region_common', 'level')
    names = set(data.CHEST_ROOMS.values()) | set(data.PANEL_CHEST_ROOMS.values())
    for lv in os.listdir(lvl_out) if os.path.isdir(lvl_out) else ():
        for f in os.listdir(os.path.join(lvl_out, lv)):
            if f.endswith('.leb'):
                names.add(f[:-4])
    fixed = 0
    for name in sorted(names):
        lv = name.rsplit('_', 1)[0]
        p = os.path.join(lvl_out, lv, name + '.leb')
        src = p if os.path.exists(p) else os.path.join(romfs, 'region_common', 'level', lv, name + '.leb')
        if not os.path.exists(src):
            continue
        raw = open(src, 'rb').read()
        if b'ObjTreasureBox' not in raw:
            continue
        room = leb.Room(raw)
        n = 0
        for a in room.actors:
            if a.type == 0xF7 and not (isinstance(a.parameters[6], bytes) and a.parameters[6].endswith(b'.bfres')):
                a.parameters[6], a.parameters[7] = VANILLA.encode(), b'TreasureBox'
                n += 1
        if n:
            os.makedirs(os.path.dirname(p), exist_ok=True)
            if os.path.islink(p):
                os.remove(p)
            with open(p, 'wb') as f:
                f.write(room.repack())
            fixed += n
    return fixed


if __name__ == '__main__':
    romfs, out = sys.argv[1:3]
    from RandomizerCore.Tools.bntx_editor import bntx_editor
    d = open(os.path.join(romfs, 'region_common', 'actor', VANILLA), 'rb').read()
    b = d.find(b'BNTX\0\0\0\0')
    e = bntx_editor.BNTXEditor()
    e.openFile(d[b:b + struct.unpack_from('<I', d, b + 0x1C)[0]])
    i = next(i for i, t in enumerate(e.bntx.textures) if t.name == TEXTURE)
    img = Image.open(BytesIO(e.exportTexByIndex(i))).convert('RGB')
    row = Image.new('RGB', (img.width * 3, img.height))
    for n, im in enumerate((img, recolour(img, 'key'), recolour(img, 'plain'))):
        row.paste(im, (n * img.width, 0))
    row.save(out)
