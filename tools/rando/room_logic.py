#!/usr/bin/env python3
"""Item logic for shuffled dungeons (Dungeon room shuffle: Coupled / Decoupled, tools/rando/dungeon_gen.py).

LAS's dungeon logic describes the vanilla layouts: a few "room" regions per dungeon named after cells (D1-8D,
D1-6F...) and each check gated by one of them plus key counts and what the check itself needs. With rooms moved
that no longer holds, so for every shuffled dungeon this module builds the logic from the generated layout:

  graph      nodes = the areas of each cell's room (room_areas: floor Link walks around freely; crossings between
             areas: jumps over pits / lava, ledge drops, swimming, the hookshot); edges = doorways that connect
             (neighbours whose doorways line up, door jumps - directed), from the area a doorway opens into, plus
             the stairs (side-view passages - by where their rooms sit - needing the feather; stairs rooms are
             anchors and never move); drop-down-only doorways (a ledge Link can't climb from his
             room: dungeon_gen.inward_only) lead in only
  doors      only a room's OWN door actors count (a "(mirror)" door stood in the old neighbour, which isn't there):
             lock -> the dungeon's small keys, boss door -> its Nightmare Key, cracked wall -> bombs, shutter ->
             something to fight with / hit switches (approximate), pot door -> something thrown at it from the room
             Link stands in (throw_req: a pot with the bracelet, an elephant with bracelet L2), water in the
             doorway -> flippers, crystal -> break-crystal; one-way doors pass one way (actor halves 0x1A3 / 0x1A4); statues
             and chests in a doorway are left out (impassable for the logic: safe, never relied on)
  keys       a lock needs ALL of the dungeon's small keys (always solvable: the keys sit before every lock); the
             number of small keys per dungeon = the number of locks in its new layout
  cells      each area's requirement = OR over paths from the entrance of AND of the door requirements, computed
             as a fixed point in minimal sum-of-products form (LAS evaluates conditions recursively, so cyclic
             region references aren't possible - the finished, loop-free condition goes into the logic); a cell's
             = every area of it that can be reached (cell_terms: where in its room a check stands isn't known)
  regions    LAS's cell regions (Dn-XY) are redefined as "reach cell XY of the new layout" (macros elsewhere use them)
  checks     reach the cell holding the check's room AND what
             the check needs inside its room: its vanilla condition with the region / key tokens taken out (where
             such a token sat inside an OR, the OR becomes an AND - stricter, never weaker). A check whose room isn't
             used anywhere is excluded (junk). Dungeon items go to the dungeon a check physically is in now.

  shutters   by what opens each one (shutter_req): the enemies its kill-all trigger watches (or the mini-boss / boss
             that drives it; a shutter nothing drives: every enemy in the room) -> their LAS kill rules (kill-spiny,
             kill-pols-voice...; the pit variant in rooms with pits). Those enemies always stay vanilla (LAS enemies.py
             kill_linked). A switch / block / floor tile -> something to hit switches with; anything else -> something
             to fight with.

Not modelled yet (stricter or approximate): blocks, statues and switches inside a room (collision at floor height
is walked through: LAS's condition for the room covers them), where in its room a check stands, key counts along
paths. tools/rando/room_review.py draws every room as read here, for checking against the game.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rooms  # noqa: E402  (cell positions: Eagle's Tower's grid doesn't follow its names)
import room_overrides  # noqa: E402

LEVELS = {
    'Lv10ClothesDungeon': (0, 'color-dungeon'), 'Lv01TailCave': (1, 'tail-cave'), 'Lv02BottleGrotto': (2, 'bottle-grotto'),
    'Lv03KeyCavern': (3, 'key-cavern'), 'Lv04AnglersTunnel': (4, 'angler-tunnel'), 'Lv05CatfishsMaw': (5, 'catfish-maw'),
    'Lv06FaceShrine': (6, 'face-shrine'), 'Lv07EagleTower': (7, 'eagle-tower'), 'Lv08TurtleRock': (8, 'turtle-rock'),
}
SHUTTER = 'sword | shoot-arrows | bombs | rod | boomerang'  # a shutter whose enemies LAS has no kill rule for (the
SWITCH_SHUTTER = 'sword | shoot-arrows | bombs | rod | boomerang | hookshot'  # hookshot only stuns) / a switch's shutter
DOOR_REQ = {'cracked': 'bombs', 'blocked:water': 'flippers', 'blocked:crystal': 'break-crystal',
            'blocked:block': None, 'shutter': 'rs-shutter'}  # (shutters: shutter_req, this only without the room)
IMPASSABLE = {'blocked:statue', 'blocked:chest'}
STEP = {'N': (0, -1), 'S': (0, 1), 'W': (-1, 0), 'E': (1, 0)}
OPP = {'N': 'S', 'S': 'N', 'W': 'E', 'E': 'W'}
MAX_TERMS = 48
EXIT_REQ = {}  # catalog room -> frozenset of atoms leaving it needs (mini-boss souls: tools/rando/souls.py exit_req)


# checks on a ledge that only their room's vanilla neighbour reaches (dungeon_gen.TIED / STAY keep the two together):
# the check also needs that neighbour's cell - the way up is there (Face Shrine 08G's ramp, Key Cavern 03D's, Turtle
# Rock 03E's), not in the check's own room
LEDGE_VIA = {'D6-wizzrobes-ledge': 'Lv06FaceShrine_08G', 'D3-east-ledge': 'Lv03KeyCavern_03D',
             'D8-northern-ledge': 'Lv08TurtleRock_03E'}


# location subtypes LAS shuffles (shuffler.py: every other item location keeps its vanilla content)
RANDOMIZED_SUBTYPES = ('chest', 'boss', 'enemy', 'drop', 'npc', 'standing', 'overworld-statue', 'dungeon-statue')


def cell_pos(cell, level=None):  # '06C' -> (col 2, row 6) (Eagle's Tower: its own grid, rooms.POSITIONS)
    p = rooms.POSITIONS.get(level, {}).get(cell[:3])
    return p if p else (ord(cell[2]) - ord('A'), int(cell[:2]))


def cell_name(col, row, level=None):
    return rooms.cell_name(level, col, row).rsplit('_', 1)[1] if level else f'{row:02d}{chr(ord("A") + col)}'


# ---------------------------------------------------------------- conditions: parse, strip, print
TOK = re.compile(r'\s*(\(|\)|&|\||!|[^\s()&|!]+)')


def top_level_or(s):
    """'a | (b & c) | d' -> ['a', '(b & c)', 'd'] (alternatives outside parentheses)."""
    out, depth, cur = [], 0, ''
    for ch in s:
        depth += ch == '('
        depth -= ch == ')'
        if ch == '|' and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    return out + [cur] if cur.strip() else out


def parse(s):
    toks = [t for t in TOK.findall(s or '') if t.strip()]
    pos = [0]

    def peek():
        return toks[pos[0]] if pos[0] < len(toks) else None

    def take():
        pos[0] += 1
        return toks[pos[0] - 1]

    def expr():
        n = [term()]
        while peek() == '|':
            take()
            n.append(term())
        return n[0] if len(n) == 1 else ('|', n)

    def term():
        n = [factor()]
        while peek() == '&':
            take()
            n.append(factor())
        return n[0] if len(n) == 1 else ('&', n)

    def factor():
        t = take()
        if t == '!':
            return ('!', [factor()])
        if t == '(':
            e = expr()
            take()  # ')'
            return e
        return ('atom', t)
    if not toks:
        return None
    return expr()


def strip(node, drop):
    """Removes atoms for which drop(atom) is true (treated as satisfied). Returns (node or None, stripped?). An OR
    whose branches lost something becomes an AND of its branches (strict)."""
    if node is None:
        return None, False
    kind = node[0]
    if kind == 'atom':
        return (None, True) if drop(node[1]) else (node, False)
    if kind == '!':
        sub, s = strip(node[1][0], drop)
        return (('!', [sub]) if sub else None), s
    parts, any_s = [], False
    for c in node[1]:
        sub, s = strip(c, drop)
        any_s |= s
        if sub is not None:
            parts.append(sub)
    if kind == '|' and any_s:
        kind = '&'
    if not parts:
        return None, any_s
    return (parts[0] if len(parts) == 1 else (kind, parts)), any_s


def falsify(node, bad):
    """The condition with the atoms for which bad(atom) is true taken as never satisfied (None = never)."""
    if node is None:
        return None
    kind = node[0]
    if kind == 'atom':
        return None if bad(node[1]) else node
    if kind == '!':
        return node
    parts = [falsify(c, bad) for c in node[1]]
    if kind == '&':
        return None if any(p is None for p in parts) else ('&', parts)
    parts = [p for p in parts if p is not None]
    return None if not parts else parts[0] if len(parts) == 1 else ('|', parts)


def show(node):
    if node is None:
        return ''
    kind = node[0]
    if kind == 'atom':
        return node[1]
    if kind == '!':
        return '!' + paren(node[1][0])
    return f' {kind} '.join(paren(c) if c[0] in ('|', '&') and c[0] != kind else show(c) for c in node[1])


def paren(n):
    return show(n) if n[0] in ('atom', '!') else '(' + show(n) + ')'


def dnf_str(terms):
    if not terms:
        return None
    out = []
    for t in sorted(terms, key=lambda t: (len(t), sorted(t))):
        out.append(' & '.join(f'({a})' if '|' in a else a for a in sorted(t)) if t else '')
    out = [o for o in out if o]
    return ' | '.join(f'({o})' for o in out) if len(out) > 1 else (out[0] if out else None)


# ---------------------------------------------------------------- the dungeon graph
def own_doors(cat, room, side, slot):
    """Types of the room's own door actors at that doorway (no '(mirror)')."""
    out = []
    if isinstance(slot, str):  # wide opening ('w0-9'): no door actors
        return out
    for d in cat.get(room, {}).get('doors', []):
        if len(d) > 3 and d[0] == side and abs(d[3] - slot) <= 1 and not d[2].endswith('(mirror)'):
            out.append(d[2])
    return out


ONEWAY_OUT, ONEWAY_IN = 0x1A3, 0x1A4  # ObjOnewayDoor halves: the room Link leaves through it / the room he enters
                                     # (Bottle Grotto 05B 0x1A3 -> Hinox room 04B 0x1A4, as LAS's logic goes)


