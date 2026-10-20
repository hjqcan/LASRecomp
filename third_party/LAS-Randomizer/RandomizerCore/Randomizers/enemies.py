import random
from RandomizerCore.randomizer_data import ENEMY_DATA

# LARecomp: mini-bosses that work as overworld enemies (spawn and fight outside their dungeon arena; Hinox is woken by
# the runtime, runtime/hle/miniboss.c). id -> leb parameters. Only replaces randomized 'land' enemies on the Field
# (never the Excluded_Actors kept vanilla for logic / tricks, so no check needs one killed), with a chance per enemy
# (setting 'enemy-minibosses', 'miniboss-frequency': MINIBOSS_CHANCES) - at most one per room, except with All, where
# every enemy that has a mini-boss fitting its spot becomes one. No switches: no defeat flag is ever written.
# Left out (2026-10-12): Moblin Chief 0x52 / Grim Creeper 0x5d (constructed, then deleted at once by the actor
# manager - their init decides; runtime RE in notes), Lanmola 0x57 (sand only).
FIELD_MINIBOSSES = {
    0x53: ['HinoxStart'],  # Hinox (red, or blue: MINIBOSS_VARIANTS)
    0x56: [],              # Dodongo Snake (bombs)
    0x5B: [],              # Armos Knight
    0x61: [],              # Giant Buzz Blob (Magic Powder, then sword)
    0x5C: [],              # Smasher (with its iron ball)
    0x60: [],              # Avalaunch
    0x51: ['RolaStart'],   # Rolling Bones (with its spike bar)
    0x5F: [],              # Blaino (his knock-out punch: Link falls back down - runtime)
    0x58: [1, 0],          # Cue Ball (circles a rail fitted to the room)
    0x59: [3],             # Master Stalfos (starts by himself; 3: his last fight - no escape, he dies: miniboss_guard)
    0x57: [],              # Lanmola (his rail from Field_13O; copies give no item: tools/rando/miniboss_guard.py)
    0x52: ['Bossblin'],    # Moblin Chief (his hideout's story check skipped on the Field: tools/patches.py)
    0x55: [],              # Ball-and-chain Soldier (Dampe's chamber dungeons only; fights on his own)
    0x21E: [],             # Shadow Link (Dampe's chamber, unlocked by an amiibo in the original; rises from a shadow and
    #                        fights on his own; his defeat event only plays his fade - no flags, a rupee drops)
}
MINIBOSS_VARIANTS = {0x53: [['HinoxStart'], [2]]}  # (one picked per mini-boss: Hinox 2 = Turtle Rock's blue one)
ANY_TERRAIN_MINIBOSSES = {0x58}  # (also on water / shallow water / air / tree / hole spots: Cue Ball, tested 2026-10-12)
# Out for now (user 2026-10-12, maybe later): Gohma 0x5A - its own AI heads for Link with targeted patches
# (runtime/hle/miniboss.c, tools/patches.py GOHMA_*; walls from tools/rando/roam.py), but it isn't good enough yet.
# Out (user 2026-10-12): Grim Creeper 0x5D - placed on the Field he is never created (his class is built once at level
# load; the placements only get a stub), and his fight is his room's script (bats, doors, AreaEventBox).
# Mini-bosses that need room (Avalaunch rolls spokes out from its spot; Master Stalfos is big; Moblin Chief charges) only
# where it isn't cramped: at most NEEDS_ROOM[type] of the 5 x 5 tiles around it blocked (the screen's grid, original
# room; bushes and trees count; near the screen's edge, the same share of the tiles on it). Typical spots have 5 - 8,
# cramped ones 10 - 17.
NEEDS_ROOM = {0x60: 8, 0x59: 8, 0x52: 6}  # (most blocked of 25: Avalaunch, Master Stalfos, Moblin Chief - he charges)
# The other big ones (and Cue Ball, whose loop needs free tiles) skip the tightest land spots (over BIG_ROOM blocked,
# about 1 in 10). The small ones - Shadow Link, the Ball-and-chain Soldier and Blaino (the boxer), Link-sized - fit any
# land spot, even those (user 2026-10-18: not water, shallow water or holes - those keep Cue Ball only). Several
# mini-boss spots on one screen (frequency All): one within MB_GAP of a mini-boss already there may only be a small one.
BIG_ROOM = 14
SMALL_MINIBOSSES = {0x21E, 0x55, 0x5F}  # Shadow Link, Ball-and-chain Soldier, Blaino
MB_GAP = 4.5  # (three tiles: the big ones are 2 - 3 tiles across; Smasher's iron ball lies 2.5 tiles off him)
# Screens where only the small ones work (user 2026-10-18): the graveyard - rows of gravestones, the big ones (Cue Ball
# too) got wedged between them. Its spots aren't tight by the 5 x 5 measure (gravestones in a lawn: ~45% of its tiles
# blocked, ~40 screens are tighter), so a list. Its spots are Ghinis (air): there a small one stands on the ground.
SMALL_ONLY_SCREENS = {'Field_07G', 'Field_07H', 'Field_08G', 'Field_08H'}
SMALL_ON_AIR = True  # (tested 2026-10-18 at the graveyard's Ghini spots: they land and fight)


