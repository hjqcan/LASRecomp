"""Per-instruction replacements applied by gen.py (guest address -> function(gen, addr) returning C code).

Widescreen: the game's camera code loads a 16:9 aspect-ratio literal (0x70016afba4) as the default
projection aspect. These loads are redirected to rt_aspect_site(), which returns the aspect for the
current output shape (or 16:9) per site, so only the 3D cameras widen.
"""
import struct
from game_addresses import ADDRESS as A, addr_text

ASPECT_LITERAL = A.ASPECT_LITERAL

# address of `ldr sN, [xM, #0xba4]` -> site id (0x7001023c70 is UI layout math: left alone)
ASPECT_SITES = {
    A.ASPECT_SITES_0: 0,
    A.ASPECT_SITES_1: 1,
    A.ASPECT_SITES_2: 2,
    A.ASPECT_SITES_3: 3,
    A.ASPECT_SITES_4: 4,
    A.ASPECT_SITES_5: 5,
}


def _aspect_load(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert (w & 0xffc00000) == 0xbd400000 and ((w >> 10) & 0xfff) * 4 == 0xba4, hex(w)
    rt = w & 31
    return (f'{{ extern float rt_aspect_site(int); float f = rt_aspect_site({ASPECT_SITES[a]}); '
            f'c->v[{rt}] = 0; memcpy(&c->v[{rt}], &f, 4); }}  /* {a:#x}: ldr s{rt}, =16/9 */')


PATCHES = {a: _aspect_load for a in ASPECT_SITES}


# Off-screen culling: the scene's visibility pass loads the camera's aspect from [x27, #0x570] into s9
# (`ldr s9, [x27, #0x570]` at 0x7000852ad4). The community 32:9 patch (Jaddey / KeatonTheBot, build
# AE16F71E...) forces s9 here; we load the runtime aspect instead (site 6).
CULL_ASPECT = A.CULL_ASPECT


def _cull_aspect(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xbd457369, hex(w)  # ldr s9, [x27, #0x570]
    return ('{ extern float rt_cull_aspect(float); float o; memcpy(&o, (const void *)(uintptr_t)(c->x[27] + 0x570), 4); '
            'float f = rt_cull_aspect(o); c->v[9] = 0; memcpy(&c->v[9], &f, 4); }'
            f'  /* {a:#x}: ldr s9, [x27, #0x570] (culling aspect) */')


PATCHES[CULL_ASPECT] = _cull_aspect


# Vertical field of view (camera field +0x564): taller-than-16:9 outputs keep the designed horizontal view
# and widen the vertical one ("Vert+"). Loaded by the main camera and by the culling pass.
FOVY_SITES = {A.FOVY_SITES_0: (0xbd4566e0, 23), A.FOVY_SITES_1: (0xbd456768, 27)}


def _fovy_load(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    want, rn = FOVY_SITES[a]
    assert w == want, hex(w)
    rt = w & 31
    return ('{ extern float rt_fovy(float); float o; '
            f'memcpy(&o, (const void *)(uintptr_t)(c->x[{rn}] + 0x564), 4); float f = rt_fovy(o); '
            f'c->v[{rt}] = 0; memcpy(&c->v[{rt}], &f, 4); }}  /* {a:#x}: ldr s{rt}, [x{rn}, #0x564] (fovy) */')


for _a in FOVY_SITES:
    PATCHES[_a] = _fovy_load


# Sky and sea beyond the overworld's north edge (MapSkySea, map/SkySea.bfres): pieces 60 apart, each drawn only while
# Link stands inside its box, the piece's centre +-(67.5, 50, 27) - its own reach plus half a 16:9 view. With a wider
# view the next piece is needed sooner: at 32:9 the screen beside Link's showed black (no sky, no sea) until Link came
# within 67.5. The box's across bounds are widened by the extra view (runtime/hle/widescreen.c rt_skysea_margin).
SKYSEA_BOX_SITES = {A.SKYSEA_BOX_MAX_SITE: (0x3dc0bae1, 0x2e0, 1.0), A.SKYSEA_BOX_MIN_SITE: (0x3dc0b6e2, 0x2d0, -1.0)}


def _skysea_box(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    want, off, sign = SKYSEA_BOX_SITES[a]
    assert w == want, hex(w)  # ldr qN, [x23, #off]
    rt = w & 31
    return ('{ extern float rt_skysea_margin(void); float b[4]; '
            f'memcpy(b, (const void *)(uintptr_t)(c->x[23] + {off:#x}), 16); b[0] += {sign}f * rt_skysea_margin(); '
            f'memcpy(&c->v[{rt}], b, 16); }}  /* {a:#x}: ldr q{rt}, [x23, #{off:#x}] (sky / sea piece box) */')


for _a in SKYSEA_BOX_SITES:
    PATCHES[_a] = _skysea_box


# Indoor detection: 0x7001023c70 (`ldr s1, =16/9`) is in the indoor shadow setup (community patch notes:
# "Indoor Shadow AR"). It keeps loading 16:9 and tells the runtime an interior is being rendered.
INDOOR_SITE = A.INDOOR_SITE


def _indoor(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert (w & 0xffc00000) == 0xbd400000 and ((w >> 10) & 0xfff) * 4 == 0xba4, hex(w)
    rt = w & 31
    return ('{ extern float rt_indoor_aspect(void); float f = rt_indoor_aspect(); '
            f'c->v[{rt}] = 0; memcpy(&c->v[{rt}], &f, 4); }}  /* {a:#x}: ldr s{rt}, =16/9 (indoor shadow) */')


PATCHES[INDOOR_SITE] = _indoor


# Warp support: the level-jump request object (filled by GameControl.RequestLevelJump via main+0x81b950)
# registers itself in its parent while a level loads: `str xR, [x8, #0x1c0]` at these two sites (two scene
# kinds). Record it so runtime/hle/warp.c can fill the request itself.
WARP_REQUEST_SITES = {A.WARP_REQUEST_SITES_0: 20, A.WARP_REQUEST_SITES_1: 19}


def _warp_request(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    rt = WARP_REQUEST_SITES[a]
    assert w == (0xf900e100 | rt), hex(w)  # str xRT, [x8, #0x1c0]
    return ('{ extern void rt_warp_request_obj(uint64_t); '
            f'*(uint64_t *)(uintptr_t)(c->x[8] + 0x1c0) = c->x[{rt}]; rt_warp_request_obj(c->x[{rt}]); }}'
            f'  /* {a:#x}: str x{rt}, [x8, #0x1c0] (warp: level-jump request) */')


for _a in WARP_REQUEST_SITES:
    PATCHES[_a] = _warp_request

# Blaino's knock-out: Link flies up and a countdown (main+0xd97df0, +0xb8) starts MightPunchCrashOut - fade, then a
# restart warp to his ring's start. On the Field there's no ring: no event (runtime/hle/miniboss.c), Link falls back.
CRASHOUT_SITE = A.CRASHOUT_SITE



# Gohma on the Field (seeds): its moves walk / charge along a track - a heading of +-90 degrees (+0x15ec / +0x15f0,
# the shared movement controller at +0x15c0 turns to it and the mover moves it, walls and all) toward a track end,
# snapping onto the end when there (main+0x399550). The Field has no track: the heading points at Link and there is
# no end - each move runs until its own timer (runtime/hle/miniboss.c).
GOHMA_HEADING_SITES = (A.GOHMA_HEADING_SITES_0, A.GOHMA_HEADING_SITES_1)  # str w8, [x19, #0x15ec] (then the same to +0x15f0)
GOHMA_ARRIVED_SITE = A.GOHMA_ARRIVED_SITE  # ldr q0, [x19, #0x1600] (the track end, against Gohma's spot)


# Master Stalfos' body collision (CharaBodyCollision, main+0x1841c0 from his constructor) blocks Link - in his arena,
# fine; on the Field Link couldn't damage-boost past him as past the other mini-bosses. Its flag (x6 -> sp+0x328) off
# there (seeds, level Field).
MSTALFOS_BODY_SITE = A.MSTALFOS_BODY_SITE  # mov w8, #1



# Master Stalfos' jump (state 4 exec) sets its vertical speed to 6 * sin(phase) - height: an arc from his arena's floor
# (height 0). On the Field the ground is higher - he dropped through it. There the arc starts from his takeoff height
# (his movement component [+0x1160] + 0x28c, stored as the jump starts); in the arena that is 0 anyway.
MSTALFOS_JUMP_SITE = A.MSTALFOS_JUMP_SITE  # fsub s0, s0, s1



# Moblin Chief deletes himself in his init (main+0x1f8af0) unless a story bit is set (player status +0xac bit 7 - his
# hideout's time); on the Field (seeds) that check is skipped. The defeated check before it stays.
MCHIEF_STORY_SITE = A.MCHIEF_STORY_SITE  # tbnz w8, #7, 0x70001f8b58


# Generated dungeons (runtime/hle/chamber.c): every level jump goes through the request processor main+0x81ba80
# (x0 = request); its prologue reports the request so jumps into Tail Cave can be redirected.
LEVEL_REQUEST_SITE = A.LEVEL_REQUEST_SITE


def _level_request(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xd10203ff, hex(w)  # sub sp, sp, #0x80
    return ('{ extern void rt_level_request(Cpu *); rt_level_request(c); c->sp -= 0x80; }'
            f'  /* {a:#x}: sub sp, sp, #0x80 (level-jump request) */')


PATCHES[LEVEL_REQUEST_SITE] = _level_request


# Chamber staircase exit prompt ("Return to Arranging/List" vs "Back to Adventuring"): main+0x7f1540 opens it with
# `bl main+0xf05280` at 0x7f15e0; the callback only stores the chosen option at [main+0x1cb9fb8]+0x18f8 (0 = leave).
# In a generated dungeon the dialog is skipped and 0 stored, so the staircase leaves directly (runtime/hle/chamber.c).
ADV_EXIT_DIALOG_SITE = A.ADV_EXIT_DIALOG_SITE


def _adv_exit_dialog(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x9402_5d28 or (w >> 26) == 0x25, hex(w)  # bl
    return addr_text('{ extern int rt_adv_exit_choice(void); extern void f_@ADV_EXIT_DIALOG_TARGET@(Cpu *); '
            'if (!rt_adv_exit_choice()) { c->x[30] = 0x@ADV_EXIT_DIALOG_RETURN@ull; f_@ADV_EXIT_DIALOG_TARGET@(c); } }'
            f'  /* {a:#x}: bl 0xf05280 (chamber exit prompt) */')


PATCHES[ADV_EXIT_DIALOG_SITE] = _adv_exit_dialog


# Difficulty (runtime/ui/difficulty.mm): Link's damage is computed as `damage << hero` (Hero Mode doubles it;
# hero = PlayerStatus+0x120 == 1) at these `lsl Wd, Wn, Wm` sites, then subtracted from health (+0x84).
# Each is redirected to rt_damage(damage, shift) so the difficulty multiplier / one-hit KO applies.
DAMAGE_LSL_SITES = (A.DAMAGE_LSL_SITES_0, A.DAMAGE_LSL_SITES_1, A.DAMAGE_LSL_SITES_2)
# Fixed damage (-2 quarter hearts, -4 in Hero Mode: `csel w20, w10, w9, eq` with w10=-4, w9=-2).
DAMAGE_FIXED_SITE = A.DAMAGE_FIXED_SITE


def _damage_lsl(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert (w & 0xffe0fc00) == 0x1ac02000, hex(w)  # lslv (32-bit)
    rd, rn, rm = w & 31, (w >> 5) & 31, (w >> 16) & 31
    return (f'{{ extern int32_t rt_damage(int32_t, int32_t); c->x[{rd}] = (uint32_t)rt_damage((int32_t)c->x[{rn}], '
            f'(int32_t)(c->x[{rm}] & 31)); }}  /* {a:#x}: lsl w{rd}, w{rn}, w{rm} (damage) */')


def _damage_fixed(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert (w & 0xffe00c00) == 0x1a800000 and (w & 31) == 20, hex(w)  # csel w20, ...
    return ('{ extern int32_t rt_damage(int32_t, int32_t); extern int rt_hero_mode(void); '
            'c->x[20] = (uint32_t)(-rt_damage(2, rt_hero_mode())); }'
            f'  /* {a:#x}: csel w20, -4, -2 (fixed damage) */')


for _a in DAMAGE_LSL_SITES:
    PATCHES[_a] = _damage_lsl
PATCHES[DAMAGE_FIXED_SITE] = _damage_fixed


# Items page description ("UI:ItemExplanation_{n}"): main+0xeb6604.. builds the item name then looks the explanation
# message up (bl 0x903250 -> [sp+0xf0]); a null result takes the no-text path (buffer already cleared). In randomizer
# seeds the explanation is hidden (it overlaps the key / trade-item rows of the inventory overlay; player knows).
ITEM_EXPLANATION_SITE = A.ITEM_EXPLANATION_SITE


def _item_explanation(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xf9407bf6, hex(w)  # ldr x22, [sp, #0xf0]
    return ('{ extern int rt_hide_item_explanation(void); '
            'c->x[22] = rt_hide_item_explanation() ? 0 : LD64(c->sp + 0xf0); }'
            f'  /* {a:#x}: ldr x22, [sp, #0xf0] (item explanation message) */')


PATCHES[ITEM_EXPLANATION_SITE] = _item_explanation

# ... and the item NAME ("UI:ItemName_{0}"): main+0xeb6658 `bl 0xf8c8b0` sets the name pane from that message; in
# randomizer seeds it is skipped too (user 2026-10-08: the name showed under the key rows of the inventory overlay).
ITEM_NAME_SITE = A.ITEM_NAME_SITE


def _item_name(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x94035896, hex(w)  # bl 0x7000f8c8b0
    return addr_text('c->x[30] = 0x@ITEM_NAME_RETURN@ull; { extern int rt_hide_item_explanation(void); '
            'if (!rt_hide_item_explanation()) f_@ITEM_NAME_TARGET@(c); }'
            f'  /* {a:#x}: bl 0xf8c8b0 (set the item name pane) */')


PATCHES[ITEM_NAME_SITE] = _item_name


# Keysanity (runtime/hle/keysanity.c): AddItem (main+0x7e18b0, x0 item / x1 count / x2 index) only adds dungeon
# items to the current dungeon; with an explicit dungeon index the hook updates that dungeon's save record instead.
ADD_ITEM_SITE = A.ADD_ITEM_SITE  # inner add (34 callers: events, chests, drops); AddItem 0x7e18b0 wraps it


def _add_item(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xd10143ff, hex(w)  # sub sp, sp, #0x50
    return ('{ extern int rt_add_item(Cpu *); if (rt_add_item(c)) return; c->sp -= 0x50; }'
            f'  /* {a:#x}: sub sp, sp, #0x50 (AddItem: keysanity) */')


PATCHES[ADD_ITEM_SITE] = _add_item


# Trendsanity (runtime/hle/shopsanity.c): AddPrizeByKey (main+0x7e1c70) looked the prize row up (x0) and is about to
# call AddPrize(row, count, index) (main+0x1489c0, rows 0-20 only): a randomized prize row sets its check's flags.
PRIZE_ADD_SITE = A.PRIZE_ADD_SITE


def _prize_add(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x2a1403e2, hex(w)  # mov w2, w20
    return ('{ c->x[2] = (uint32_t)c->x[20]; extern void rt_add_prize(Cpu *); rt_add_prize(c); }'
            f'  /* {a:#x}: mov w2, w20 (AddPrize index; trendsanity) */')


PATCHES[PRIZE_ADD_SITE] = _prize_add


# Inventory overlay (runtime/ui/inventory_overlay.mm): main+0xeb7cd0 runs once per frame only while the pause
# menu's Items page is shown (found by profiling Items vs Map/System pages). Its prologue reports that.
ITEMS_PAGE_SITE = A.ITEMS_PAGE_SITE


def _items_page(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xd10243ff, hex(w)  # sub sp, sp, #0x90
    return ('{ extern void rt_items_page_tick(void); rt_items_page_tick(); c->sp -= 0x90; }'
            f'  /* {a:#x}: sub sp, sp, #0x90 (items page shown) */')


PATCHES[ITEMS_PAGE_SITE] = _items_page


# Check tracker (runtime/ui/tracker_overlay.mm): main+0xe93800 runs once per frame only while the pause menu's
# Map page is shown (fnprof diff of Map vs Items/System/closed). Its prologue reports that.
MAP_PAGE_SITE = A.MAP_PAGE_SITE


def _map_page(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xd10203ff, hex(w)  # sub sp, sp, #0x80
    return ('{ extern void rt_map_page_tick(Cpu *); rt_map_page_tick(c); c->sp -= 0x80; }'
            f'  /* {a:#x}: sub sp, sp, #0x80 (map page shown) */')


PATCHES[MAP_PAGE_SITE] = _map_page


# ... and its epilogue (the page's layout was updated this frame): the check tracker hides the game's own cursor and
# button bar there (tracker_overlay.mm map_page_takeover), after the update that may have shown them again.
MAP_PAGE_END_SITE = A.MAP_PAGE_END_SITE


def _map_page_end(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x910203ff, hex(w)  # add sp, sp, #0x80
    return ('{ extern void rt_map_page_end(Cpu *); rt_map_page_end(c); c->sp += 0x80; }'
            f'  /* {a:#x}: add sp, sp, #0x80 (map page updated) */')


PATCHES[MAP_PAGE_END_SITE] = _map_page_end


# Chaos map letters (runtime/ui/chaos_overlay.mm): main+0xe78740 runs once per frame while the DUNGEON map page is
# shown (fnprof: map open vs closed in Face Shrine); x0 = the dungeon map page.
DGN_MAP_SITE = A.DGN_MAP_SITE


def _dgn_map(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xd10243ff, hex(w)  # sub sp, sp, #0x90
    return ('{ extern void rt_dgnmap_tick(Cpu *); rt_dgnmap_tick(c); c->sp -= 0x90; }'
            f'  /* {a:#x}: sub sp, sp, #0x90 (dungeon map page shown) */')


PATCHES[DGN_MAP_SITE] = _dgn_map


# ... and its epilogue: the check tracker hides the dungeon map page's own cursor and button bar there
# (tracker_overlay.mm), after the update that may have shown them again.
DGN_MAP_END_SITE = A.DGN_MAP_END_SITE


def _dgn_map_end(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x910243ff, hex(w)  # add sp, sp, #0x90
    return ('{ extern void rt_dgnmap_end(Cpu *); rt_dgnmap_end(c); c->sp += 0x90; }'
            f'  /* {a:#x}: add sp, sp, #0x90 (dungeon map page updated) */')


PATCHES[DGN_MAP_END_SITE] = _dgn_map_end


# Debug (LARECOMP_LOG_RESNAME=1): main+0x10def30 builds a resource handle from a name (x2 -> {char *name, ...})
# and looks it up; the generated-dungeon map crash dereferences a handle whose resource wasn't found.
RES_NAME_SITE = A.RES_NAME_SITE


def _res_name(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xd10503ff, hex(w)  # sub sp, sp, #0x140
    return ('{ extern void rt_log_resname(uint64_t); rt_log_resname(c->x[2]); c->sp -= 0x140; }'
            f'  /* {a:#x}: sub sp, sp, #0x140 (resource-by-name debug log) */')


PATCHES[RES_NAME_SITE] = _res_name


# Field mini-bosses (enemy randomizer, runtime/hle/miniboss.c): a mini-boss waits in its idle state until its dungeon
# room's start event runs, which never happens in the Field. f_70000ec310 (an actor component's per-frame update)
# asks its actor a type question through vtable +0x68 every frame: the runtime sees each actor there, picks out the
# mini-boss classes by vtable and moves them into their fight state itself.
MB_SEEN_SITE = A.MB_SEEN_SITE




# Frame pacing (runtime/hle/framepace.c): the game's frame timer (f_700000c910) advances by its frame period times
# the swap interval it reads from the system object (+0x812c, written only by the interval setter f_7000017800). It
# reads it here and nowhere else: we hand it the number of vsyncs that really passed since the last frame, so a
# late frame advances the world by the time that passed (no slow motion below 60 fps).
FRAME_STEPS_SITE = A.FRAME_STEPS_SITE


def _frame_steps(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xb8696908, hex(w)  # ldr w8, [x8, x9]
    return ('{ extern uint32_t rt_frame_steps(uint32_t interval); '
            'c->x[8] = rt_frame_steps(LD32(c->x[8] + c->x[9])); }'
            f'  /* {a:#x}: ldr w8, [x8, x9] (swap interval -> vsyncs elapsed: runtime/hle/framepace.c) */')


PATCHES[FRAME_STEPS_SITE] = _frame_steps


# The game's own dynamic resolution, off: resolution is ours to choose (runtime/nvn/output_scale.c - a fixed choice
# renders at exactly that size, "Dynamic" is our scaler alone). The game sizes its main pass by a level (renderer
# +0x6e4c, docked): 1080 - 36 x level lines at 16:9 (f_7001023c10), level 0 = 1920x1080, 9 = 1344x756, 10 = 1280x720.
# Its scaler (the object at +0x648: enabled byte, table indexes, the level tables - docked [0 3 6 9 10]) is set up at
# boot (f_700107cb90) at the first table level >= the boot default (8: so 9, 1344x756). Then each frame its frame
# controller (f_700000c910) turns its GPU timing counters (renderer+0x81c0) into a load figure (GPU time x 100 /
# (16.67 ms x swap interval)); 6 frames all over 90%: one level down the table (f_700107d110), 6 all under 70%: up.
# GAME_DYNRES_SITE: the enabled byte reads as 0 - no samples, no steps. GAME_DYNRES_INIT_SITE: the set-up reads the
# boot default as 0 - it starts (and so stays) at level 0, full size. The rest of the frame controller - the 30 fps
# mode, which reads the same counters - runs as before. LARECOMP_GAME_DYNRES=1 (test): the game's own again.
GAME_DYNRES_SITE = A.GAME_DYNRES_SITE
GAME_DYNRES_INIT_SITE = A.GAME_DYNRES_INIT_SITE


def _game_dynres(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x395922e8, hex(w)  # ldrb w8, [x23, #0x648]
    return ('{ extern int rt_game_dynres(void); '
            'c->x[8] = rt_game_dynres() ? LD8(c->x[23] + 0x648) : 0; }'
            f'  /* {a:#x}: ldrb w8, [x23, #0x648] (the game\'s dynamic resolution: off) */')


def _game_dynres_init(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xb86d698c, hex(w)  # ldr w12, [x12, x13] (x13 = 0x6e4c)
    return ('{ extern int rt_game_dynres(void); '
            'c->x[12] = rt_game_dynres() ? LD32(c->x[12] + c->x[13]) : 0; }'
            f'  /* {a:#x}: ldr w12, [x12, x13] (the game\'s starting resolution level: full size) */')


PATCHES[GAME_DYNRES_SITE] = _game_dynres
PATCHES[GAME_DYNRES_INIT_SITE] = _game_dynres_init


# Digsanity (runtime/hle/digs.c): the shovel's dig commit (f_7000ddc530) spawns the hole actor (ObjHolesDug 0x1cd)
# through f_7000d228d0(&out, transform x21, zone, &room, ...); right after it the hole (out at [sp+0x10]) and its
# snapped tile-centre position (x21) tell the runtime which tile was dug - the first dig of a tile gives its item.
DIG_HOLE_SITE = A.DIG_HOLE_SITE



# The game keeps its dug holes in a list (main+0x1cbae80, count at +0x10) and fills in the oldest ones once there are
# more than 80 (f_7000d22d10: while count > 0x50). Digsanity keeps holes open: the limit comes from the runtime.
HOLE_CAP_SITES = (A.HOLE_CAP_SITES_0, A.HOLE_CAP_SITES_1)  # cmp x8, #0x51 / cmp x8, #0x50


# Escape Bottle (runtime/hle/escape.c): the player's per-item "use it now?" predicates (one per item slot, main+0xd50e60
# .. 0xd52f00; each: X / Y newly pressed with that slot equipped on it) only run while Link may use items (no cutscene,
# dialog, menu or transition). main+0xd50e60 is the third bottle's (slot 0xf): owned (PlayerStatus+0xa9 bit 7) and
# empty (+0xc0 bit 2 clear) -> `cset w0, eq` = yes, and the bottle swing starts. In randomizer seeds that bottle is the
# Escape Bottle: the runtime warps instead and the answer is no.
BOTTLE3_USE_SITE = A.BOTTLE3_USE_SITE


def _bottle3_use(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x1a9f17e0, hex(w)  # cset w0, eq
    return ('{ extern int rt_bottle3_use(void); c->x[0] = COND_NE(c) ? 0 : 1; if (c->x[0] && rt_bottle3_use()) c->x[0] = 0; }'
            f'  /* {a:#x}: cset w0, eq (third bottle used: Escape Bottle) */')


PATCHES[BOTTLE3_USE_SITE] = _bottle3_use


# Hint Book (runtime/hle/hintbook.c): the same predicate of the FIRST bottle (slot 0xd, main+0xd510c0: owned =
# PlayerStatus+0xa9 bit 5, empty = +0xc0 bit 0). In randomizer seeds that bottle is the Hint Book: used, the runtime
# shows the next hint in the game's message box and the answer is no (no swing).
BOTTLE1_USE_SITE = A.BOTTLE1_USE_SITE


def _bottle1_use(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x1a9f17e0, hex(w)  # cset w0, eq
    return ('{ extern int rt_bottle1_use(void); c->x[0] = COND_NE(c) ? 0 : 1; if (c->x[0] && rt_bottle1_use()) c->x[0] = 0; }'
            f'  /* {a:#x}: cset w0, eq (first bottle used: Hint Book) */')


PATCHES[BOTTLE1_USE_SITE] = _bottle1_use


# Boss souls (runtime/hle/souls.c): the actor factory f_70008f5ea0 (x2 = spawn info, type u16 at +8) takes the type's
# create function from the actor table (main+0x1bcc898, 0x58 bytes per type, +0 create fn, +0x50 name); a null fn is
# its "no such actor" path (the caller gets an empty handle). A dungeon boss whose soul isn't found yet gets that path.
BOSS_SPAWN_SITE = A.BOSS_SPAWN_SITE


# Enemy randomizer, mode Random (runtime/hle/enemy_random.c): the same spot first lets the runtime re-roll a randomized
# enemy in the spawn info (type +8, the eight parameters +0x10, as in its room file's record), then takes the type
# again (x23) and its table entry (x8 = x27 + type * 0x58, x27 = the table: main+0x1bcc898).
def _boss_spawn(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xf9400118, hex(w)  # ldr x24, [x8]
    if a - 4 >= gen.base:  # (the address table test gives this one word only)
        w = struct.unpack_from('<I', gen.nso.image, a - 4 - gen.base)[0]
        assert w == 0x9b086ee8, hex(w)  # madd x8, x23, x8, x27
    return ('{ extern uint64_t rt_actor_create_fn(uint64_t fn, uint32_t type, uint64_t info); '
            'extern void rt_enemy_spawn(uint64_t info); rt_enemy_spawn(c->x[2]); '
            'c->x[23] = LD16(c->x[2] + 8); c->x[8] = c->x[27] + c->x[23] * 0x58ull; '
            'c->x[24] = rt_actor_create_fn(LD64(c->x[8]), (uint32_t)c->x[23], c->x[2]); }'
            f'  /* {a:#x}: ldr x24, [x8] (actor create fn: boss souls, enemy mode Random) */')


PATCHES[BOSS_SPAWN_SITE] = _boss_spawn


# Hookshot anything (runtime/hle/hookshot.c): the hookshot's Shot update (main+0xd7ecc0) loads the attribute word of
# the surface its ray hit; bit 0x4000 (hookshot target) makes it latch (Adhere) instead of bouncing off (Return).
HOOKSHOT_ATTR_SITE = A.HOOKSHOT_ATTR_SITE


def _hookshot_attr(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xb9403bf7, hex(w)  # ldr w23, [sp, #0x38]
    return ('{ extern uint32_t rt_hookshot_attr(Cpu *, uint32_t); '
            'c->x[23] = rt_hookshot_attr(c, LD32(c->sp + 0x38)); }'
            f'  /* {a:#x}: ldr w23, [sp, #0x38] (hookshot hit attribute) */')


PATCHES[HOOKSHOT_ATTR_SITE] = _hookshot_attr


# 360-degree movement (runtime/hle/qol.c, setting move_360): main+0xded9f0 snaps Link's stick vector (s1 = x, s2 = y)
# to one of 8 directions and returns it as a 32-bit angle (full turn = 2^32, 0 = down). With the setting on, the
# runtime returns the stick's own angle instead (as the community "360 Controls" mod does).
SNAP_DIR_SITE = A.SNAP_DIR_SITE


def _snap_dir(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xd10103ff, hex(w)  # sub sp, sp, #0x40
    return ('{ extern int rt_snap_dir(Cpu *); if (rt_snap_dir(c)) return; c->sp -= 0x40; }'
            f'  /* {a:#x}: sub sp, sp, #0x40 (8-way stick snap: 360-degree movement) */')


PATCHES[SNAP_DIR_SITE] = _snap_dir


# Walk speed (runtime/hle/qol.c, setting walk_speed): Link's walk state (main+0xdcdae0) and shield walk (main+0xdb7d30)
# put a speed multiplier in s0 right before these loads - 1.0, or 1.15 with a Piece of Power - and scale the ramped
# walk speed by it (then by the slow-ground factors 0.25 / 0.75). rt_walk_speed multiplies s0 by the setting. The
# Pegasus Boots dash, swimming and side-view ladders have their own speeds and are untouched, and so is the hop off a
# ledge (the same multiplier loaded again at main+0xdce508 / 0xdd0254 / 0xdb86e8 / 0xdb8f3c: left alone, so jump
# distances stay the game's own).
WALK_SPEED_SITES = {A.WALK_SPEED_SITES_0: (19, 0), A.WALK_SPEED_SITES_1: (19, 1), A.WALK_SPEED_SITES_2: (20, 2),
                    A.WALK_SPEED_SITES_3: (20, 3)}


def _walk_speed(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    rn, site = WALK_SPEED_SITES[a]
    assert w == 0xf94ca408 | (rn << 5), hex(w)  # ldr x8, [x19|x20, #0x1948]
    return ('{ extern float rt_walk_speed(uint64_t, float, int); float m_; memcpy(&m_, &c->v[0], 4); '
            f'm_ = rt_walk_speed(c->x[{rn}], m_, {site}); c->v[0] = 0; memcpy(&c->v[0], &m_, 4); '
            f'c->x[8] = LD64(c->x[{rn}] + 0x1948); }}'
            f'  /* {a:#x}: ldr x8, [x{rn}, #0x1948] (walk speed multiplier in s0) */')


for _a in WALK_SPEED_SITES:
    PATCHES[_a] = _walk_speed


# Walk animation rate: main+0xdce320 (walk state) has the walk animation's play rate in s8 (1, 0.75 on slow ground,
# x1.15 with a Piece of Power), handed to the animation layers right after.
def _walk_anim(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xf94ab668, hex(w)  # ldr x8, [x19, #0x1568]
    return ('{ extern float rt_walk_speed(uint64_t, float, int); float m_; memcpy(&m_, &c->v[8], 4); '
            'm_ = rt_walk_speed(c->x[19], m_, 100); c->v[8] = 0; memcpy(&c->v[8], &m_, 4); '
            'c->x[8] = LD64(c->x[19] + 0x1568); }'
            f'  /* {a:#x}: ldr x8, [x19, #0x1568] (walk animation rate in s8) */')


PATCHES[A.WALK_ANIM_SITE_0] = _walk_anim


# The same for the shield walk (main+0xdb8168): s0 = 1 or 0.75 on slow ground (times s1 = 1.15 with a Piece of Power
# right after).
def _shield_anim(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xf94006c8, hex(w)  # ldr x8, [x22, #8]
    return ('{ extern float rt_walk_speed(uint64_t, float, int); float m_; memcpy(&m_, &c->v[0], 4); '
            'm_ = rt_walk_speed(c->x[20], m_, 101); c->v[0] = 0; memcpy(&c->v[0], &m_, 4); '
            'c->x[8] = LD64(c->x[22] + 8); }'
            f'  /* {a:#x}: ldr x8, [x22, #8] (shield walk animation rate in s0) */')


PATCHES[A.WALK_ANIM_SITE_1] = _shield_anim


# No Guardian Acorns / Pieces of Power (randomizer no-buff-drops, runtime/hle/qol.c): the drop roll main+0x89b960 reads
# its ItemDrop row's mAllowBuffReplace (+0x38); set, the kill counters may turn the drop into a Piece of Power / Guardian
# Acorn. Read as false for such seeds.
BUFF_REPLACE_SITE = A.BUFF_REPLACE_SITE


def _buff_replace(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x3940e308, hex(w)  # ldrb w8, [x24, #0x38]
    return ('{ extern int rt_buff_replace(int); c->x[8] = (uint32_t)rt_buff_replace(LD8(c->x[24] + 0x38)); }'
            f'  /* {a:#x}: ldrb w8, [x24, #0x38] (mAllowBuffReplace: no-buff-drops) */')


PATCHES[BUFF_REPLACE_SITE] = _buff_replace


# Blur removal (runtime/hle/qol.c, setting blur_removal): main+0x1071220 sets up the post-process's tilt-shift object
# (x0) once, looking its shader parameters up by name; qol.c keeps the object and takes the parameter handle out / puts
# it back live.
TILT_INIT_SITE = A.TILT_INIT_SITE


def _tilt_init(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xd10243ff, hex(w)  # sub sp, sp, #0x90
    return ('{ extern void rt_tilt_obj(uint64_t); rt_tilt_obj(c->x[0]); c->sp -= 0x90; }'
            f'  /* {a:#x}: sub sp, sp, #0x90 (tilt-shift set-up: blur removal) */')


PATCHES[TILT_INIT_SITE] = _tilt_init


# Event parameters: main+0x109fb80 looks up a String parameter of an event action and throws when it's missing
# ("String 型の '{0}' というパラメータが見つかりません"). Two of the game's timelines leave an AimPlayer / AimActor
# 'boneName' out (tl/WindFishSong: learning Marin's song; tl/HippoModelSlap); on the Switch the throw is caught up in
# the event system, but recompiled code can't unwind (crash). A missing String reads as "" instead - what the other
# events pass for boneName (tl/ManboTamegoro's PlayOneshotSE has no 'label': no sound).
MISSING_STRING_SITE = A.MISSING_STRING_SITE


def _missing_string(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x321c03e0, hex(w)  # mov w0, #0x10 (orr w0, wzr, #0x10: __cxa_allocate_exception size)
    # "" (the terminator of ".ptcl" right before the message; callers strlen() the result, so not NULL)
    return addr_text('{ ST64(c->sp + 0x18, 0x@EMPTY_STRING@ull); ST32(c->sp + 0x20, 0); goto L_@MISSING_STRING_RETURN@; }'
            f'  /* {a:#x}: mov w0, #0x10 (missing String event parameter: return "", no throw) */')


PATCHES[MISSING_STRING_SITE] = _missing_string

# The Float version (main+0x109fad0, "Float 型の '{0}' というパラメータが見つかりません"): a randomizer item on an
# instrument pedestal (any non-instrument) runs an action whose Float 'time' is missing - picking it up crashed
# (bug report 2026-10-11, reproduced on rando/DevInstr1 Tail Cave). A missing Float reads as 0.0 instead: the reader
# (0x7f3c90) treats time <= 0 as instant.
MISSING_FLOAT_SITE = A.MISSING_FLOAT_SITE


def _missing_float(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x321c03e0, hex(w)  # mov w0, #0x10 (__cxa_allocate_exception size)
    # s0 = 0.0 (q0 at Cpu+512), then the function's own epilogue
    return ('{ __builtin_memset((char *)c + 512, 0, 16);'
            ' c->x[29] = LD64(c->sp + 48); c->x[30] = LD64(c->sp + 56);'
            ' c->x[20] = LD64(c->sp + 32); c->x[19] = LD64(c->sp + 40); c->sp += 0x40; return; }'
            f'  /* {a:#x}: mov w0, #0x10 (missing Float event parameter: return 0.0, no throw) */')


PATCHES[MISSING_FLOAT_SITE] = _missing_float


# Treat Tunics as Upgrades (runtime/hle/qol.c rt_tunic_upgrades): the add-item code (main+0x8d08e0) sets a tunic's
# bit in the tunics byte (PlayerStatus +0x9c = save +0x555C: 0x40 red, 0x80 blue), then clears the other one; with
# the seed option the clear is skipped, so both tunics (and both effects) are kept.
TUNIC_CLEAR_SITES = {
    A.TUNIC_CLEAR_SITES_0: (0x12197929, 0xffffffbf, 'and w9, w9, #0xffffffbf (blue tunic: red cleared)'),
    A.TUNIC_CLEAR_SITES_1: (0x12001929, 0x7f, 'and w9, w9, #0x7f (red tunic: blue cleared)'),
}


def _tunic_clear(gen, a):
    word, mask, what = TUNIC_CLEAR_SITES[a]
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == word, hex(w)
    return ('{ extern int rt_tunic_upgrades(void); '
            f'if (!rt_tunic_upgrades()) c->x[9] = (uint32_t)((uint32_t)c->x[9] & {mask:#x}u); }}'
            f'  /* {a:#x}: {what} - kept with Treat Tunics as Upgrades */')


for _a in TUNIC_CLEAR_SITES:
    PATCHES[_a] = _tunic_clear


# Underwater checks (tools/rando/underwater.py; LAS 0.5 dev build's main+0x8c45b8 patch): the item actor's "should I
# appear?" (main+0x8c4510) asks the inventory whether Link owns its item (bl HasItem(item +0x2e8, 1, index +0xbc)). The
# sunken heart-piece actors hold randomized items now and hide by their own flag (Npc.gsheet layout condition), so for
# heart-piece actors in seeds made with it the answer is "not owned" (runtime/hle/qol.c rt_underwater_skip_owned).
UNDERWATER_OWNED_SITE = A.UNDERWATER_OWNED_SITE


def _underwater_owned(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x9400351e, hex(w)  # bl 0x8d1a30 (HasItem)
    return addr_text('{ extern int rt_underwater_skip_owned(uint64_t); if (rt_underwater_skip_owned(c->x[20])) c->x[0] = 0; '
            'else { extern void f_@HAS_ITEM@(Cpu *); c->x[30] = 0x@UNDERWATER_OWNED_RETURN@ull; f_@HAS_ITEM@(c); } }'
            f'  /* {a:#x}: bl HasItem (sunken heart pieces: their own flag decides) */')


PATCHES[UNDERWATER_OWNED_SITE] = _underwater_owned

# ...and their item models show underwater. Water is opaque here (its depth hides what's below): what's seen below the
# surface is a second, low-resolution view the water samples, holding only models whose materials have the shader option
# under_water - the heart piece's do, other items' don't. Model setup (main+0xff53d8 loop over a material's options)
# reads it at 0xff54b4; in underwater-item seeds every material counts as under_water (as LAS-Randomizer's 0.5 dev
# build does: its exlaunch mod writes mov w27, #1 here).
UNDERWATER_VIEW_SITE = A.UNDERWATER_VIEW_SITE


def _underwater_view(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x1a9f07fb, hex(w)  # cset w27, ne (under_water != 0)
    return ('{ extern int rt_underwater_items(void); c->x[27] = (rt_underwater_items() || COND_NE(c)) ? 1u : 0u; }'
            f'  /* {a:#x}: cset w27, ne (under_water: forced on for sunken items) */')


PATCHES[UNDERWATER_VIEW_SITE] = _underwater_view

# ...and picking one up gives only its own item (its event): the heart-piece actor's pickup (main+0x855a0, x19 = the
# actor) would also add a heart piece (bl 0x89b960; LAS-Randomizer's 0.5 dev build NOPs it).
UNDERWATER_PICKUP_SITE = A.UNDERWATER_PICKUP_SITE


def _underwater_pickup(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x9420589f, hex(w)  # bl 0x89b960
    return addr_text('{ extern int rt_underwater_skip_owned(uint64_t); if (!rt_underwater_skip_owned(c->x[19])) '
            '{ extern void f_@HEART_PIECE_ADD@(Cpu *); c->x[30] = 0x@UNDERWATER_PICKUP_RETURN@ull; f_@HEART_PIECE_ADD@(c); } }'
            f'  /* {a:#x}: bl 0x89b960 (adds the heart piece: skipped for sunken items) */')


PATCHES[UNDERWATER_PICKUP_SITE] = _underwater_pickup


# Event actions with no handler: main+0x108e300 runs an event action through the actor's handler map; an entry whose
# std::function is empty throws std::bad_function_call (0x108edc8), which the same function catches (catch
# std::exception, landing pad 0x108f1c8 -> 0x108ef3c) to abort the event. Common.bfevfl's OcarinaWarpOut (every Manbo's
# Mambo warp) gets here at WarpHoleEventBox.PlayTailorOtherChannelEx, after the level jump has been requested.
# Recompiled code can't unwind, so the throw goes straight into the catch block, without the C++ runtime's
# begin / end catch (nothing was thrown).
NO_HANDLER_SITES = {
    A.NO_HANDLER_SITES_0: (0x321d03e0, addr_text('goto L_@NO_HANDLER_CATCH@;'), 'mov w0, #8 (bad_function_call: to the catch block)'),
    A.NO_HANDLER_SITES_1: (0x940e8868, '', 'bl __cxa_begin_catch (skipped)'),
    A.NO_HANDLER_SITES_2: (0x940e887b, '', 'bl __cxa_end_catch (skipped)'),
}


def _no_handler(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    want, code, what = NO_HANDLER_SITES[a]
    assert w == want, hex(w)
    return f'{{ {code} }}  /* {a:#x}: {what} */'


for _a in NO_HANDLER_SITES:
    PATCHES[_a] = _no_handler


# Mirror mode: the HUD update places the in-world action prompt (main+0xeaa800: stp s2, s0, [x8, #0x30] = the
# pane's translation x, y from the target's screen spot); x is mirrored there for that pane (runtime/hle/mirror.c).
PROMPT_POS_SITE = A.PROMPT_POS_SITE


def _prompt_pos(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x2d060102, hex(w)  # stp s2, s0, [x8, #0x30]
    return ('{ extern float rt_prompt_x(uint64_t pane, float x); float px_, py_; memcpy(&px_, &c->v[2], 4); '
            'memcpy(&py_, &c->v[0], 4); px_ = rt_prompt_x(c->x[8], px_); '
            'memcpy((void *)(uintptr_t)(c->x[8] + 0x30), &px_, 4); memcpy((void *)(uintptr_t)(c->x[8] + 0x34), &py_, 4); }'
            f'  /* {a:#x}: stp s2, s0, [x8, #0x30] (action prompt spot: mirror mode) */')


PATCHES[PROMPT_POS_SITE] = _prompt_pos


# Issue #45: setting up a font, the game looks up the glyph of "□" (U+25A1, its box for a missing character) and
# marks it (main+0x111dfc0: the lookup at main+0x111dffc, then `ldrb w8, [x0, #0x38]` at main+0x111e000) without
# checking for none - its second lookup in the same function does check. With a substitute font that has no such
# glyph for the game (a Yu Gothic on one Windows PC, although its cmap lists it) that read crashed at 0x38. Now no
# glyph skips the marking as the second lookup's check does (to the function's end).
FONT_BOX_GLYPH_SITE = A.FONT_BOX_GLYPH_SITE


def _font_box_glyph(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0x3940e008, hex(w)  # ldrb w8, [x0, #0x38]
    return (addr_text('if (c->x[0] == 0) goto L_@FONT_BOX_GLYPH_SKIP@; c->x[8] = LD8(c->x[0] + 0x38);') +
            f'  /* {a:#x}: ldrb w8, [x0, #0x38] (font box glyph: none skips the marking) */')


PATCHES[FONT_BOX_GLYPH_SITE] = _font_box_glyph


# Issue #19: [this+0x10] is the delegate of a recycled "action" object (the class whose vtable is 0x7001c51358, and
# the ~100 siblings sharing these forwarders). Its slot +0x28 (main+0x10cce50) and +0x30 (main+0x10cce30) forward to
# that delegate without checking it, while the class's own slot +0x38 (main+0x10cce80) checks *its* delegate (+0x18)
# with `cbz` and returns when there is none. The task loop (main+0x13c21e0) picks the slot from the task's state and
# the object's slot +0x00 (main+0x10cce00) re-initialises it, clearing the delegate: the tick can then land on a
# freshly reset object and the forwarder dereferences NULL (signal 11 at main+0x10cce50+0x8, always the same object).
# runtime/hle/delegate_null.c makes both forwarders read a missing delegate as "nothing to do" as well.
DELEGATE_FORWARDER_SITES = [A.DELEGATE_FORWARDER_SITES_0, A.DELEGATE_FORWARDER_SITES_1]


def _delegate_forwarder(gen, a):
    w = struct.unpack_from('<I', gen.nso.image, a - gen.base)[0]
    assert w == 0xaa0003e1, hex(w)  # mov x1, x0
    slot = 0x28 if a == A.DELEGATE_FORWARDER_SITES_0 else 0x30
    return ('{ extern int rt_delegate_null(Cpu *, int); if (rt_delegate_null(c, %#x)) return; c->x[1] = c->x[0]; }'
            f'  /* {a:#x}: mov x1, x0 (delegate forwarder: nothing to forward to) */' % slot)


for _a in DELEGATE_FORWARDER_SITES:
    PATCHES[_a] = _delegate_forwarder


# Issue #41: an event's handler that is an empty std::function. The event dispatcher main+0x108ddc0 calls it through
# main+0x108dfc0 (std::function's call: throws std::bad_function_call when empty) and catches that (its landing pad
# 0x108de78 -> 0x108de00: returns 0, "not handled") - the catch can't work in recompiled code (no unwinding), so the
# throw ended in std::terminate. The hook skips the call when the function is empty, to where the catch goes. Seen
# with Warp tab jumps; same family as NO_HANDLER_SITES (the action dispatcher's).
EMPTY_HANDLER_SITE = A.EMPTY_HANDLER_SITE  # bl main+0x108dfc0

# Issue #183: the message window's question setup (main+0xef43b0) counts a question's answers as the text's pages
# after the question's own (pages end at <0.4> and, past the style's line count, at a line break) and shows the
# InOutChoice2/3/4 pane for it; Selecting (main+0xef2250, main+0xef2300) indexes those panes by the count with no
# check. Every language's own text gives 2..4. Text of a 2-line language (Chinese, Japanese, Korean) in a 3-line
# language's folder (a translation patch over the English text) makes the Mermaid's necklace question (Warashibe
# QuestBra) a 1-answer question: pane "1" is no pane (the 0x10001 read) and the game crashes, on the console too. The
# hook stops there with a crash message that says so instead.
CHOICE_COUNT_SITE = A.CHOICE_COUNT_SITE  # str w8, [x19, #0x1cc4]

# Issue #19 (the hang): the per-frame kick-off (main+0x15730, four inlined copies, one per owner X) rebuilds X's frame
# job J (X+0xc8) in place. The job worker (main+0x13c21e0) runs J (main+0x10cceb0: sets X's done flag, wakes the main
# thread, unlocks) and only then calls J's finish (main+0x10ccfa0 -> slot +0x00: base vtable, delegate cleared). A
# worker preempted between the two lets the main thread kick off the next frame first; the late finish then wipes the
# new J, whose release is dropped and the main thread waits for X's done flag forever. The kick-off hooks wait for the
# previous run's finish (runtime/hle/delegate_null.c); the finish hook only serves a debug delay to force the race.
FRAME_JOB_KICKOFF_SITES = [A.FRAME_JOB_KICKOFF_SITES_0, A.FRAME_JOB_KICKOFF_SITES_1, A.FRAME_JOB_KICKOFF_SITES_2,
                           A.FRAME_JOB_KICKOFF_SITES_3]
FRAME_JOB_FINISH_SITE = A.FRAME_JOB_FINISH_SITE

# Startup hang (Windows, few cores): a call run on its object's owner thread. main+0x1048cf0 (slot +0x48 of the class
# whose vtable is 0x7001c490b0; a loader thread calls it for an object O the main thread owns) locks O's mutex (O+0x18),
# posts a task to the owner's queue ([O+8]) and waits on O's condition variable (O+0x38) once, with no predicate. The
# task (main+0x1049ed0, on the main thread) stores the result at O+0x10 and calls notify_one without taking O's mutex.
# If the loader thread is preempted between its post and its wait, the notify finds nobody and the loader waits forever,
# while the main thread waits for that loader's load (key in its own task queue). The hook takes and drops O's mutex
# before the notify: the loader holds it from before the post until its wait has begun, so the notify always lands.
OWNER_CALL_NOTIFY_SITE = A.OWNER_CALL_NOTIFY_SITE


# Shadow Link's Appear event (main+0xd157f0: requested once Link is within 3 units, ShadowLink.bfevfl) stops Link and
# turns the camera to him; on the Field (seeds) the request is skipped and runtime/hle/miniboss.c plays his appearance
# (Sign glow, then he pops up) without an event. The site is right after the function's already-requested check; the
# skip goes to its return.
SHADOW_LINK_APPEAR_SITE = A.SHADOW_LINK_APPEAR_SITE

# Shadow Link's own music: once he is up and Link is in range, his code calls the chamber manager's start
# (main+0xce8ce0: BGM_PANEL_SHADOW_LINK, the level's music held until his defeat calls main+0xce8f00). Dampe's chamber is
# the only place he has in the game; in seeds, everywhere else (the Field, dungeons, caves, houses) the call is skipped
# and the area's own music plays on (runtime/hle/miniboss.c rt_shadow_link_bgm).
SHADOW_LINK_BGM_SITE = A.SHADOW_LINK_BGM_SITE

# An actor's AI starting one of its events (main+0x94a510, x20 = the AI, flow name at [x29-0x40]): right before it
# builds the event request (bl main+0x80c290). Any event request holds Link still while it runs; a randomizer item
# picked up from the ground (a SinkingSword item actor) whose entry only sets its flag and adds a rupee / heart /
# refill is given by the runtime instead (runtime/hle/quick_pickup.c) and the request skipped: the skip goes to the
# function's no-event return.
ACTOR_EVENT_SITE = A.ACTOR_EVENT_SITE


# Issue #314 (Hot Head, Turtle Rock: crash reading 0x11 in main+0x142afb0 when the Magic Rod's fire hit him): a PhysX
# shape change on an actor in the scene (main+0x1408db0, RigidCore::onShapeChange - the geometry / pose / filter /
# flags / offsets a shape setter just changed, flags at [x2]) and its sibling main+0x1408d60 look up the actor's
# simulation object for that shape with main+0x14090a0 (RigidSim::getSimForShape). PhysX hands back the address 1
# when the shape has none ("should never happen": it is only asserted in PhysX's own debug builds) and the update then
# reads through it. The hooks right after the lookup skip the update for a shape the simulation doesn't hold (the
# functions' own return): its simulation object, if one is made later, starts from the shape's current state.
SHAPE_SIM_SITES = [A.SHAPE_SIM_SITES_0, A.SHAPE_SIM_SITES_1]

# Issue #349 (a Chamber Stone from a Shellsanity rock in Ukuku Prairie: crash reading 0x8 in main+0xe44280): an
# effect emitter that follows a bone of its actor's model (an effect descriptor of kind 1 - e.g. the glitter
# ObjSinkingSword.drsb puts on the bone "root" of every SinkingSword item) keeps the bone's node at emitter +0x50, looked
# up once when the emitter is made (main+0xe40bb0 -> main+0xe411b8). The lookup gives no node when the actor has no
# model instance then (main+0xe51f20) and nothing checks it: each update (main+0xe44280) reads the bone's matrix
# through it. The hook takes an emitter without its node down the kind-0 path instead (the actor's own transform),
# so the effect plays where the item is.
EFFECT_BONE_SITE = A.EFFECT_BONE_SITE

# ---- hook sites: the instruction stays as it is; just before it the runtime's rt_hook (runtime/hle/hooks.c) is asked
# what to do - 0 run it, 1 the hook did it all, 2.. jump to the site's first.. target (each a label in its function).
# The game code they generate never changes when a hook's behaviour does: only a new site needs the game code
# rebuilt. New game-code changes go here, as a hook, unless they can't.
# site -> (the instruction word, (jump targets...), what it's for)
HOOKS = {
    MB_SEEN_SITE: (0xf94002a8, (), 'ldr x8, [x21]: an actor seen - field mini-bosses (miniboss.c)'),
    CRASHOUT_SITE: (0xb900b81f, (A.CRASHOUT_SKIP,), 'str wzr, [x0, #0xb8]: Blaino knock-out, no crash-out on the Field'),
    GOHMA_HEADING_SITES[0]: (0xb915ee68, (), 'str w8, [x19, #0x15ec]: Gohma heading at Link on the Field'),
    GOHMA_HEADING_SITES[1]: (0xb915ee68, (), 'str w8, [x19, #0x15ec]: Gohma heading at Link on the Field (again)'),
    GOHMA_ARRIVED_SITE: (0x3dc58260, (A.GOHMA_ARRIVED_SKIP,), 'ldr q0, [x19, #0x1600]: Gohma, no track end on the Field'),
    MSTALFOS_BODY_SITE: (0x320003e8, (), 'mov w8, #1: Master Stalfos body collision flag, off on the Field'),
    MSTALFOS_JUMP_SITE: (0x1e213800, (), 'fsub s0, s0, s1: Master Stalfos jump arc from his takeoff height'),
    SHADOW_LINK_APPEAR_SITE: (0xf9411809, (A.SHADOW_LINK_APPEAR_SKIP,), 'ldr x9, [x0, #0x230]: Shadow Link, no Appear event on the Field'),
    SHADOW_LINK_BGM_SITE: (0x97ff5eac, (), 'bl main+0xce8ce0: Shadow Link starts his chamber music, not outside it'),
    ACTOR_EVENT_SITE: (0x97fb074a, (A.ACTOR_EVENT_SKIP,), 'bl main+0x80c290: an actor\'s event request (quick pickups: none)'),
    MCHIEF_STORY_SITE: (0x37380148, (A.MCHIEF_STORY_SKIP,), 'tbnz w8, #7: Moblin Chief, no story check on the Field'),
    DIG_HOLE_SITE: (0xf94007f6, (), 'ldr x22, [sp, #8]: a hole was dug (digs.c)'),
    HOLE_CAP_SITES[0]: (0xf101451f, (), 'cmp x8, #0x51: dug-hole limit (digs.c)'),
    HOLE_CAP_SITES[1]: (0xf101411f, (), 'cmp x8, #0x50: dug-hole limit (digs.c)'),
    EMPTY_HANDLER_SITE: (0x9400005e, (A.EMPTY_HANDLER_CATCH,), 'bl std::function call: an empty event handler, as its catch'),
    CHOICE_COUNT_SITE: (0xb91cc668, (), 'str w8, [x19, #0x1cc4]: a question\'s answer count (2..4 in the game\'s own text)'),
    FRAME_JOB_KICKOFF_SITES[0]: (0x910782d3, (), 'add x19, x22, #0x1e0: frame job kick-off, previous finish first'),
    FRAME_JOB_KICKOFF_SITES[1]: (0x910782d3, (), 'add x19, x22, #0x1e0: frame job kick-off, previous finish first'),
    FRAME_JOB_KICKOFF_SITES[2]: (0x910782d3, (), 'add x19, x22, #0x1e0: frame job kick-off, previous finish first'),
    FRAME_JOB_KICKOFF_SITES[3]: (0x910782b3, (), 'add x19, x21, #0x1e0: frame job kick-off, previous finish first'),
    FRAME_JOB_FINISH_SITE: (0xf9400008, (), 'ldr x8, [x0]: frame job finish (debug delay)'),
    OWNER_CALL_NOTIFY_SITE: (0x9100e2a0, (), 'add x0, x21, #0x38: owner-thread call done, the caller is waiting first'),
    SHAPE_SIM_SITES[0]: (0xaa0003f4, (A.SHAPE_SIM_SKIP_0,), 'mov x20, x0: a PhysX shape change, no update without its simulation object (#314)'),
    SHAPE_SIM_SITES[1]: (0xaa0003e1, (A.SHAPE_SIM_SKIP_1,), 'mov x1, x0: a PhysX shape change, no update without its simulation object (#314)'),
    EFFECT_BONE_SITE: (0xf9402ac0, (A.EFFECT_BONE_SKIP,), 'ldr x0, [x22, #0x50]: a bone-following effect without its bone, at the actor instead (#349)'),
}