_lock_cache = {}


def lock_flag(romfs, room, side):
    """The 'opened' flag of the room's own ObjLockDoor (0xED) on that side, or None. A vanilla locked door is two
    actors - one in each room - sharing this flag, so one key opens both halves: locks are counted by flag."""
    if room not in _lock_cache:
        import sys
        sys.path.insert(0, os.path.join(HERE, '..', '..', 'third_party', 'LAS-Randomizer'))
        from RandomizerCore.Tools import leb
        import rooms as _rooms
        out = []
        try:
            r = leb.Room(open(_rooms._leb_path(romfs, room), 'rb').read())
            x0, z0 = _rooms.cell_origin(room)
            for a in r.actors:
                if a.type != 0xED:
                    continue
                lx, lz = a.posX - x0, a.posZ - z0
                d = {'W': lx, 'E': 15.0 - lx, 'N': lz, 'S': 12.0 - lz}
                sd = min(d, key=d.get)
                flag = next((i for t, i in a.switches if t == 1), None)
                out.append((sd, flag))
        except (OSError, ValueError):
            pass
        _lock_cache[room] = out
    for sd, flag in _lock_cache[room]:
        if sd == side:
            return flag
    return None


_block_cache = {}


def lock_blocks(romfs, room):
    """The 'opened' flags of the room's keyhole blocks (ObjLockBlock 0x102): each takes a small key. What a block
    guards inside its room isn't known, so the logic asks for it for the room's checks and for leaving the room
    through its other doorways (entering stays free)."""
    if room not in _block_cache:
        import sys
        sys.path.insert(0, os.path.join(HERE, '..', '..', 'third_party', 'LAS-Randomizer'))
        from RandomizerCore.Tools import leb
        import rooms as _rooms
        out = []
        try:
            r = leb.Room(open(_rooms._leb_path(romfs, room), 'rb').read())
            for a in r.actors:
                if a.type == 0x102:
                    out.append(next((i for t, i in a.switches if t == 1), None) or f'{room}-{len(out)}')
        except (OSError, ValueError):
            pass
        _block_cache[room] = out
    return _block_cache[room]


_throw_cache = {}


def throw_req(romfs, room):
    """What breaking a pot door from this room takes: something thrown at it - bombs and swords don't break it. A pot
    (ObjDungeonPot) with the bracelet, else an elephant statue (ObjElephantS) with the level 2 bracelet (Face Shrine
    04D: only the elephant); None: nothing to throw."""
    if room not in _throw_cache:
        from RandomizerCore.Tools import leb
        import rooms as _rooms
        names = set()
        try:
            names = {a.name.decode(errors='ignore').split('-')[0]
                     for a in leb.Room(open(_rooms._leb_path(romfs, room), 'rb').read()).actors}
        except (OSError, ValueError):
            pass
        _throw_cache[room] = frozenset({'bracelet'}) if 'ObjDungeonPot' in names else \
            frozenset({'bracelet:2'}) if 'ObjElephantS' in names else None
    return _throw_cache[room]


# enemy actor type -> the LAS kill rules (logic.yml) a shutter waiting for it needs. Types not listed: SHUTTER.
KILL = {0x1b: ('kill-zol-green',), 0x1c: ('kill-zol-red',), 0x1d: ('kill-zol-green',),  # (0x1d: Gels, as Zols)
        0x24: ('kill-hardhat-beetle',), 0x25: ('kill-rope',), 0x26: ('kill-vire',), 0x27: ('kill-minimoldorm',),
        0x28: ('kill-spiny',), 0x2e: ('kill-stalfos-spear',), 0x33: ('kill-gibdo',), 0x34: ('kill-keese',),
        0x31: ('kill-bombite-red', 'kill-bombite-green'), 0x32: ('kill-bombite-red', 'kill-bombite-green'),  # (LAS's
        0x37: ('kill-pols-voice',), 0x38: ('kill-like-like',), 0x39: ('kill-stalfos',),  # D3-triple-bombites: both)
        0x3a: ('kill-stalfos',), 0x3b: ('kill-wizzrobe',), 0x40: ('kill-helmasaur',), 0x41: ('kill-shy-guy',),
        0x42: ('kill-pairodd',), 0x43: ('kill-water-tektite',), 0x44: ('kill-star',)}
NOT_COUNTED = (0x3d, 0xb7)  # Sparks, Bubbles: no kill-all check waits for them
PIT_KILLS = ('kill-hardhat-beetle', 'kill-spiny', 'kill-stalfos', 'kill-wizzrobe', 'kill-zol-red')  # (condition-pit)
KILL_TRIGGERS = (0xF0, 0x1B9, 0x247)  # TagHolocaust, TagPopChecker, TagHistoricalHolocaustChecker: watched enemies dead


def _kills():
    import souls  # (bosses / mini-bosses that drive their arena's shutters)
    out = dict(KILL)
    for _, _, t, kill, _ in souls.BOSSES:
        out[t] = (kill,)
    for _, t, kill, _ in souls.MINIBOSSES:
        if kill:
            out[t] = (kill,)
    return out


_shutter_cache = {}
_level_cache = {}


def _level_rooms(romfs, level):
    from RandomizerCore.Tools import leb
    if level not in _level_cache:
        d = os.path.join(romfs, 'region_common', 'level', level)
        _level_cache[level] = {f[:-4]: leb.Room(open(os.path.join(d, f), 'rb').read())
                               for f in sorted(os.listdir(d)) if f.endswith('.leb')}
    return _level_cache[level]


def has_pits(r):
    """A room file with pits (grid tiles: byte 0 bit 0, or sunk below -0.3, that aren't water / lava)."""
    import struct
    import rooms as _rooms
    g = _rooms._grid(r)
    raw = g[b'data'].data if g and b'data' in g else b''
    return any((raw[o] & 1 or struct.unpack_from('<f', raw, o + 12)[0] < -0.3) and not raw[o + 2] & 4
               for o in range(0, len(raw) - 15, 16))


def shutter_req(romfs, room, side):
    """frozenset of atoms the room's own shutter(s) on that side need to open: what opens it (see the docstring)."""
    key = (room, side)
    if key in _shutter_cache:
        return _shutter_cache[key]
    sys.path.insert(0, os.path.join(HERE, '..', '..', 'third_party', 'LAS-Randomizer'))
    import rooms as _rooms
    level = room.rsplit('_', 1)[0]
    try:
        lvl = _level_rooms(romfs, level)
        r = lvl[room]
    except (OSError, KeyError):
        _shutter_cache[key] = frozenset({'rs-shutter'})
        return _shutter_cache[key]
    kills, pits = _kills(), has_pits(r)
    A = r.actors

    def atoms(acts):  # the kill rules for these enemies (rs-shutter for one LAS has no rule for)
        out = set()
        for a in acts:
            if a.type not in NOT_COUNTED:
                out.update(f'{k}[pit]' if pits and k in PIT_KILLS else k for k in kills.get(a.type, ('rs-shutter',)))
        return out

    def watched(t, acts):
        return [acts[s[1]] for s in t.relationships.section_1 if 0 <= s[1] < len(acts)]

    x0, z0 = _rooms.cell_origin(room)
    req = set()
    for i, a in enumerate(A):
        if a.type != 0xEF:
            continue
        lx, lz = a.posX - x0, a.posZ - z0
        d = {'W': lx, 'E': 15.0 - lx, 'N': lz, 'S': 12.0 - lz}
        if min(d, key=d.get) != side:
            continue
        flag = next((f for t, f in a.switches if t == 1), None)
        drivers = [b for j, b in enumerate(A) if j != i and b.type != 0xEF and
                   i in [s[1] for s in b.relationships.section_1]]
        got = None
        if drivers:  # a mini-boss / boss (its relationships open the doors)
            got = atoms(drivers)
        elif flag is None:  # nothing drives it: the room's own all-enemies-dead check (LAS keeps them all vanilla)
            enemies = [b for b in A if b.name.startswith(b'Enemy')]
            got = atoms(enemies) if enemies else None
        else:  # opened by its flag: a kill-all trigger here (or in another room of the level), or a switch
            setters = [b for b in A if b.type != 0xEF and (1, flag) in [tuple(s) for s in b.switches]]
            trig = [b for b in setters if b.type in KILL_TRIGGERS]
            if not trig and not setters:
                for other, ro in lvl.items():
                    if other != room:
                        trig += [b for b in ro.actors if b.type in KILL_TRIGGERS and
                                 (1, flag) in [tuple(s) for s in b.switches]]
                        if trig:
                            A2 = ro.actors
                            break
            else:
                A2 = A
            if trig:
                got = set().union(*(atoms(watched(t, A2)) for t in trig))
            elif setters:  # a floor switch, block, tile, horse head...
                got = {'rs-switch-shutter'}
        req |= got if got else {'rs-shutter'}
    _shutter_cache[key] = frozenset(req or {'rs-shutter'})
    return _shutter_cache[key]


def door_terms(types, n, entering=False, room_types=(), lock_id='', shutter=None, throw=frozenset({'bracelet:2'})):
    """frozenset of requirement atoms for passing these door types, or None if impassable. entering: going INTO the
    room that owns the doors (else leaving it). A one-way door passes one way: out of the room with the 0x1A3 half,
    into the room with the 0x1A4 half. shutter: what the owner's shutter on that side needs (shutter_req). throw: what
    throwing something at a pot door takes from the room Link stands in (throw_req; None: nothing to throw)."""
    req = set()
    for t in types:
        if t == 'shutter' and shutter:
            req |= shutter
            continue
        if t == 'potdoor':
            if throw is None:
                return None
            req |= throw
            continue
        if t == 'oneway':
            if (ONEWAY_IN if entering else ONEWAY_OUT) in room_types:
                continue
            return None
        if t in IMPASSABLE:
            return None
        if t == 'lock':
            req.add(f'LOCK|{n}|{lock_id}')  # (-> key-Dn:k, by the worst-case key rule)
        elif t == 'bossdoor':
            req.add(f'nightmare-key-D{n}')
        elif DOOR_REQ.get(t):
            req.add(DOOR_REQ[t])
    return frozenset(req)