def room_needed(mini):
    """Most blocked of the 25 tiles around a land spot for `mini` (25: no limit)."""
    return 25 if mini in SMALL_MINIBOSSES else NEEDS_ROOM.get(mini, BIG_ROOM)


SOLID_OBJECTS = {0xB6, 0xB8, 0xCB, 0x21B}  # bushes, rocks, gravestones, wooden boxes (actors: not in the grid)


def roomy(room, act, most_blocked):
    import math
    try:
        i, tiles = room.grid.info, room.grid.tilesdata
    except AttributeError:
        return True
    tile = lambda x, z: (math.floor((x - i.x_coord) / i.tile_size), math.floor((z - i.z_coord) / i.tile_size))
    cx, cz = tile(act.posX, act.posZ)
    solid = {tile(a.posX, a.posZ) for a in room.actors if a.type in SOLID_OBJECTS}
    blocked = seen = 0
    for z in range(cz - 2, cz + 3):
        for x in range(cx - 2, cx + 3):
            if 0 <= x < i.room_width and 0 <= z < i.room_height:
                t = tiles[z * i.room_width + x]
                seen += 1
                blocked += bool(t.flags1['containscollision'] or t.flags1['deepwaterlava'] or t.flags3['iswaterlava']
                                or (x, z) in solid)
    return blocked * 25 <= most_blocked * seen  # (near the screen's edge: the same share of the tiles on it)


# Chance per eligible enemy for frequency Normal / 2x / 3x / All. Normal, 2x and 3x keep one mini-boss per screen;
# 2x / 3x are set so the seed gets about twice / three times Normal's count (8%: ~19 on the Field; 16% / 24% under
# the one-per-screen limit gave only 1.9x / 2.7x, as many screens have 2 - 6 enemies). All: every enemy whose spot
# fits one (no limit per screen; user 2026-10-18: no performance cap either).
MINIBOSS_CHANCES = (0.08, 0.17, 0.28, 1.0)
import os as _os
_ALL = bool(_os.environ.get('LARECOMP_ALL_MINIBOSSES'))  # (test: as frequency All)
_ONLY_PARAM = _os.environ.get('LARECOMP_ONLY_MINIBOSS_PARAM')  # (test: its first parameter, an int)
_ONLY = _os.environ.get('LARECOMP_ONLY_MINIBOSS')  # (test: hex type - every randomized Field enemy becomes it, any terrain)
# (test, dev seeds: <room>=<hex type>,... - the first randomized land enemy there that may be it becomes it)
_FORCE = {k: int(v, 16) for k, v in (x.split('=') for x in _os.environ.get('LARECOMP_FORCE_ENEMY', '').split(',') if x)}
# Mini-bosses that need what stands with them in their dungeon room (2026-10-12): the arena room and the actor types
# copied from it next to the mini-boss (same offsets, linked both ways as there): Smasher's iron ball (without it the
# game crashes spawning him), Rolling Bones' spike bar, Cue Ball's rail. The runtime wakes them
# (runtime/hle/miniboss.c). Needs enemy_settings['arena'] (room name -> leb.Room of the original game).
MINIBOSS_KITS = {  # (no AreaEventBox: it plays the arena intro - camera, music - which the overworld doesn't want)
    0x5C: ('Lv06FaceShrine_05F', (0x1E4,)),  # Smasher's iron ball
    0x51: ('Lv01TailCave_06G', (0x11A,)),    # Rolling Bones' spike bar
    0x58: ('Lv04AnglersTunnel_03E', ()),     # Cue Ball (its rail)
    0x57: ('Field_13O', ()),                 # Lanmola (his rail)
}


