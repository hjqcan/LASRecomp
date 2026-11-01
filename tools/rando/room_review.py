#!/usr/bin/env python3
"""Dungeon room review page: every grid room as the room logic sees it (room_logic.room_areas: the areas Link walks
between, what crosses from one to another, doorways and their doors, stairs and passages), for checking against the
game. Writes one self-contained HTML file.

usage: room_review.py <romfs> <out.html>"""
import html
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'third_party', 'LAS-Randomizer'))
import room_logic as rl  # noqa: E402
import dungeon_gen  # noqa: E402

NAMES = {'Lv01TailCave': 'Tail Cave', 'Lv02BottleGrotto': 'Bottle Grotto', 'Lv03KeyCavern': 'Key Cavern',
         'Lv04AnglersTunnel': "Angler's Tunnel", 'Lv05CatfishsMaw': "Catfish's Maw", 'Lv06FaceShrine': 'Face Shrine',
         'Lv07EagleTower': "Eagle's Tower", 'Lv08TurtleRock': 'Turtle Rock', 'Lv10ClothesDungeon': 'Color Dungeon'}
ORDER = ['Lv01TailCave', 'Lv02BottleGrotto', 'Lv03KeyCavern', 'Lv04AnglersTunnel', 'Lv05CatfishsMaw', 'Lv06FaceShrine',
         'Lv07EagleTower', 'Lv08TurtleRock', 'Lv10ClothesDungeon']
WORDS = {'rs-jump': "Roc's Feather, or Rooster (companions in dungeons)",
         'rs-long-jump': "Roc's Feather + Pegasus Boots, or Rooster (companions in dungeons)", 'feather': "Roc's Feather", 'rooster': 'Rooster', 'bad-pets': 'companions in dungeons', 'boots': 'Pegasus Boots',
         'flippers': 'flippers', 'hookshot': 'hookshot', 'bracelet': 'bracelet', 'bracelet:2': 'bracelet L2',
         'bombs': 'bombs', 'break-crystal': 'hit a crystal', 'rs-shutter': 'shutter opens',
         'rs-switch-shutter': 'switch opens shutter'}
DOOR_WORDS = {'lock': 'locked door', 'bossdoor': 'Nightmare Key door', 'shutter': 'shutter', 'cracked': 'bombable wall',
              'potdoor': 'pot door', 'oneway': 'one-way door', 'blocked:water': 'water', 'blocked:crystal': 'crystal',
              'blocked:block': 'block', 'blocked:statue': 'statue', 'blocked:chest': 'chest'}
T = 22  # px per tile


def req_text(alts):
    out = []
    for a in alts:
        if not a:
            return 'free'
        out.append(' + '.join(WORDS.get(x, x) for x in sorted(a)))
    return ' or '.join(out)


def area_names(n):
    return [chr(ord('A') + i) if i < 26 else str(i) for i in range(n)]