def _side_rect(level_dir, room):
    """(x0, z0, x1, z1) a room's tile grid covers (its 'info': rows, columns, origin; 1.5 per tile), or None."""
    import struct
    from RandomizerCore.Tools import leb
    import rooms as _rooms
    try:
        g = _rooms._grid(leb.Room(open(os.path.join(level_dir, room + '.leb'), 'rb').read()))
        h, w = struct.unpack_from('<HH', g[b'info'].data, 0)
        ox, oz = struct.unpack_from('<2f', g[b'info'].data, 8)
    except (OSError, TypeError, KeyError, struct.error):
        return None
    return ox, oz, ox + 1.5 * w, oz + 1.5 * h


def _touch(a, b, eps=0.01):
    """Two grid rectangles share an edge."""
    side_by_side = (abs(a[2] - b[0]) < eps or abs(b[2] - a[0]) < eps) and min(a[3], b[3]) - max(a[1], b[1]) > eps
    stacked = (abs(a[3] - b[1]) < eps or abs(b[3] - a[1]) < eps) and min(a[2], b[2]) - max(a[0], b[0]) > eps
    return side_by_side or stacked


_stairs_cache = {}


def stairs_links(romfs, level, cat=None):
    """[(grid room, grid room)] joined through side-view passages under the stairs (side-view room = not a grid room
    of the catalog)."""
    if (romfs, level, 'links') in _stairs_cache:
        return _stairs_cache[(romfs, level, 'links')]
    import sys
    sys.path.insert(0, os.path.join(HERE, '..', '..', 'third_party', 'LAS-Randomizer'))
    from RandomizerCore.Tools import leb
    d = os.path.join(romfs, 'region_common', 'level', level)
    down, up = {}, {}  # grid -> side-view room; side-view -> grid
    for f in sorted(os.listdir(d)):
        if not f.endswith('.leb'):
            continue
        room = f[:-4]
        r = leb.Room(open(os.path.join(d, f), 'rb').read())
        for a in r.actors:
            n = a.name.decode(errors='ignore')
            if not a.parameters or not isinstance(a.parameters[0], bytes):
                continue
            target = a.parameters[0].decode(errors='ignore')
            if n.startswith('AreaDownstairs'):
                down.setdefault(room, set()).add(target)
            elif n.startswith('AreaUpstairs'):
                up.setdefault(room, set()).add(target)
    if cat is None:
        cat = json.load(open(os.path.join(HERE, 'room_catalog.json')))
    side = {r for r in set(up) | set(down) | {t for v in list(down.values()) + list(up.values()) for t in v} if r not in cat}
    parent = {s: s for s in side}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    rect = {s: _side_rect(d, s) for s in side}
    for s in side:  # side-view rooms that touch (a row, or Bottle Grotto's 3-row area 04D-06E): one passage. By their
        for t in side:  # grids' places, not their names: Face Shrine's 01A-01F are three passages of two rooms each
            if s < t and rect[s] and rect[t] and _touch(rect[s], rect[t]):  # (seed RapidBowWow958: as one, every
                parent[find(s)] = find(t)                                    # staircase led to every other)
    comp = {}
    for a, ts in list(down.items()) + list(up.items()):  # stairs either way between a grid room and a passage
        for t in ts:
            if a in parent and t not in parent:
                comp.setdefault(find(a), set()).add(t)
            elif t in parent and a not in parent:
                comp.setdefault(find(t), set()).add(a)
    links = []
    for a, ts in list(down.items()) + list(up.items()):  # stairs straight from one grid room to another (Key Cavern's
        for t in ts:                                      # floors: 04B <-> 04G, 04C <-> 04H)
            if a in cat and t in cat and a != t:
                links.append(tuple(sorted((a, t))))
    for gs in comp.values():
        gs = sorted(g for g in gs if g not in side)  # (grid rooms only)
        for i in range(len(gs)):
            for j in range(i + 1, len(gs)):
                links.append((gs[i], gs[j]))
    _stairs_cache[(romfs, level, 'links')] = sorted(set(links))
    return _stairs_cache[(romfs, level, 'links')]


def hole_links(romfs, level, cat, spots=False):
    """[(grid room, grid room it drops into)]: holes (AreaHoleLower) name a player start ('<room>' or '<room>_a'). Eagle's
    Tower starts collapsed in room shuffle seeds (eagle_tower.py): 2F 04C's stairs lead to 04G (stairs_links finds it from
    04G's side)."""
    import sys
    sys.path.insert(0, os.path.join(HERE, '..', '..', 'third_party', 'LAS-Randomizer'))
    from RandomizerCore.Tools import leb
    d = os.path.join(romfs, 'region_common', 'level', level)
    out = []
    for f in sorted(os.listdir(d)):
        room = f[:-4]
        if not f.endswith('.leb') or room not in cat:
            continue
        for a in leb.Room(open(os.path.join(d, f), 'rb').read()).actors:
            ps = [p for p in a.parameters or () if isinstance(p, bytes) and p]  # (the first one is empty)
            if a.name.decode(errors='ignore').startswith('AreaHoleLow') and ps:
                t = ps[0].decode(errors='ignore')
                t = t if t in cat else t.rsplit('_', 1)[0] if t.rsplit('_', 1)[0] in cat else None
                if t and t != room:
                    out.append((room, (a.posX, a.posZ), t) if spots else (room, t))
    if not spots:
        return sorted(set(out))
    starts = {}  # (spots: the hole's spot, and the landing room's player starts)
    for _, _, t in out:
        if t not in starts:
            starts[t] = [(a.posX, a.posZ) for a in leb.Room(open(os.path.join(d, t + '.leb'), 'rb').read()).actors
                         if a.name.decode(errors='ignore').startswith('TagPlayerStart')]
    return [(r, p, t, starts[t]) for r, p, t in out]


def stairs_ports(romfs, level, cat):
    """[(room, (x, z), room, (x, z), [frozenset alternatives])]: stairs as one-way hops from a stairs actor's spot in one
    grid room to where Link arrives in another, both ways. Side-view passages need PASSAGE (Link jumps across gaps
    down there); stairs straight between grid rooms go from / to each room's own stairs, or its player starts where
    only the other room lists them (Eagle's Tower 04C <-> 04G, Face Shrine 04F <-> 06F)."""
    if (romfs, level, 'ports') in _stairs_cache:
        return _stairs_cache[(romfs, level, 'ports')]
    from RandomizerCore.Tools import leb
    d = os.path.join(romfs, 'region_common', 'level', level)
    acts = {}  # grid room -> [(kind, target, (x, z))]
    starts = {}
    for room in cat:
        if not room.startswith(level + '_'):
            continue
        try:
            r = leb.Room(open(os.path.join(d, room + '.leb'), 'rb').read())
        except OSError:
            continue
        for a in r.actors:
            nm = a.name.decode(errors='ignore')
            if nm.startswith('TagPlayerStart'):
                starts.setdefault(room, []).append((a.posX, a.posZ))
            if nm.startswith(('AreaDownstairs', 'AreaUpstairs')) and a.parameters and isinstance(a.parameters[0], bytes):
                acts.setdefault(room, []).append((a.parameters[0].decode(errors='ignore'), (a.posX, a.posZ)))
    out = []
    links = stairs_links(romfs, level, cat)
    for a, b in links:
        direct_ab = [p for t, p in acts.get(a, ()) if t == b]
        direct_ba = [p for t, p in acts.get(b, ()) if t == a]
        if direct_ab or direct_ba:  # (straight between grid rooms)
            req = [frozenset({'break-crystal'})] if {a, b} == {'Lv07EagleTower_04C', 'Lv07EagleTower_04G'} else \
                [frozenset()]
            for src, dst, here, there in ((a, b, direct_ab, direct_ba), (b, a, direct_ba, direct_ab)):
                for p in here or starts.get(src, ()):
                    for q in there or starts.get(dst, ()):
                        out.append((src, p, dst, q, req))
            continue
        pa = [p for t, p in acts.get(a, ()) if t not in cat]  # (its stairs into the passage)
        pb = [p for t, p in acts.get(b, ()) if t not in cat]
        req = room_overrides.alts(room_overrides.PASSAGES[frozenset({a, b})]) \
            if frozenset({a, b}) in room_overrides.PASSAGES else PASSAGE
        for p in pa:
            for q in pb:
                out.append((a, p, b, q, req))
                out.append((b, q, a, p, req))
    _stairs_cache[(romfs, level, 'ports')] = out
    return out


# ---------------------------------------------------------------- inside a room: the areas Link walks between
# A grid room (8 x 10 tiles, rooms.py) is split into areas: floor Link walks across freely (steps up to STEP_UP:
# ramps, Angler's Tunnel 07D's 0.9; doorway tiles under a door frame read as wall height and join anything). Between areas: a jump over 1 tile of pit / lava (Roc's Feather, or the Rooster where companions may enter
# dungeons), 2 tiles (with the Pegasus Boots), the hookshot over pits / water to the first solid thing within HOOK_REACH
# (Catfish's Maw 03E's pit before the Nightmare door: the block in the next doorway), a drop off a ledge (one tile of wall, then floor LEDGE lower - one way;
# Face Shrine 04D's raised strips), swimming (deep water: flippers). Nothing else crosses (the hookshot only where LAS
# says so - not modelled, stricter). Seed RapidBowWow958: Turtle Rock 07D's lava strip cut its north doorway off, and
# the logic walked through.
STEP_UP = 1.0
LEDGE = 1.0
# (one macro atom each, defined in apply() like rs-shutter: alternatives as separate terms multiplied past MAX_TERMS)
JUMP = 'feather | (rooster & bad-pets)'
LONG_JUMP = '(feather & boots) | (rooster & bad-pets)'
FEATHER = [frozenset({'rs-jump'})]
JUMPS = {1: FEATHER, 2: [frozenset({'rs-long-jump'})]}
HOOK_REACH = 8  # tiles (hookshot_logic.py: 12 units) - pulled across pits / water to a target behind the landing
# what the hookshot latches onto in standard play: objects, never a plain wall (runtime/hle/hookshot.c: a surface
# without the target bit clinks off). Chests, blocks, statues - not pots (Bottle Grotto 02D, Angler's Tunnel 06F: the
# shot breaks them). With "hookshot anything" (HOOK_ANYTHING, set by las_headless from the seed's settings) any solid
# tile.
HOOK_TARGETS = ('ObjTreasureBox', 'ObjSquareBlock', 'ObjDungeonSt', 'ObjDungeonOw', 'ObjHookBridge')
HOOK_ANYTHING = False
PASSAGE = [frozenset({'feather'})]  # side-view passages under the stairs (Face Shrine 05D <-> 04F: a gap only the
# feather crosses - the Rooster doesn't come into side-view rooms)
FLOOR, SOLID, PIT, WATER = 'floor', 'solid', 'pit', 'water'
_tiles_cache = {}
_hook_tiles = {}  # room -> tiles with collision (room_tiles)
ledge_drops = {}  # room -> {(ledge tile, (dr, dc))}: where Link jumps down (room_areas; the review page draws them)
hook_targets = {}  # room -> tiles of the objects the hookshot latches onto (room_areas; the review page draws them)
_areas_cache = {}