KIT_KEY_TAG = 0x6B  # (copied kit actors get their own keys: the game keeps one actor per key, so copies sharing the
#                    arena's key spawned once among all loaded screens - one Smasher in a load had its ball)


def add_miniboss_kit(room_data, mi, arena, room_name=''):
    """Appends the kit actors of room_data.actors[mi]'s mini-boss (copied from its arena room, same offsets) and links
    them as in the arena; rails (section 2: params, rail, point - a global point index too) of every kit actor are
    copied with their points (a rail-riding mini-boss's rail is then fitted to the room: fit_rail)."""
    import copy
    import struct
    act = room_data.actors[mi]
    src_i = next(i for i, x in enumerate(arena.actors) if x.type == act.type)
    src = arena.actors[src_i]
    take = [src_i] + [i for i, x in enumerate(arena.actors) if x.type in MINIBOSS_KITS[act.type][1]]
    newidx = {src_i: mi}
    import zlib
    for i in take[1:]:
        a = copy.deepcopy(arena.actors[i])
        a.key = (KIT_KEY_TAG << 56) | ((zlib.crc32(room_name.encode()) & 0xFFFFFF) << 32) | \
            ((len(room_data.actors) & 0xFFFF) << 16) | (a.key & 0xFFFF)
        a.name = a.name.split(b'-')[0] + b'-%016X' % a.key
        a.posX, a.posY, a.posZ = act.posX + (a.posX - src.posX), act.posY + (a.posY - src.posY), act.posZ + (a.posZ - src.posZ)
        a.roomID = act.roomID
        room_data.actors.append(a)
        newidx[i] = len(room_data.actors) - 1
    sec = lambda room, name: next(e for e in room.fixed_hash.entries if e.name == name)
    sp, sr, dp, dr = sec(arena, b'point'), sec(arena, b'rail'), sec(room_data, b'point'), sec(room_data, b'rail')
    rail_map, point_map = {}, {}
    for i in take:
        srl = arena.actors[i].relationships
        rl = room_data.actors[newidx[i]].relationships
        rl.section_1 = [[s[0], newidx[s[1]]] for s in srl.section_1 if s[1] in newidx]
        rl.section_3 = [newidx[j] for j in srl.section_3 if j in newidx]
        rails = []
        for s2 in srl.section_2:
            ri = s2[1]
            if ri not in rail_map:
                rail = copy.deepcopy(sr.data.entries[ri])
                d = bytearray(rail.data)
                for k in range(struct.unpack_from('<H', d, 0x2C)[0]):
                    pi = struct.unpack_from('<H', d, 0x30 + 2 * k)[0]
                    if pi not in point_map:
                        pt = copy.deepcopy(sp.data.entries[pi])
                        pd = bytearray(pt.data)
                        x, y, z = struct.unpack_from('<3f', pd, 0)
                        struct.pack_into('<3f', pd, 0, act.posX + (x - src.posX), act.posY + (y - src.posY),
                                         act.posZ + (z - src.posZ))
                        pt.data = bytes(pd)
                        dp.data.entries.append(pt)
                        point_map[pi] = len(dp.data.entries) - 1
                    struct.pack_into('<H', d, 0x30 + 2 * k, point_map[pi])
                for k in range(4):  # (string params point into the names section, which repack rebuilds: ints)
                    struct.pack_into('<II', d, 0xC + 8 * k, 0, 3)
                rail.data = bytes(d)
                dr.data.entries.append(rail)
                rail_map[ri] = len(dr.data.entries) - 1
            rails.append([s2[0], rail_map[ri], point_map.get(s2[2], s2[2])])
        rl.section_2 = rails
        rl.x, rl.y, rl.z = len(rl.section_1), len(rl.section_3), len(rl.section_2)
    if act.type in RAIL_FIT and act.relationships.section_2:
        fit_rail(room_data, act, sec(room_data, b'point'), sec(room_data, b'rail').data.entries[act.relationships.section_2[0][1]])