def room_card(romfs, cat, room, stairs, inward):
    g = rl.room_tiles(romfs, room)
    area, n, moves = rl.room_areas(romfs, room)
    nm = area_names(n)
    cell = room.rsplit('_', 1)[1]
    flags = []
    svg = [f'<svg viewBox="0 0 {10 * T} {8 * T}" role="img" aria-label="{html.escape(room)} tile map">']
    if g is None:
        svg.append(f'<rect width="{10 * T}" height="{8 * T}" class="t-none"/>'
                   f'<text x="{5 * T}" y="{4 * T}" class="lbl" text-anchor="middle">no tile grid</text>')
        flags.append('no tile grid: one area')
    else:
        for (r, c), (kind, h) in sorted(g.items()):
            cls = {'solid': 't-wall', 'pit': 't-lava' if room.startswith('Lv08') else 't-pit',
                   'water': 't-water'}.get(kind)
            if kind == 'floor':
                cls = f'a{area[(r, c)] % 8}' if (r, c) in area else 't-none'
            svg.append(f'<rect x="{c * T}" y="{r * T}" width="{T}" height="{T}" class="{cls}"/>')
            if (r, c) in area and n > 1:
                svg.append(f'<text x="{c * T + T / 2}" y="{r * T + T * 0.68}" class="al">{nm[area[(r, c)]]}</text>')
            if kind == 'floor' and h is not None and abs(h) >= 0.5:
                svg.append(f'<text x="{c * T + 2}" y="{r * T + 7}" class="ht">{h:g}</text>')
    for side, lst in dungeon_gen.openings(cat, room).items():
        for slot, _, _ in lst:
            tiles, _ = rl._doorway_tiles(side, slot)
            for r, c in tiles:
                svg.append(f'<rect x="{c * T + 3}" y="{r * T + 3}" width="{T - 6}" height="{T - 6}" class="door"/>')
    for x, z, kind in stairs:
        import rooms as _rooms
        ox, oz = _rooms.cell_origin(room)
        c, r = (x - ox) / 1.5, (z - oz) / 1.5
        svg.append(f'<circle cx="{c * T}" cy="{r * T}" r="{T * 0.32}" class="stair"/>'
                   f'<text x="{c * T}" y="{r * T + 3.5}" class="sl">{kind}</text>')
    svg.append('</svg>')
    # doorways
    drows = []
    for side, lst in dungeon_gen.openings(cat, room).items():
        for slot, _, _ in lst:
            doors = rl.own_doors(cat, room, side, slot)
            ar = rl.doorway_areas(romfs, room, side, slot)
            what = ', '.join(DOOR_WORDS.get(d, d) for d in doors) or 'open'
            if 'potdoor' in doors:
                t = rl.throw_req(romfs, room)
                what += f' (throw: {req_text([t]) if t else "nothing to throw"})'
            kind = inward.get((room, side, slot))
            if kind:
                what += f' - drop-in only ({kind})'
            where = f'tiles {slot[1:]}' if isinstance(slot, str) else f'slot {slot}'
            drows.append(f'<li><b>{side}</b> {where}: {html.escape(what)}'
                         + (f' <span class="to">into {", ".join(nm[a] for a in sorted(ar))}</span>' if n > 1 else '')
                         + '</li>')
    mrows = [f'<li>{nm[a]} → {nm[b]}: {html.escape(req_text(alts))}</li>' for a, b, alts in moves]
    if n > 1:
        reached = {b for _, b, _ in moves} | {a for a in range(n) if any(a in rl.doorway_areas(romfs, room, s, sl)
                                                                          for s, lst in dungeon_gen.openings(cat, room).items()
                                                                          for sl, _, _ in lst)}
        lone = [nm[a] for a in range(n) if a not in reached and not any(a in rl.spot_areas(romfs, room, x, z)
                                                                        for x, z, _ in stairs)]
        if lone:
            flags.append(f'area {", ".join(lone)}: no doorway, stairs or crossing reaches it')
    unit = cat.get(room, {}).get('unit', [room])
    utext = (' · part of ' + ', '.join(u.rsplit('_', 1)[1] for u in unit)) if len(unit) > 1 else ''
    return (f'<article class="room{" multi" if n > 1 else ""}" id="{html.escape(room)}">'
            f'<header><h3>{cell}</h3><span class="meta">{n} area{"s" if n != 1 else ""}{utext}</span></header>'
            + ''.join(svg) +
            ('<ul class="flags">' + ''.join(f'<li>{html.escape(f)}</li>' for f in flags) + '</ul>' if flags else '') +
            '<div class="cols"><div><h4>Doorways</h4><ul>' + (''.join(drows) or '<li>none</li>') + '</ul></div>'
            '<div><h4>Crossings</h4><ul>' + (''.join(mrows) or '<li>none</li>') + '</ul></div></div></article>')