def room_tiles(romfs, room):
    """{(row, col): (kind, height)} of a grid room, or None. Tile bytes (rooms.py): byte 0 bit 1 collision, bit 0
    a hole; byte 2 bit 2 water / lava, bit 3 floor (doorway tiles carry it under a door's collision). Deep water:
    the water bit with the hole bit or sunk below -0.3 (Angler's Tunnel -0.1 with the hole bit, Face Shrine's
    channels -1.5); in Turtle Rock that's lava (-0.4) - a gap like a pit."""
    if room in _tiles_cache:
        return _tiles_cache[room]
    import struct
    from RandomizerCore.Tools import leb
    import rooms as _rooms
    out = None
    hook = _hook_tiles.setdefault(room, set())
    try:
        g = _rooms._grid(leb.Room(open(_rooms._leb_path(romfs, room), 'rb').read()))
        h, w = struct.unpack_from('<HH', g[b'info'].data, 0)
        raw = g[b'data'].data
        if (h, w) == (8, 10):
            out = {}
            for row in range(8):
                for col in range(10):
                    o = 0x10 * (row * 10 + col)
                    b0, b2 = raw[o], raw[o + 2]
                    ht = struct.unpack_from('<f', raw, o + 12)[0]
                    if b2 & 4 and (b0 & 1 or ht < -0.3):
                        kind = PIT if room.startswith('Lv08TurtleRock') else WATER
                    elif b0 & 2 and not b2 & 8:
                        kind = SOLID
                    elif b0 & 2 and b2 & 8:  # (a doorway tile under the frame's collision: no height of its own)
                        kind, ht = FLOOR, None
                    elif (b0 & 1 and not b2 & 0xC) or (ht < -1.0 and not b2 & 4):
                        kind = PIT
                    else:
                        kind = FLOOR
                    out[(row, col)] = (kind, ht)
                    if b0 & 2 and not b2 & 8:
                        hook.add((row, col))  # (anything solid: the hookshot catches it)
            # collision no higher than the floor beside it: blocks, chests, statues, pillars (Turtle Rock 02H's block
            # between its two 1.5 floors) - left walkable: what moves them isn't known, LAS's logic for the room
            # covers them. A wall stands above the floor next to it.
            for t, (kind, ht) in list(out.items()):
                if kind != SOLID:
                    continue
                beside = [out[u][1] for u in ((t[0] + 1, t[1]), (t[0] - 1, t[1]), (t[0], t[1] + 1), (t[0], t[1] - 1))
                          if u in out and out[u][0] == FLOOR and out[u][1] is not None]
                if beside and ht <= max(beside) + 0.3:
                    out[t] = (FLOOR, ht)
            fix = room_overrides.ROOMS.get(room, {})  # (what the tiles can't tell: room_overrides.py)
            for t in fix.get('solid', ()):
                out[t] = (SOLID, 3.0)
            for t in fix.get('floor', ()):
                out[t] = (FLOOR, 0.0)
    except (OSError, TypeError, KeyError, struct.error):
        out = None
    _tiles_cache[room] = out
    return out


_cat_cache = []


def _catalog():
    if not _cat_cache:
        _cat_cache.append(json.load(open(os.path.join(HERE, 'room_catalog.json'))))
    return _cat_cache[0]


def _height_gap(a, b):
    return 0.0 if a is None or b is None else abs(a - b)