def remove_actors(room_data, indices):
    """LARecomp: removes room_data.actors[i] for i in indices - with the mini-boss kit actors that came with them
    (add_miniboss_kit: their keys carry KIT_KEY_TAG and link back to their mini-boss) - and renumbers every actor's
    links (relationship sections 1 and 3 are actor indices: a plain pop left them pointing past the list or at the
    wrong actor - a Rolling Bones copy then crashed the game on its screen's load)."""
    gone = set(indices)
    for i, a in enumerate(room_data.actors):
        if a.key >> 56 == KIT_KEY_TAG and any(j in gone for j in a.relationships.section_3):
            gone.add(i)
    new = {}
    for i in range(len(room_data.actors)):
        if i not in gone:
            new[i] = len(new)
    room_data.actors = [a for i, a in enumerate(room_data.actors) if i not in gone]
    for a in room_data.actors:
        rl = a.relationships
        rl.section_1 = [[s[0], new[s[1]]] for s in rl.section_1 if s[1] in new]
        rl.section_3 = [new[j] for j in rl.section_3 if j in new]
        rl.x, rl.y = len(rl.section_1), len(rl.section_3)


RAIL_FIT = {0x58}  # Cue Ball circles a 4-point rail (its arena's 9 x 6 channel): fitted to the room's free tiles


def fit_rail(room_data, act, points, rail):
    """The 4-point rail of `act` becomes the largest rectangle of free tiles (no collision, water, lava) with the actor's
    tile as a corner; the actor moves onto that corner."""
    import struct
    from RandomizerCore.Tools import leb
    entry = next((e for e in room_data.fixed_hash.entries if e.name == b'grid'), None)
    try:
        g = leb.Grid(entry) if entry else None
    except ValueError:
        g = None
    if g is None or g.info.room_height != 8:
        return
    info = g.info

    def free(row, col):
        if not (0 <= row < info.room_height and 0 <= col < info.room_width):
            return False
        t = g.tilesdata[row * info.room_width + col]
        return not (t.flags1['containscollision'] or t.flags1['deepwaterlava'] or t.flags3['iswaterlava'])
    c0, r0 = int((act.posX - info.x_coord) / info.tile_size), int((act.posZ - info.z_coord) / info.tile_size)
    best = None
    for w in range(7, 1, -1):
        for h in range(5, 1, -1):
            for dx, dz in ((1, 1), (1, -1), (-1, 1), (-1, -1)):
                cs, rs = [c0, c0 + dx * w], [r0, r0 + dz * h]
                per = [(rr, cc) for cc in range(min(cs), max(cs) + 1) for rr in rs] + \
                      [(rr, cc) for rr in range(min(rs), max(rs) + 1) for cc in cs]
                if all(free(rr, cc) for rr, cc in per) and (best is None or w * h > best[0]):
                    best = (w * h, w, h, dx, dz)
    if best is None:
        return
    _, w, h, dx, dz = best
    ctr = lambda rr, cc: (info.x_coord + (cc + 0.5) * info.tile_size, info.z_coord + (rr + 0.5) * info.tile_size)
    corners = [ctr(r0, c0), ctr(r0 + dz * h, c0), ctr(r0 + dz * h, c0 + dx * w), ctr(r0, c0 + dx * w)]
    d = bytes(rail.data)
    for k, (x, z) in enumerate(corners[:struct.unpack_from('<H', d, 0x2C)[0]]):
        pi = struct.unpack_from('<H', d, 0x30 + 2 * k)[0]
        pd = bytearray(points.data.entries[pi].data)
        struct.pack_into('<3f', pd, 0, x, act.posY + 0.2, z)
        points.data.entries[pi].data = bytes(pd)
    act.posX, act.posZ = corners[0]