def main():
    romfs, out = os.path.abspath(sys.argv[1]), sys.argv[2]
    cat = rl._catalog()
    inward = dungeon_gen.inward_only(romfs)
    sections = []
    nav = []
    total = multi = 0
    for lv in ORDER:
        rooms_ = sorted(r for r in cat if r.startswith(lv + '_'))
        if not rooms_:
            continue
        ports = rl.stairs_ports(romfs, lv, cat)
        spots = {}
        for a, p, b, q, alts in ports:
            spots.setdefault(a, set()).add((p[0], p[1], 'S'))
        links = []
        for a, p, b, q, alts in ports:
            links.append(f'<li>{a.rsplit("_", 1)[1]} → {b.rsplit("_", 1)[1]}: {html.escape(req_text(alts))}</li>')
        cards = [room_card(romfs, cat, r, sorted(spots.get(r, ())), inward) for r in rooms_]
        total += len(rooms_)
        multi += sum(1 for r in rooms_ if rl.room_areas(romfs, r)[1] > 1)
        key = lv.replace('Lv', 'd').lower()
        nav.append(f'<a href="#{key}">{NAMES.get(lv, lv)}</a>')
        sections.append(f'<section id="{key}"><h2>{NAMES.get(lv, lv)}</h2>'
                        f'<details class="links"><summary>Stairs ({len(links)} one-way hops)</summary><ul>'
                        + ''.join(sorted(set(links))) + '</ul></details>'
                        f'<div class="grid">{"".join(cards)}</div></section>')
    page = TEMPLATE.replace('{{NAV}}', ''.join(nav)).replace('{{SECTIONS}}', ''.join(sections)) \
        .replace('{{TOTAL}}', str(total)).replace('{{MULTI}}', str(multi))
    open(out, 'w').write(page)
    print(out, total, 'rooms,', multi, 'with more than one area')