def room_areas(romfs, room):
    """({(row, col): area}, area count, [(area, area, [frozenset alternatives])]) - see above. A room without a tile
    grid: one area, every tile in it."""
    if room in _areas_cache:
        return _areas_cache[room]
    g = room_tiles(romfs, room)
    if g is None:
        _areas_cache[room] = ({(r, c): 0 for r in range(8) for c in range(10)}, 1, [])
        return _areas_cache[room]
    solid = set(_hook_tiles.get(room, ()))  # every solid tile: what the shot stops at
    hook = set()  # the ones it latches onto
    import rooms as _rooms
    from RandomizerCore.Tools import leb
    try:
        ox, oz = _rooms.cell_origin(room)
        acts = leb.Room(open(_rooms._leb_path(romfs, room), 'rb').read()).actors
    except (OSError, ValueError, KeyError):
        acts = []
    for x in acts:
        if x.name.decode(errors='ignore').startswith(HOOK_TARGETS):
            t = (int((x.posZ - oz) // 1.5), int((x.posX - ox) // 1.5))
            hook.add(t)
            solid.add(t)
    hook_targets[room] = set(hook)
    if HOOK_ANYTHING:
        hook |= solid
    cat = _catalog()
    blockers = [(d[0], d[3]) for d in cat.get(room, {}).get('doors', [])
                if len(d) > 3 and d[2].startswith('blocked:') and not d[2].endswith('(mirror)')]
    c0, r0 = cell_pos(room.rsplit('_', 1)[1])
    for u in cat.get(room, {}).get('unit', []):  # (and in the doorway of the next room of its unit, which moves with it:
        cu, ru = cell_pos(u.rsplit('_', 1)[1])  # Catfish's Maw 03E's pit - the block in 03D's east doorway)
        side = next((sd for sd, (dc, dr) in STEP.items() if (c0 + dc, r0 + dr) == (cu, ru)), None)
        if side and u != room:
            blockers += [(side, d[3]) for d in cat[u].get('doors', []) if len(d) > 3 and d[0] == OPP[side]
                         and d[2].startswith('blocked:') and not d[2].endswith('(mirror)')]
    for side, slot in blockers:  # a block / statue standing in a doorway: just off the grid, the hookshot catches it
        for t in _doorway_tiles(side, slot)[0]:
            hook.add((t[0] + STEP[side][1], t[1] + STEP[side][0]))
            solid.add((t[0] + STEP[side][1], t[1] + STEP[side][0]))
    area = {}
    n = 0
    for t, (kind, _) in sorted(g.items()):
        if kind not in (FLOOR, WATER) or t in area:
            continue
        area[t] = n
        todo = [t]
        while todo:
            r, c = todo.pop()
            for dr, dc in ((0, 1), (1, 0), (0, -1), (-1, 0)):
                u = (r + dr, c + dc)
                if u in area or u not in g or g[u][0] != kind:
                    continue
                if kind == FLOOR and _height_gap(g[u][1], g[(r, c)][1]) > STEP_UP:
                    continue
                area[u] = n
                todo.append(u)
        n += 1
    moves = {}

    def add(a, b, alts):
        if a != b:
            moves.setdefault((a, b), set()).update(alts)
    for (r, c), (kind, ht) in g.items():
        if (r, c) not in area:
            continue
        a = area[(r, c)]
        for dr, dc in ((0, 1), (1, 0), (0, -1), (-1, 0)):
            u = (r + dr, c + dc)
            if u not in g:
                continue
            k2, h2 = g[u]
            if u in area and area[u] != a:
                if WATER in (kind, k2):  # into the water: swimming (out of it: free)
                    add(a, area[u], [frozenset({'flippers'})] if k2 == WATER else [frozenset()])
                elif ht is not None and h2 is not None and ht > h2 and (r in (0, 7) or c in (0, 9)):
                    add(a, area[u], [frozenset()])  # off a doorway tile at the room's edge, down into it (the
                    # entrance's step in) - inside the room a raised edge is a wall (Tail Cave 05E's platform)
            if kind != FLOOR or ht is None:
                continue
            gap = 0  # a jump: 1 or 2 tiles of pit / lava, landing on floor no higher
            while gap < 3 and (r + dr * (gap + 1), c + dc * (gap + 1)) in g and \
                    g[(r + dr * (gap + 1), c + dc * (gap + 1))][0] == PIT:
                gap += 1
            land = (r + dr * (gap + 1), c + dc * (gap + 1))
            if gap in JUMPS and land in area and g[land][0] == FLOOR and (g[land][1] or 0) <= ht + 0.3:
                add(a, area[land], JUMPS[gap])
            # the hookshot: it flies on (over pits, water, floor) to the first solid thing within reach; a target pulls
            # Link to the tile before it - a crossing when that's floor of another area past a pit / water
            over, i = False, 1
            while i <= HOOK_REACH and (r + dr * i, c + dc * i) not in solid:
                t = (r + dr * i, c + dc * i)
                if t not in g:
                    break
                over |= t in g and g[t][0] in (PIT, WATER)
                i += 1
            land = (r + dr * (i - 1), c + dc * (i - 1))
            hit = (r + dr * i, c + dc * i)
            if hit in g and g[hit][0] == FLOOR:  # an object standing on floor (Catfish's Maw 02E's hook post): Link
                land = hit  # is pulled up against it, onto its tile
            if over and i <= HOOK_REACH and hit in hook and land in area and \
                    g[land][0] == FLOOR:
                add(a, area[land], [frozenset({'hookshot'})])
            if k2 == SOLID:  # a ledge: one tile of wall, then floor at least LEDGE lower
                land = (r + 2 * dr, c + 2 * dc)
                if land in area and g[land][0] == FLOOR and g[land][1] is not None and g[land][1] <= ht - LEDGE:
                    add(a, area[land], [frozenset()])
                    if area[land] != a:
                        ledge_drops.setdefault(room, set()).add((u, (dr, dc)))
    # stairs on a raised spot: assumed climbable from where it drops to (Turtle Rock 04E's stairs platform - the only
    # way to 02H / 03H - shows no way up in the tile grid; up for review, rando_room_review)
    import rooms as _rooms
    from RandomizerCore.Tools import leb
    try:
        ox, oz = _rooms.cell_origin(room)
        acts = leb.Room(open(_rooms._leb_path(romfs, room), 'rb').read()).actors
        raised = {area.get((min(7, max(0, int((x.posZ - oz) / 1.5))), min(9, max(0, int((x.posX - ox) / 1.5)))))
                  for x in acts if x.name.decode(errors='ignore').startswith(('AreaDownstairs', 'AreaUpstairs'))}
    except (OSError, ValueError, KeyError):
        raised = set()
    for (a, b), alts in list(moves.items()):
        if a in raised and frozenset() in alts and not any(x != a for (x, y) in moves if y == a):
            add(b, a, [frozenset()])
    fix = room_overrides.ROOMS.get(room, {})  # moves the game has / hasn't (room_overrides.py)
    for key, both in (('cut', False), ('cut2', True)):
        for t, u in fix.get(key, ()):
            for x, y in ((area[t], area[u]), (area[u], area[t])) if both else ((area[t], area[u]),):
                moves.pop((x, y), None)
    for key, both in (('add', False), ('add2', True)):
        for t, u, req in fix.get(key, ()):
            for x, y in ((area[t], area[u]), (area[u], area[t])) if both else ((area[t], area[u]),):
                add(x, y, room_overrides.alts(req))
    if 'check' in fix:  # the room's checks need more: an area of no tiles behind it (cell_terms: every area reached)
        t, req = fix['check']
        add(area[t], n, room_overrides.alts(req))
        n += 1
    out = (area, n, [(a, b, sorted(alts, key=sorted)) for (a, b), alts in sorted(moves.items())])
    _areas_cache[room] = out
    return out


def _doorway_tiles(side, slot):
    """Edge tiles of a doorway: slot = centre in half tiles, or 'wA-B' (a wide opening, tiles A..B)."""
    n = 10 if side in 'NS' else 8
    if isinstance(slot, str):
        a, b = (int(x) for x in slot[1:].split('-'))
        along = range(a, b + 1)
    else:
        along = {slot // 2 - 1, slot // 2} if slot % 2 == 0 else {slot // 2}
    edge = {'N': lambda i: (0, i), 'S': lambda i: (7, i), 'W': lambda i: (i, 0), 'E': lambda i: (i, 9)}[side]
    inward = {'N': (1, 0), 'S': (-1, 0), 'W': (0, 1), 'E': (0, -1)}[side]
    return [edge(i) for i in sorted(along) if 0 <= i < n], inward


def doorway_areas(romfs, room, side, slot):
    """Areas of the room a doorway opens into (its edge tiles; under a door, the first floor a tile or two in)."""
    area, n, _ = room_areas(romfs, room)
    if n == 1:
        return {0}
    tiles, (dr, dc) = _doorway_tiles(side, slot)
    out = set()
    for r, c in tiles:
        for i in range(3):
            t = (r + dr * i, c + dc * i)
            if t in area:
                out.add(area[t])
                break
    return out or set(range(n))  # (nothing found: every area, as before - the old cell-wide doorway)


def spot_areas(romfs, room, x, z):
    """Areas at an actor's spot (its tile, else the floor next to it)."""
    import rooms as _rooms
    area, n, _ = room_areas(romfs, room)
    if n == 1:
        return {0}
    ox, oz = _rooms.cell_origin(room)
    r, c = min(7, max(0, int((z - oz) / 1.5))), min(9, max(0, int((x - ox) / 1.5)))
    if (r, c) in area:
        return {area[(r, c)]}
    near = {area[u] for u in ((r + 1, c), (r - 1, c), (r, c + 1), (r, c - 1)) if u in area}
    return near or set(range(n))


def node(cell, a, n):
    """Graph node of area a of a cell: the cell itself when its room is one area, else 'cell.a'."""
    return cell if n == 1 else f'{cell}.{a}'


def cell_of(nd):
    return nd.split('.', 1)[0]


def and_terms(a, b):
    """AND of two minimal sums of products."""
    out = set()
    for x in a:
        for y in b:
            t = x | y
            if any(o <= t for o in out):
                continue
            out = {o for o in out if not t <= o}
            out.add(t)
    if len(out) > MAX_TERMS:
        out = set(sorted(out, key=lambda t: (len(t), sorted(t)))[:MAX_TERMS])
    return out


def cell_terms(terms):
    """{node: terms} -> {cell: terms}: a cell is 'reached' once every area of it that can be reached is (where in its
    room a check stands isn't known: stricter, never weaker - a check on a ledge needs the ledge)."""
    out = {}
    for nd, t in terms.items():
        c = cell_of(nd)
        if not t:
            out.setdefault(c, set())
            continue
        out[c] = and_terms(out[c], t) if out.get(c) else set(t)
    return out


def entrance_node(cat, romfs, level, layout):
    """The node Link starts in: the entrance room's area at its way out of the dungeon."""
    import dungeon_gen
    ent = dungeon_gen.ENTRANCES[level]
    cell = ent.rsplit('_', 1)[1]
    room = layout.get(cell, ent)
    _, n, _ = room_areas(romfs, room)
    if n == 1:
        return cell
    got = set()
    for s in dungeon_gen._exit_sides(cat, room) if room in cat else ():
        for sl, _, _ in dungeon_gen.openings(cat, room)[s]:
            got |= doorway_areas(romfs, room, s, sl)
    return node(cell, min(got) if got else 0, n)


def rooms_neighbour(room, side):
    import rooms
    return rooms.neighbour(room, side)


def level_graph(cat, romfs, level, layout, jump_rows, openings, trace=None):
    """{node: [(node, frozenset req)]} directed edges; lock count. Nodes: the areas of each cell's room (room_areas;
    node(): a one-area room is just its cell). trace: a list getting (cell, side, slot, target cell) for every edge
    through a doorway (inward_exits)."""
    n = LEVELS[level][0]
    pos = {cell_pos(c, level): c for c in layout}
    info = {cell: room_areas(romfs, room) for cell, room in layout.items()}
    edges = {node(c, a, info[c][1]): [] for c in layout for a in range(info[c][1])}

    def link(src, src_areas, dst, dst_areas, alts):
        for a in sorted(src_areas):
            for b in sorted(dst_areas):
                edges[node(src, a, info[src][1])] += [(node(dst, b, info[dst][1]), r) for r in alts]
    for cell, (_, k, moves) in info.items():  # inside the room
        for a, b, alts in moves:
            edges[node(cell, a, k)] += [(node(cell, b, k), r) for r in alts]
    flags = set()  # distinct locks: one per 'opened' flag (a vanilla door's two halves share it)
    for cell, room in layout.items():
        for d in cat.get(room, {}).get('doors', []):
            if len(d) > 3 and d[2] == 'lock':
                flags.add(lock_flag(romfs, room, d[0]) or (room, d[0]))
    for room in set(layout.values()):
        flags.update(('block', b) for b in lock_blocks(romfs, room))
    locks = len(flags)
    import dungeon_gen
    # drop-down-only doorways (dungeon_gen.inward_only): Link comes in through them, never leaves by them - no edge
    # out (a ledge holding stairs: out from the rooms the stairs come from, see below)
    inward = dungeon_gen.inward_only(romfs) if romfs else {}
    via_stairs = []  # (cell, target, target areas, req): leaving through such a doorway after arriving by the stairs
    jumped = set()
    for row in jump_rows:
        if row[0] != level:
            continue
        cell, side, own = row[1], row[2], int(row[3])
        target, tside, tslot = row[5], row[6], int(row[4])  # (level, cell, side, own, partner, tcell, tside, ...)
        jumped.add((cell, side, own))
        if target not in layout:
            continue
        kind = inward.get((layout[cell], side, own))
        req = door_terms(own_doors(cat, layout[cell], side, own), n, room_types=cat[layout[cell]].get('types', ()),
                         lock_id=f'{lock_flag(romfs, layout[cell], side) or f"{cell}{side}{own}"}',
                         shutter=shutter_req(romfs, layout[cell], side), throw=throw_req(romfs, layout[cell]))
        fx = (room_overrides.door(layout[cell], side, own, 'out'), room_overrides.door(layout[target], tside, tslot, 'in'))
        if req is None or None in fx:
            continue
        req = req | fx[0] | fx[1]
        dst = doorway_areas(romfs, layout[target], tside, tslot)
        if kind == 'arrival':
            via_stairs.append((cell, target, dst, req))
        elif not kind:
            link(cell, doorway_areas(romfs, layout[cell], side, own), target, dst, [req])
            if trace is not None:
                trace.append((cell, side, own, target))
    for cell, room in layout.items():
        op = openings(cat, room)
        c, r = cell_pos(cell, level)
        for side in 'NSWE':
            nb = pos.get((c + STEP[side][0], r + STEP[side][1]))
            if nb is None:
                continue
            theirs = {x[0] for x in openings(cat, layout[nb])[OPP[side]]}
            # original neighbours at home: connected as in the game, even where the doorway model doesn't line up
            # (asked from both sides: a room whose doorway tiles hold its neighbour's door lists no opening there)
            home_pair = room == f'{level}_{cell}' and layout[nb] == f'{level}_{nb}' and \
                (rooms_neighbour(room, side) == layout[nb] or rooms_neighbour(layout[nb], OPP[side]) == room)
            slots = [x[0] for x in op[side]]
            if home_pair:  # (either room's doorway: the door's own tiles can hide it on one side)
                slots += [t for t in theirs if t not in slots]
            for slot in slots:
                if (cell, side, slot) in jumped or (slot not in theirs and not home_pair):
                    continue
                # (a pot door is broken by throwing a pot at it from the room Link stands in - this one)
                req_out = door_terms(own_doors(cat, room, side, slot), n, room_types=cat[room].get('types', ()),
                                     lock_id=f'{lock_flag(romfs, room, side) or f"{cell}{side}{slot}"}',
                                     shutter=shutter_req(romfs, room, side), throw=throw_req(romfs, room))
                req_in = door_terms(own_doors(cat, layout[nb], OPP[side], slot), n, entering=True,
                                    room_types=cat[layout[nb]].get('types', ()),
                                    lock_id=f'{lock_flag(romfs, layout[nb], OPP[side]) or f"{nb}{OPP[side]}{slot}"}',
                                    shutter=shutter_req(romfs, layout[nb], OPP[side]), throw=throw_req(romfs, room))
                fx = (room_overrides.door(room, side, slot, 'out'), room_overrides.door(layout[nb], OPP[side], slot, 'in'))
                if req_out is None or req_in is None or None in fx:
                    continue
                req_out, req_in = req_out | fx[0], req_in | fx[1]
                kind = inward.get((room, side, slot))
                dst = doorway_areas(romfs, layout[nb], OPP[side], slot)
                if kind == 'arrival':
                    via_stairs.append((cell, nb, dst, req_out | req_in))
                elif not kind:
                    link(cell, doorway_areas(romfs, room, side, slot), nb, dst, [req_out | req_in])
                    if trace is not None:
                        trace.append((cell, side, slot, nb))
    for cell, room in layout.items():  # leaving a room with keyhole blocks: they may guard the way (see lock_blocks)
        bl = frozenset(f'LOCK|{n}|block{b}' for b in lock_blocks(romfs, room))
        if bl:
            for a in range(info[cell][1]):
                nd = node(cell, a, info[cell][1])
                edges[nd] = [(t, r | bl if cell_of(t) != cell else r) for t, r in edges[nd]]
    where = {room: cell for cell, room in layout.items()}  # (LINK_LEVELS: stairs / hole rooms may have moved)
    stairs_from = {}  # cell -> [(cell, areas, alternatives)] whose stairs arrive in it
    for a, p, b, q, alts in stairs_ports(romfs, level, cat):
        if level in dungeon_gen.LINK_LEVELS:
            ca, cb = where.get(a), where.get(b)
        else:  # (stairs rooms stay home)
            ca, cb = a.rsplit('_', 1)[1], b.rsplit('_', 1)[1]
            if layout.get(ca) != a or layout.get(cb) != b:
                continue
        if ca and cb:
            sa = spot_areas(romfs, a, *p)
            link(ca, sa, cb, spot_areas(romfs, b, *q), alts)
            stairs_from.setdefault(cb, []).append((ca, sa, alts))
    for cell, target, dst, req in via_stairs:  # (Turtle Rock 06E's south strip: its upstairs arrive on it)
        for src, sa, alts in stairs_from.get(cell, ()):
            link(src, sa, target, dst, [req | x for x in alts])
    if level in dungeon_gen.LINK_LEVELS:  # holes: one way down, into the room holding the landing spot
        for a, p, b, starts in hole_links(romfs, level, cat, spots=True):
            if where.get(a) and where.get(b):
                land = set().union(*(spot_areas(romfs, b, *q) for q in starts)) if starts else \
                    set(range(info[where[b]][1]))
                link(where[a], spot_areas(romfs, a, *p), where[b], land, [frozenset()])
    for cell, room in layout.items():  # leaving a mini-boss room (not into its own unit): its soul (EXIT_REQ)
        if EXIT_REQ.get(room):
            for a in range(info[cell][1]):
                nd = node(cell, a, info[cell][1])
                edges[nd] = [(t, r if cell_of(t) == cell or EXIT_REQ.get(layout.get(cell_of(t))) == EXIT_REQ[room]
                              else r | EXIT_REQ[room]) for t, r in edges[nd]]
    return edges, locks


def inward_exits(cat, romfs, level, layout, jump_rows):
    """[(cell, side, slot, target)]: logic edges that leave a cell through a drop-down-only doorway
    (dungeon_gen.inward_only) - must be none (seed_sweep.py checks every room-shuffled seed). layout: dungeon_gen's
    ({'Lv.._08D': room}) or cells ('08D')."""
    import dungeon_gen
    L = {c.rsplit('_', 1)[-1]: r for c, r in layout.items()}
    trace = []
    level_graph(cat, romfs, level, L, jump_rows, dungeon_gen.openings, trace)
    inward = dungeon_gen.inward_only(romfs)
    return [t for t in trace if (L[t[0]], t[1], t[2]) in inward]


def key_counts(edges, start):
    """{lock atom: keys it needs} by the worst-case key rule: a lock needs one key more than every OTHER lock that
    can be reached (and opened, wasting a key) without going through it. Safe for any order the player opens doors
    in; keys may sit behind earlier locks (the all-keys-before-any-lock rule made some layouts unfillable)."""
    locks = {a for outs in edges.values() for _, r in outs for a in r if a.startswith('LOCK|')}
    starts = list(start) if isinstance(start, (list, set, frozenset, dict)) else [start]
    out = {}
    for L in locks:
        seen, todo, met = set(starts), list(starts), set()
        while todo:
            a = todo.pop()
            for b, r in edges[a]:
                if L in r:
                    continue
                met |= {x for x in r if x.startswith('LOCK|')}
                if b not in seen:
                    seen.add(b)
                    todo.append(b)
        out[L] = len(met) + 1
    return out


def resolve_keys(terms, kc, n, total):
    """LOCK atoms of each term -> one key-Dn:k (the largest k of the term's locks, at most the dungeon's keys)."""
    out = set()
    for t in terms:
        ks = [kc.get(a, total) for a in t if a.startswith('LOCK|')]
        rest = {a for a in t if not a.startswith('LOCK|')}
        if ks:
            rest.add(f'key-D{n}:{min(max(ks), total)}')
        out.add(frozenset(rest))
    return out


def reach_terms(edges, start, base):
    """{cell: set of frozensets} minimal sum-of-products of what reaching the cell needs."""
    req = {c: set() for c in edges}
    req[start] = {frozenset([base])}
    changed, passes = True, 0
    while changed and passes < 400:  # (the MAX_TERMS cut can keep trading terms: stop, the cheapest stay)
        changed, passes = False, passes + 1
        for a, outs in edges.items():
            for b, er in outs:
                for t in list(req[a]):
                    nt = t | er
                    if any(o <= nt for o in req[b]):
                        continue
                    req[b] = {o for o in req[b] if not nt <= o}
                    req[b].add(nt)
                    if len(req[b]) > MAX_TERMS:  # keep the cheapest
                        req[b] = set(sorted(req[b], key=lambda t: (len(t), sorted(t)))[:MAX_TERMS])
                    changed = True
    return req


# ---------------------------------------------------------------- applying it to LAS's logic
def apply(cat, romfs, layouts, jump_rows, logic_defs, item_defs, settings, extra_locations=None):
    """Rewrites logic_defs / item_defs for the shuffled dungeons. extra_locations: {name: room} (pots...).
    Returns a summary string. Sets settings['dungeon-of'] (location -> 'Dn') and adds unreachable checks to
    settings['excluded-locations']."""
    import dungeon_gen
    # (dungeon_gen names cells like rooms, 'Lv01TailCave_08D'; here cells are '08D')
    layouts = {lv: {c.rsplit('_', 1)[1]: room for c, room in L.items()} for lv, L in layouts.items()}
    lr = json.load(open(os.path.join(HERE, 'location_rooms.json')))
    rooms_at = {}  # room -> [(level, cell)]
    cell_expr = {}  # (level, cell) -> condition string
    lock_count = {}
    logic_defs['rs-shutter'] = {'type': 'interaction', 'condition-basic': SHUTTER}
    logic_defs['rs-switch-shutter'] = {'type': 'interaction', 'condition-basic': SWITCH_SHUTTER}
    logic_defs['rs-jump'] = {'type': 'interaction', 'condition-basic': JUMP}
    logic_defs['rs-long-jump'] = {'type': 'interaction', 'condition-basic': LONG_JUMP}
    boss_cell = {}
    junk_delta = [0]
    kcs = {}
    cell_keys = {}
    check_keys = {}  # location -> fewest keys reaching it takes
    for level, layout in layouts.items():
        n, region = LEVELS[level]
        # a dungeon region defined by its own cells (Face Shrine: "D6-8D | D6-3A"): the cells now hang off the region,
        # so the region becomes the way in from outside - the entrance cell's vanilla alternatives that don't go
        # through the dungeon itself (seed RapidBowWow994: no D6 check was reachable)
        rc = logic_defs.get(region, {}).get('condition-basic', '')
        if re.search(rf'\bD{n}-\d', rc):
            start0 = dungeon_gen.ENTRANCES[level].rsplit('_', 1)[1]
            ent = logic_defs.get(f'D{n}-{start0.lstrip("0")}', {}).get('condition-basic', '')
            outside = [a.strip() for a in top_level_or(ent) if not re.search(rf'\bD{n}-\d', a)]
            if outside:
                logic_defs[region]['condition-basic'] = ' | '.join(f'({a})' for a in outside)
        for cell, room in layout.items():
            rooms_at.setdefault(room, []).append((level, cell))
            for d in cat.get(room, {}).get('doors', []):
                if len(d) > 3 and d[2] == 'bossdoor':  # the boss sits behind it
                    c, r = cell_pos(cell, level)
                    dc, dr = STEP[d[0]]
                    boss_cell[level] = cell_name(c + dc, r + dr, level)
        edges, locks = level_graph(cat, romfs, level, layout, jump_rows, dungeon_gen.openings)
        start = entrance_node(cat, romfs, level, layout)
        terms = cell_terms(reach_terms(edges, start, region))
        kc = key_counts(edges, start)
        kcs[level] = (kc, n)
        nlocks = len(kc)
        locks = max(locks, nlocks)  # (keys: at least one per lock the layout can meet)
        lock_count[n] = locks
        for cell, t in terms.items():
            cell_expr[(level, cell)] = dnf_str(resolve_keys(t, kc, n, locks))
            ks = [min(max([kc.get(a, locks) for a in x if a.startswith('LOCK|')] or [0]), locks) for x in t
                  if not any(a.startswith('nightmare-key') for a in x)]
            if ks:
                cell_keys[(level, cell)] = min(ks)  # (fewest keys any way in; the fill check below)
        # LAS's cell regions of this dungeon: "reach that cell of the new layout"
        for k, v in logic_defs.items():
            if v.get('type') == 'room' and re.match(rf'D{n}-\d\d?[A-P]$', k):
                cell = k.split('-', 1)[1].rjust(3, '0')
                logic_defs[k]['condition-basic'] = cell_expr.get((level, cell)) or f'nightmare-key-D{n} & !nightmare-key-D{n}'
                for key in ('condition-advanced', 'condition-glitched', 'condition-hell'):
                    logic_defs[k].pop(key, None)  # (the vanilla tricks start from vanilla neighbour cells)
        # small keys: as many as the new layout has locks (the pool keeps its size: extra keys replace junk, fewer
        # keys leave room for junk - LAS has exactly one item per location)
        if f'key-D{n}' in item_defs:  # exactly one per lock (user: no spare keys)
            delta = locks - item_defs[f'key-D{n}']['quantity']
            item_defs[f'key-D{n}']['quantity'] = locks
            junk_delta[0] += delta
    # balance the pool (LAS: one item per location): take junk for keys added, give junk back for keys removed. Only
    # what no check could still claim as its vanilla content is taken (the Color Dungeon's rupees stay vanilla and
    # need their 5-rupee items: seed RapidBowWow994)
    claimed = {}
    for v in logic_defs.values():
        if v.get('type') == 'item' and v.get('content'):
            claimed[v['content']] = claimed.get(v['content'], 0) + 1
    left = junk_delta[0]
    while left > 0:
        spare = sorted((k for k, d in item_defs.items() if k.startswith(('rupee-', 'chamber-stone'))
                        and d.get('quantity', 0) - claimed.get(k, 0) > 0),
                       key=lambda k: claimed.get(k, 0) - item_defs[k]['quantity'])
        if not spare:
            break
        item_defs[spare[0]]['quantity'] -= 1
        left -= 1
    if left < 0 and 'rupee-20' in item_defs:
        item_defs['rupee-20']['quantity'] -= left
    shuffled = {LEVELS[lv][0] for lv in layouts}
    # events and interactions that name vanilla cells of a shuffled dungeon: D<n>-clear = reach the boss arena of the
    # new layout + what the event needs there; others (ammo from a dungeon room's pots) lose those alternatives - the
    # room at that cell is another one now. Then vanilla key counts (D0-clear: key-D0:3; the new layout can have fewer
    # keys - seed DevWarp2: the Color Dungeon clear unreachable): every key of the new layout, the safe bound
    cell_tok = re.compile(r'D(\d)-\d\d?[A-P]$')
    for k, v in logic_defs.items():
        if v.get('type') in ('item', 'room') or k == 'face-shrine':
            continue
        m = re.match(r'D(\d)-clear$', k)
        if m and int(m.group(1)) in shuffled:
            lv = next(l for l, (i, _) in LEVELS.items() if i == int(m.group(1)))
            arena = cell_expr.get((lv, boss_cell.get(lv)))
            if arena:
                rest, _ = strip(parse(v.get('condition-basic', '')), lambda a: cell_tok.match(a) or
                                re.match(r'key-D\d(:\d+)?$', a))
                v['condition-basic'] = f'({arena}) & ({show(rest)})' if rest else arena
            continue
        for key in ('condition-basic', 'condition-advanced', 'condition-glitched', 'condition-hell'):
            c = v.get(key)
            if c and any(int(x) in shuffled for x in re.findall(r'\bD(\d)-\d\d?[A-P]\b', c)):
                n2 = falsify(parse(c), lambda a: bool(cell_tok.match(a)) and int(cell_tok.match(a).group(1)) in shuffled)
                v[key] = show(n2) if n2 else 'nightmare-key-D0 & !nightmare-key-D0'  # (never)
    for k, v in logic_defs.items():
        if v.get('type') in ('item', 'room'):
            continue
        for key in ('condition-basic', 'condition-advanced', 'condition-glitched', 'condition-hell'):
            c = v.get(key)
            if c and re.search(r'key-D\d:\d+', c):
                v[key] = re.sub(r'key-D(\d):\d+', lambda m: f'key-D{m.group(1)}:{lock_count[int(m.group(1))]}'
                                if int(m.group(1)) in shuffled and lock_count.get(int(m.group(1))) else m.group(0), c)
    region_tok = {k for k, v in logic_defs.items() if v.get('type') in ('room', 'region')}

    def drop(atom):
        return atom in region_tok or re.match(r'key-D\d(:\d+)?$', atom) is not None

    def remap_cells(cond):
        """Cell names of shuffled dungeons (D8-4C) in a vanilla condition -> reaching the room vanilla has there,
        wherever it is now. (condition, room gone from every layout?)"""
        gone = [False]

        def vanilla_room(m):
            x = int(m.group(1))
            if x not in shuffled:
                return m.group(0)
            xl = next(l for l, (i, _) in LEVELS.items() if i == x)
            ps = [cell_expr[p] for p in rooms_at.get(f'{xl}_{m.group(2).rjust(3, "0")}', []) if cell_expr.get(p)]
            gone[0] |= not ps
            return '(' + ' | '.join(f'({e})' for e in ps) + ')' if ps else m.group(0)
        return re.sub(r'\bD(\d)-(\d\d?[A-P])\b', vanilla_room, cond), gone[0]

    dungeon_of = settings.setdefault('dungeon-of', {})
    excluded = settings.setdefault('excluded-locations', set())
    absent = settings.setdefault('absent-locations', set())  # (their rooms aren't in the layout: not in the game)
    rewritten = unused = fallback = 0
    # dungeons with a crystal switch somewhere in their layout
    crystal = {lv: any('ObjCrystalSw' in cat.get(r, {}).get('actors', ()) for r in layout.values())
               for lv, layout in layouts.items()}
    targets = {k: (v.get('room'), v.get('level')) for k, v in lr.items()}
    for k, room in (extra_locations or {}).items():
        targets[k] = (room, room.rsplit('_', 1)[0])
    for loc, v in list(logic_defs.items()):
        if v.get('type') != 'item' or loc not in targets:
            continue
        room, level = targets[loc]
        m = re.match(r'D(\d)-', loc)
        home = int(m.group(1)) if m else (LEVELS[level][0] if level in LEVELS else None)
        if home not in shuffled:
            continue
        if room is None and v.get('subtype') == 'boss':
            lv = next(l for l, (i, _) in LEVELS.items() if i == home)
            if lv in boss_cell:
                room = layouts[lv].get(boss_cell[lv])
        cond = v.get('condition-basic', '')
        if room is None:  # no room known (owl statues, Color Dungeon rupees...): vanilla condition, where its cell
            fallback += 1  # names (D8-4C) mean the room vanilla has there, wherever it is now (seed DevRL_chaos_off:
            n = home       # above-turtle-rock read "whatever room sits at 4C")
            hl = next(l for l, (i, _) in LEVELS.items() if i == n)
            c2, gone = remap_cells(cond)
            if gone:  # (its room isn't in any layout: never reachable)
                excluded.add(loc), absent.add(loc)
                unused += 1
                continue
            c2 = re.sub(rf'key-D{n}:\d+', f'key-D{n}:{lock_count.get(n, 0)}' if lock_count.get(n) else '', c2)
            v['condition-basic'] = re.sub(r'&\s*&', '&', c2).strip(' &') or LEVELS[hl][1]
            for key in ('condition-advanced', 'condition-glitched', 'condition-hell'):
                v.pop(key, None)
            continue
        places = rooms_at.get(room, [])
        def with_blocks(p):  # a check in a room with keyhole blocks: they may guard it
            e = cell_expr.get(p)
            bl = lock_blocks(romfs, room)
            if not e or not bl or p[0] not in kcs:
                return e
            kc, n = kcs[p[0]]
            k = max(kc.get(f'LOCK|{n}|block{b}', 1) for b in bl)
            return f'({e}) & key-D{n}:{k}'
        reach = [with_blocks(p) for p in places if cell_expr.get(p)]
        ck = [cell_keys[p] for p in places if p in cell_keys]
        via = LEDGE_VIA.get(loc)
        if via and reach:  # (a ledge reached from its vanilla neighbour: that room's cell too)
            ve = [cell_expr[p] for p in rooms_at.get(via, []) if cell_expr.get(p)]
            reach = [f'({e}) & ({" | ".join(f"({x})" for x in ve)})' for e in reach] if ve else []
            vk = [cell_keys[p] for p in rooms_at.get(via, []) if p in cell_keys]
            if ck and vk:
                ck = [max(min(ck), min(vk))]
        if ck:
            k = min(ck)
            bl = lock_blocks(romfs, room)
            p0 = next((p for p in places if p in cell_keys and cell_keys[p] == k), None)
            if bl and p0 and p0[0] in kcs:
                kc0, n0 = kcs[p0[0]]
                k = max([k] + [kc0.get(f'LOCK|{n0}|block{b}', 1) for b in bl])
            check_keys[loc] = k
        if not reach and any(p[1] == room.rsplit('_', 1)[1] and p[0] == room.rsplit('_', 1)[0] for p in places):
            fallback += 1  # a room at home the graph can't enter (no doorways: Bottle Grotto 03C, reached by a fall):
            continue       # its vanilla condition, on the redefined cell regions
        if not reach:
            excluded.add(loc), absent.add(loc)
            unused += 1
            continue
        # (a pot: its room's vanilla way in isn't this room's business - only what the pot itself needs)
        cond = v.get('room-local', cond)
        local, _ = strip(parse(cond), drop)
        lvl_now = next((p for p in places if LEVELS[p[0]][0] == home), places[0])[0]
        if 'hit-crystal-switch' in cond and not crystal.get(lvl_now):
            # (crystal blocks guard it, and the dungeon it's in now has no crystal switch to lower them)
            excluded.add(loc), absent.add(loc)
            unused += 1
            continue
        r = reach[0] if len(reach) == 1 else ' | '.join(f'({x})' for x in reach)
        v['condition-basic'] = f'({r}) & ({show(local)})' if local else r
        for key in ('condition-advanced', 'condition-glitched', 'condition-hell'):
            v.pop(key, None)  # (the vanilla tricks describe the vanilla layout)
        lvl0 = next((p for p in places if LEVELS[p[0]][0] == home), places[0])[0]
        dungeon_of[loc] = f'D{LEVELS[lvl0][0]}'
        rewritten += 1
    # item locations the loop doesn't own (no room listed, not a dungeon name) that sit in a dungeon cell
    # (above-turtle-rock: "D8-4C")
    for loc, v in logic_defs.items():
        if v.get('type') != 'item' or loc in dungeon_of or loc in excluded:
            continue
        cond = v.get('condition-basic', '')
        if not any(int(x) in shuffled for x in re.findall(r'\bD(\d)-\d\d?[A-P]\b', cond)):
            continue
        c2, gone = remap_cells(cond)
        if gone:
            excluded.add(loc), absent.add(loc)
            unused += 1
            continue
        v['condition-basic'] = c2
        for key in ('condition-advanced', 'condition-glitched', 'condition-hell'):
            v.pop(key, None)
        rewritten += 1
    # can each dungeon be filled? (keysanity off: its keys go into its own checks.) Key j has to sit in a check that
    # needs at most j-1 keys - else the caller plans another layout
    if settings.get('keysanity', 'off') == 'off':
        need_k = {}  # 'Dn' -> [keys each check of the dungeon needs]
        for loc, d in dungeon_of.items():
            v = logic_defs.get(loc, {})
            if loc in excluded or v.get('type') != 'item' or loc not in check_keys or vanilla_check(loc, v, settings):
                continue
            need_k.setdefault(d, []).append(check_keys[loc])
        for level, (kc, n) in kcs.items():
            total = lock_count[n]
            ks = sorted(need_k.get(f'D{n}', []) + [total] * roomless_checks(settings).get(n, 0))
            for j in range(1, total + 1):
                if sum(1 for k in ks if k <= j - 1) < j:
                    settings['room-logic-unfillable'] = (f'{level}: key {j} of {total} has '
                                                         f'{sum(1 for k in ks if k <= j - 1)} checks to go in')
                    break
            else:
                if len(ks) < total + dungeon_items(settings):  # (room for the Nightmare Key, Map, Compass, Beak too)
                    settings['room-logic-unfillable'] = f'{level}: {len(ks)} checks for {total + dungeon_items(settings)} dungeon items'
    # a dungeon whose instrument the room graph can't reach (cut off from its entrance): never a beatable seed - the
    # next layout instead (seen with Eagle's Tower, DevEaglePlay + dungeon items mcb: the whole tower unreachable)
    lost = sorted(k for k in excluded if re.match(r'D\d-instrument$', k))
    if lost and not settings.get('room-logic-unfillable'):
        settings['room-logic-unfillable'] = 'unreachable ' + ', '.join(lost)
    # Eagle's Tower (collapsed, eagle_tower.py): the boss is on the roof, up the stairs at the end of 02F's raised
    # walkway (hookshot / rooster) - 02F has other doorways, so the room graph would let Link past the Nightmare door.
    # Its checks need 03F (the Nightmare door's room: 02F / 03F / 03G / 02G stay together, dungeon_gen.STAY) and the key.
    tower = cell_expr.get(('Lv07EagleTower', '03F')) if 'Lv07EagleTower' in layouts else None
    if tower:
        roof = f'({tower}) & nightmare-key-D7 & (hookshot | (rooster & bad-pets))'
        for k in ('D7-instrument', 'D7-eagle'):
            if k in logic_defs:
                logic_defs[k]['condition-basic'] = f'{roof} & hookshot & kill-evil-eagle'
                for lv_ in ('advanced', 'glitched', 'hell'):
                    logic_defs[k].pop('condition-' + lv_, None)
    return (f'{len(layouts)} dungeons, {rewritten} checks rewritten, {unused} in unused rooms (excluded), '
            f'{fallback} without a room (vanilla condition), locks {dict(sorted(lock_count.items()))}')


def boss_door_ok(cat, romfs, level, layout, rows):
    """The generator's check (Coupled / Decoupled): every room with a doorway is reachable, and without passing a boss
    door every cell is except the boss arena and the instrument room (DevRL_chaos_off: Turtle Rock's entrance was paired straight into the room before
    the boss door - the whole dungeon sat behind the Nightmare Key). layout: dungeon_gen's ({'Lv.._08D': room})."""
    import dungeon_gen
    L = {c.rsplit('_', 1)[1]: r for c, r in layout.items()}
    edges, _ = level_graph(cat, romfs, level, L, rows, dungeon_gen.openings)
    start = entrance_node(cat, romfs, level, L)
    def reach(boss_doors):
        seen, todo = {start}, [start]
        while todo:
            a = todo.pop()
            for b, r in edges[a]:
                if b not in seen and (boss_doors or not any(x.startswith('nightmare-key') for x in r)):
                    seen.add(b)
                    todo.append(b)
        return seen
    every = reach(True)
    # every room with a doorway is reached somehow (the stairs pair as in the game now - seed RapidBowWow958 - and no
    # longer join a dungeon's far rooms by themselves; Bottle Grotto 03C, reached by a fall, has no doorway)
    if any(not any(cell_of(x) == c for x in every) for c, r in L.items()
           if any(cat.get(r, {}).get('openings', {}).get(s) for s in 'NSWE')):
        return False
    behind = every - reach(False)  # (areas unreachable either way aren't this check's business)
    allowed = set()
    n = LEVELS[level][0]
    lr = json.load(open(os.path.join(HERE, 'location_rooms.json')))
    inst = (lr.get(f'D{n}-instrument') or {}).get('room')
    for cell, room in L.items():
        if room == inst:
            allowed.add(cell)
        for d in cat.get(room, {}).get('doors', []):
            if len(d) > 3 and d[2] == 'bossdoor':
                c, r = cell_pos(cell, level)
                allowed.add(cell_name(c + STEP[d[0]][0], r + STEP[d[0]][1], level))
    allowed = {nd for nd in edges if cell_of(nd) in allowed}
    # beyond the boss arena (the instrument room, the Color Dungeon's reward room): reached through the arena
    todo = [c for c in allowed if c in behind]
    while todo:
        a = todo.pop()
        for b, _ in edges.get(a, []):
            if b in behind and b not in allowed:
                allowed.add(b)
                todo.append(b)
    return not (behind - allowed)


INSTRUMENTS = ('full-moon-cello', 'conch-horn', 'sea-lilys-bell', 'surf-harp', 'wind-marimba', 'coral-triangle',
               'evening-calm-organ', 'thunder-drum')


def vanilla_check(loc, v, settings):
    """A check LAS keeps vanilla (RandomizerCore/shuffler.py run()): no dungeon item can go there. (An unshuffled
    instrument counted as room for one: DevGrid1's Tail Cave had 7 checks for 8 items - LAS crashed placing them.)"""
    if v.get('content') in INSTRUMENTS and not settings.get('shuffle-instruments'):
        return True
    if loc.startswith('D0-rupee-') and not settings.get('blupsanity'):
        return True
    return v.get('subtype') not in ('chest', 'boss', 'enemy', 'drop', 'npc', 'standing', 'overworld-statue',
                                    'dungeon-statue', 'underwater')


def dungeon_items(settings):
    """How many items besides the small keys stay in their dungeon: Nightmare Key, Map, Compass, Stone Beak, less
    what's given at the start."""
    from RandomizerCore.Randomizers.data import start_dungeon_items
    return 4 - len(start_dungeon_items({'dungeon-items': 'none', **settings}))


def lr_vanilla(k, v, settings):
    """vanilla_check() for a location_rooms.json entry (it has no content): LAS keeps it vanilla / deletes it."""
    if k.endswith('-instrument') and not settings.get('shuffle-instruments'):
        return True
    if v.get('subtype') == 'dungeon-statue' and not settings.get('owl-dungeon-gifts'):
        return True  # (LAS deletes the owl statues' checks then)
    return k.startswith('D0-rupee-') and not settings.get('blupsanity')


def roomless_checks(settings):
    """{dungeon number: checks LAS counts in that dungeon (by name) that have no room (owl statues...)} - they keep
    their vanilla condition, which needs every key of the new layout."""
    lr = json.load(open(os.path.join(HERE, 'location_rooms.json')))
    excluded = settings.get('excluded-locations', ())
    out = {}
    for k, v in lr.items():
        m = re.match(r'D(\d)-', k)
        if lr_vanilla(k, v, settings):
            continue
        if m and not v.get('room') and v.get('subtype') != 'boss' and k not in excluded:
            out[int(m.group(1))] = out.get(int(m.group(1)), 0) + 1
    return out


def fill_ok(cat, romfs, level, layout, rows, settings=None):
    """The generator's other check (keysanity off: a dungeon's keys go into its own checks): key j needs a check that
    takes at most j-1 keys (by the worst-case key rule) - the first lock reached needs as many checks before it as
    locks reachable from the start. layout: dungeon_gen's. (Only checks with a known room count; apply() checks the
    finished logic again.)"""
    import dungeon_gen
    L = {c.rsplit('_', 1)[1]: r for c, r in layout.items()}
    edges, locks = level_graph(cat, romfs, level, L, rows, dungeon_gen.openings)
    start = entrance_node(cat, romfs, level, L)
    kc = key_counts(edges, start)
    total = max(locks, len(kc))
    n = LEVELS[level][0]
    cell_k = {}
    for cell, terms in cell_terms(reach_terms(edges, start, 'in')).items():
        ks = [max([kc.get(a, total) for a in t if a.startswith('LOCK|')] or [0]) for t in terms
              if not any(a.startswith('nightmare-key') for a in t)]
        if ks:
            cell_k[cell] = min(ks)
    lr = json.load(open(os.path.join(HERE, 'location_rooms.json')))
    per_room = {}
    for k, v in lr.items():
        if v.get('room') and v.get('subtype') != 'boss' and not lr_vanilla(k, v, settings or {}):
            per_room[v['room']] = per_room.get(v['room'], 0) + 1
    ks = []
    for room, count in per_room.items():
        cells = [c for c, r in L.items() if r == room and c in cell_k]
        if not cells:
            continue
        k = min(cell_k[c] for c in cells)
        bl = lock_blocks(romfs, room)
        if bl:
            k = max([k] + [kc.get(f'LOCK|{n}|block{b}', 1) for b in bl])
        ks += [k] * count
    settings = settings or {}
    ks += [total] * roomless_checks(settings).get(n, 0)
    return (all(sum(1 for k in ks if k <= j - 1) >= j for j in range(1, total + 1)) and
            len(ks) >= total + dungeon_items(settings))