# LARecomp: enemies that another actor watches (relationship section 1 from a non-enemy actor) stay vanilla, as the
# Excluded_Actors do: the kill-all triggers - TagHolocaust (all dead -> its switch: shutters open, a chest / key / stairs
# appear), TagPopChecker (all dead -> the room's switch 1: Angler's Tunnel 04D key chest, Catfish's Maw 03B / 06E,
# Face Shrine 06F, Tal Tal cave Tamaranch04_03B stairs), TagHistoricalHolocaustChecker, Kanalet's crow. The list
# missed 8 such rooms (2026-10-17: Turtle Rock 04D shutter, Eagle's Tower 07B / 08D, the 5 above); this finds them
# all, also the checkers LAS adds itself (golden_leaves.py) since the room is read after those.
# Also kept vanilla, to be safe: every enemy in a room with a shutter (ObjClosedDoor) that no switch drives - its opening
# condition isn't in the room data (Catfish's Maw 05E / 06D, Eagle's Tower 01F / 03E / 04F / 05F / 07E / 08F, Color
# Dungeon 05C: maybe the room's own all-enemies-dead check).
OBJ_CLOSED_DOOR = 0xEF


def kill_linked(room_data, pool):
    out = set()
    if any(a.type == OBJ_CLOSED_DOOR and not any(sw[0] in (1, 2) for sw in a.switches) for a in room_data.actors):
        out.update(i for i, a in enumerate(room_data.actors) if a.type in pool)
    for a in room_data.actors:
        if a.type in pool:
            continue
        for s in a.relationships.section_1:
            if 0 <= s[1] < len(room_data.actors) and room_data.actors[s[1]].type in pool:
                out.add(s[1])
    return out


# (LARecomp: the 'one group per room' and 2D restrictions, shared with tools/rando/enemy_random.py - Random mode)
NO_2D = (0x1E, 0x30, 0x37, 0x38, 0x3F, 0x41, 0x4A, 0x4D, 0x21E, 0x55, 0x5F)  # (the mini-bosses: 3D fighters)
FLY_BOMBS = (0x26, 0x3E, 0x48)  # vires, zirros, bone-putters
BLOCKING = (0x8, 0x9, 0x13, 0x14, 0x2E, 0x2F, 0x30, 0x35, 0x36, 0x41, 0x4D)  # shield/spear, mimics, color dungeon orbs
# The mini-bosses that are also plain enemies (pool: Include mini-bosses on) - Shadow Link, the Ball-and-chain Soldier,
# Blaino: one of them per room at most, and none in a room with a mini-boss or boss of its own (ARENA_TYPES: their
# arenas - none has a randomized land enemy today, kept so if the room data or a generated dungeon ever has one)
ONE_PER_ROOM = (0x21E, 0x55, 0x5F)
ARENA_TYPES = set(range(0x51, 0x71)) | {0xB9}  # (the game's mini-boss and boss actors; 0x55: Kanalet's own Soldier)