TEMPLATE = '''<title>Dungeon Room Review</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Sans+Condensed:wght@400;600&family=IBM+Plex+Mono:wght@400;600&display=swap">
<style>
/* layout: a field manual - one dense card per room, map first, then what the logic reads from it */
:root {
  --bg: #f3f4f1; --paper: #ffffff; --ink: #1d2320; --muted: #5d6762; --line: #d5dad6; --accent: #2f6f5e;
  --wall: #8a918d; --pit: #2b2b2b; --lava: #d4572a; --water: #3f86c9; --door: #f1c232; --stair: #7c4dbd;
  --a0: #cfe6d8; --a1: #f3d9b1; --a2: #cbd8f0; --a3: #efc6d3; --a4: #e2e6b4; --a5: #c9e8ea; --a6: #e0cff0; --a7: #f0e0c8;
  --f-body: "IBM Plex Sans Condensed", "Arial Narrow", system-ui, sans-serif;
  --f-mono: "IBM Plex Mono", ui-monospace, Menlo, monospace;
}
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --bg: #151917; --paper: #1e2421; --ink: #e4e9e6; --muted: #9aa49f; --line: #333c37; --accent: #7cc4ad;
  --wall: #4e5551; --pit: #050505; --lava: #e0703f; --water: #4d93d6; --door: #e6b520; --stair: #a77ee6;
  --a0: #2f4a3c; --a1: #5a4527; --a2: #2f3f5e; --a3: #5a3343; --a4: #4c5127; --a5: #285257; --a6: #45345a; --a7: #5a4a33;
  color-scheme: dark } }
:root[data-theme="dark"] {
  --bg: #151917; --paper: #1e2421; --ink: #e4e9e6; --muted: #9aa49f; --line: #333c37; --accent: #7cc4ad;
  --wall: #4e5551; --pit: #050505; --lava: #e0703f; --water: #4d93d6; --door: #e6b520; --stair: #a77ee6;
  --a0: #2f4a3c; --a1: #5a4527; --a2: #2f3f5e; --a3: #5a3343; --a4: #4c5127; --a5: #285257; --a6: #45345a; --a7: #5a4a33;
  color-scheme: dark }
body { background: var(--bg); color: var(--ink); font-family: var(--f-body); font-size: 14px; line-height: 1.45;
  padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px));
  padding-block: 24px 64px; }
.wrap { max-width: 1400px; margin: 0 auto; display: grid; gap: 28px; }
h1 { font-size: 1.9rem; margin: 0; text-wrap: balance; }
.intro { max-width: 70ch; color: var(--muted); display: grid; gap: 8px; }
.intro p { margin: 0; }
nav { display: flex; flex-wrap: wrap; gap: 6px 14px; position: sticky; top: env(safe-area-inset-top, 0px);
  background: var(--bg); padding-block: 8px; z-index: 5; border-bottom: 1px solid var(--line); }
body::before { content: ""; position: fixed; inset: 0 0 auto; height: env(safe-area-inset-top, 0px); z-index: 10; background: var(--bg); }
nav a { color: var(--accent); text-decoration: none; font-weight: 600; }
nav a:focus-visible, summary:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
.legend { display: flex; flex-wrap: wrap; gap: 6px 16px; font-size: 0.85rem; color: var(--muted); }
.legend span { display: inline-flex; align-items: center; gap: 6px; }
.sw { width: 14px; height: 14px; border-radius: 2px; display: inline-block; }
section { display: grid; gap: 14px; }
h2 { font-size: 1.4rem; margin: 0; border-bottom: 2px solid var(--ink); padding-bottom: 4px; }
.links summary { cursor: pointer; color: var(--muted); }
.links ul { columns: 3 220px; font-family: var(--f-mono); font-size: 0.8rem; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 12px; }
.room { background: var(--paper); border: 1px solid var(--line); border-radius: 4px; padding: 10px; display: grid;
  gap: 8px; align-content: start; min-width: 0; }
.room.multi { border-color: var(--accent); }
.room header { display: flex; justify-content: space-between; align-items: baseline; gap: 8px; }
.room h3 { margin: 0; font-family: var(--f-mono); font-size: 1.05rem; }
.meta { color: var(--muted); font-size: 0.78rem; text-align: right; }
.room svg { width: 100%; height: auto; display: block; }
.cols { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
h4 { margin: 0 0 2px; font-size: 0.72rem; text-transform: uppercase; letter-spacing: 0.06em; color: var(--muted); }
.room ul { margin: 0; padding-left: 14px; font-size: 0.8rem; }
.to { color: var(--muted); }
.flags { list-style: none; padding: 6px 8px !important; background: color-mix(in srgb, var(--lava) 14%, var(--paper));
  border-radius: 3px; }
.t-wall { fill: var(--wall); } .t-pit { fill: var(--pit); } .t-lava { fill: var(--lava); } .t-water { fill: var(--water); }
.t-none { fill: var(--line); }
.a0 { fill: var(--a0); } .a1 { fill: var(--a1); } .a2 { fill: var(--a2); } .a3 { fill: var(--a3); }
.a4 { fill: var(--a4); } .a5 { fill: var(--a5); } .a6 { fill: var(--a6); } .a7 { fill: var(--a7); }
.door { fill: none; stroke: var(--door); stroke-width: 2.5; }
.stair { fill: var(--stair); }
.al { font: 600 10px var(--f-mono); fill: var(--ink); text-anchor: middle; opacity: 0.7; }
.ht { font: 6px var(--f-mono); fill: var(--ink); opacity: 0.6; }
.sl { font: 600 9px var(--f-mono); fill: #fff; text-anchor: middle; }
.lbl { font: 12px var(--f-body); fill: var(--muted); }
@media (max-width: 420px) { .cols { grid-template-columns: 1fr; } }
</style>
<div class="wrap">
<header class="intro">
<h1>Dungeon Room Review</h1>
<p>Every dungeon grid room as the room-shuffle logic reads it from the game's tile data: {{TOTAL}} rooms, {{MULTI}} of
them split into more than one area. Each letter is an area Link walks around in freely. Crossings say what gets him
from one area to another; anything not listed is treated as impossible. Doorways show which area they open into.</p>
<p>Check each map against the game. Room names are the vanilla cell (Face Shrine 04D is the room north of 05D's keyhole
block). Numbers in a tile are its floor height where it differs from 0.</p>
</header>
<div class="legend">
<span><i class="sw" style="background:var(--a0)"></i>area</span>
<span><i class="sw" style="background:var(--wall)"></i>wall</span>
<span><i class="sw" style="background:var(--pit)"></i>pit</span>
<span><i class="sw" style="background:var(--lava)"></i>lava</span>
<span><i class="sw" style="background:var(--water)"></i>deep water</span>
<span><i class="sw" style="border:2.5px solid var(--door)"></i>doorway</span>
<span><i class="sw" style="background:var(--stair);border-radius:50%"></i>stairs</span>
<span>Accent border: room with more than one area</span>
</div>
<nav>{{NAV}}</nav>
{{SECTIONS}}
</div>
'''

if __name__ == '__main__':
    main()