def shuffleEnemyActors(room_data, folder: str, file: str, enemy_ids: dict, enemy_settings: dict, rand_state: tuple):
    # LARecomp: miniboss-frequency 0 / 1 / 2 / 3 = normal, 2x, 3x, all (MINIBOSS_CHANCES)
    freq = 3 if _ALL else min(3, max(0, int(enemy_settings.get('miniboss-frequency', 0))))
    miniboss_chance, one_per_room = MINIBOSS_CHANCES[freq], freq < 3
    """"""

    random.setstate(rand_state)
    edited_room = False

    excluded_actors = []
    if file[:-4] in list(ENEMY_DATA['Excluded_Actors'].keys()):
        excluded_actors = list(ENEMY_DATA['Excluded_Actors'][file[:-4]])
    
    restr = list(enemy_ids['restr'])

    # we want to exclude certain enemies from being in 2D rooms
    # some can block your path, others might just be annoying
    if file[:-4] in ENEMY_DATA['2D_Rooms']:
        restr.extend(NO_2D)
    if folder != 'Field' and any(a.type in ARENA_TYPES for a in room_data.actors):  # (LARecomp)
        restr.extend(ONE_PER_ROOM)

    total_ids = (
        *enemy_ids['land'],
        *enemy_ids['air'],
        *enemy_ids['water'],
        *enemy_ids['water2D'],
        *enemy_ids['water_shallow'],
        *enemy_ids['tree'],
        *enemy_ids['hole']
    )
    fly_bombs, blocking = FLY_BOMBS, BLOCKING
    excluded_actors = set(excluded_actors) | kill_linked(room_data, total_ids)  # (LARecomp)
    # LARecomp, enemy mode Random: the actors re-rolled at every load (runtime/hle/enemy_random.c), recorded with what
    # the roll needs (tools/rando/enemy_random.py writes them): exactly those this roll changes, minus mini-bosses
    random_rooms = enemy_settings.get('random')
    room_slots = [] if random_rooms is not None and enemy_settings['types'] else None
    restr_start = list(restr)
    
    room_has_miniboss = False
    placed = []  # (LARecomp: this room's mini-boss spots, x z)
    grid_room = []  # (the original room, for its grid: read once)

    # iterate through each actor
    for e, act in enumerate(room_data.actors):
        if act.type not in total_ids or e in excluded_actors: # check for valid enemy actors
            continue

        if enemy_settings['types']:
            enemy_type = [v for k,v in ENEMY_DATA['Actors'].items() if v['id'] == act.type][0]['type']
            new_enemy = -1
            vanilla = (act.type, list(act.parameters), act.rotY, (act.scaleX, act.scaleY, act.scaleZ),
                       act.relationships.e)
            
            if act.type in restr:
                restr.remove(act.type)
            
            mini = None
            fits = sorted(FIELD_MINIBOSSES) if enemy_type == 'land' else sorted(ANY_TERRAIN_MINIBOSSES)
            if file[:-4] in SMALL_ONLY_SCREENS and folder == 'Field':
                fits = sorted(SMALL_MINIBOSSES) if enemy_type == 'land' or (enemy_type == 'air' and SMALL_ON_AIR) else []
            if _ONLY:
                fits = [int(_ONLY, 16)]
            if folder == 'Field' and enemy_settings.get('arena') and enemy_type == 'land' and not _ONLY:
                if not grid_room:
                    grid_room.append(enemy_settings['arena'](file[:-4]))
                fits = [m for m in fits if room_needed(m) >= 25 or roomy(grid_room[0], act, room_needed(m))]
            if not _ONLY and any((act.posX - x) ** 2 + (act.posZ - z) ** 2 < MB_GAP ** 2 for x, z in placed):
                fits = [m for m in fits if m in SMALL_MINIBOSSES]  # (next to another mini-boss: only the small ones)
            if (enemy_settings.get('minibosses') and fits and folder == 'Field'
                    and (_ONLY or (not (one_per_room and room_has_miniboss) and random.random() < miniboss_chance))):
                mini = random.choice(fits)
                room_has_miniboss = True
                new_enemy = mini

            if mini is None and enemy_type == 'land' and _FORCE.get(file[:-4], -1) not in restr:
                new_enemy = _FORCE[file[:-4]]
            # keep shuffling each actor until it is a valid enemy
            while new_enemy in restr and mini is None:
                if enemy_type == 'land':
                    new_enemy = random.choice(enemy_ids['land'])
                elif enemy_type == 'air':
                    if folder == 'Field':
                        new_enemy = random.choice(enemy_ids['no_vire']) # remove vires from overworld
                    else:
                        new_enemy = random.choice(enemy_ids['air'])
                elif enemy_type == 'water':
                    waters = (*enemy_ids['water'], *enemy_ids['water_shallow'])
                    new_enemy = random.choice(waters)
                elif enemy_type == 'water2D':
                    new_enemy = random.choice(enemy_ids['water2D'])
                elif enemy_type == 'water-shallow':
                    new_enemy = random.choice(enemy_ids['water_shallow'])
                elif enemy_type == 'tree':
                    new_enemy = random.choice(enemy_ids['tree'])
                elif enemy_type == 'hole':
                    new_enemy = random.choice(enemy_ids['hole'])
            
            ### restrict enemy groups to one per room
            if new_enemy in fly_bombs:
                restr.extend(fly_bombs)
            elif new_enemy in blocking:
                restr.extend(blocking)
            elif new_enemy in ONE_PER_ROOM and mini is None:
                restr.extend(ONE_PER_ROOM)
            
            if mini is not None:
                params = random.choice(MINIBOSS_VARIANTS[mini]) if mini in MINIBOSS_VARIANTS else FIELD_MINIBOSSES[mini]
                params = [bytes(p, 'utf-8') if isinstance(p, str) else p for p in params]
                if _ONLY_PARAM:
                    params = [int(_ONLY_PARAM)]
                act.type = mini
                act.parameters = (params + [b''] * 8)[:8]
                act.relationships.e = 1
                act.rotY = 0
                act.scaleX = act.scaleY = act.scaleZ = 1.0
                if mini in MINIBOSS_KITS and enemy_settings.get('arena'):
                    add_miniboss_kit(room_data, e, enemy_settings['arena'](MINIBOSS_KITS[mini][0]), file[:-4])
                placed.append((act.posX, act.posZ))  # (Cue Ball: on its loop's first corner now)
                edited_room = True
                continue

            # change the enemy data into the new enemy
            if act.type != new_enemy:
                act.type = new_enemy
                try:
                    params = [v for k,v in ENEMY_DATA['Actors'].items() if v['id'] == act.type][0]['parameters']
                    for i in range(8):
                        try:
                            if isinstance(params[i], list):
                                param = random.choice(params[i])
                            else:
                                param = params[i]
                            if isinstance(param, str):
                                param = bytes(param, 'utf-8')
                            act.parameters[i] = param
                        except IndexError:
                            act.parameters[i] = b''
                except KeyError:
                    act.parameters = [b'', b'', b'', b'', b'', b'', b'', b'']

                act.relationships.e = int([v for k,v in ENEMY_DATA['Actors'].items() if v['id'] == act.type][0]['enemy'])

                if act.type == 0x4A: # StretchyGhosts - only includes one color in pool so it will be 1/3 likely now
                    act.type = random.choice((0x4A, 0x4B, 0x4C)) # decide color
                elif act.type == 0x4D: # ColorDungeon Orbs - same thing as above
                    act.type = random.choice((0x4D, 0x4E, 0x4F))
                
                act.rotY = 0 # change each enemy to be facing the screen, some will stay sideways if we don't

                # make sure actor is default size so that anything that's normally big won't cause the new enemy to be big
                act.scaleX = 1.0
                act.scaleY = 1.0
                act.scaleZ = 1.0
                edited_room = True

            if room_slots is not None:
                cls = 'air_field' if enemy_type == 'air' and folder == 'Field' else enemy_type
                room_slots.append({'key': act.key, 'cls': cls, 'vanilla': vanilla, 'name_len': len(act.name)})
        
        if enemy_settings['sizes']:
            new_scale = random.uniform(0.5, 1.5)
            act.scaleX = new_scale
            act.scaleY = new_scale
            act.scaleZ = new_scale
            edited_room = True

    if room_slots:  # (the room's file must be in the seed: the runtime re-rolls the seed's copy)
        random_rooms[file[:-4]] = {'restr': restr_start, 'slots': room_slots}
        edited_room = True
    
    return random.getstate(), edited_room
