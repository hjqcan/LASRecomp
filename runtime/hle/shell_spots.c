// Secret Seashell spots as checks: the game's own hiding places of a Secret Seashell - buried (dug up with the
// shovel), under a bush / rock / pot, in a tree (dash into it), in a hole a bomb goes into - hold the seed's item
// there (tools/rando/seashell_spots.py writes rando/<seed>/shell_spots.tsv; no file: vanilla shells).
//
// A Secret Seashell placed at a spot stays the game's own shell (its seashell index the one LAS gave it), so the
// Seashell Sensor rings for it until it is found - and for no spot holding anything else: the sensor's beacons are the
// buried shell actors and the hosts whose ItemDrop entry drops a shell not yet owned. Any other item:
//   dig   the generator takes the buried shell (ItemSecretSeashell 0x87) out of the room; the shovel's dig commit
//         (rt_dig_hole -> shell_spots_dig) on its tile brings the item up out of the hole: a SinkingSword item actor
//         (the actor LAS uses for standing items, pots, bushes, digsanity: digs.c digs_spawn_item).
//   drop  the bush / rock / pot / tree / bomb hole drops a Heart Piece tagged TAG + the spot's vanilla seashell index
//         (ItemDrop LARShellSpot<n>; a bomb hole through ItemDrop Seashell<its drop index> - see hole_spawn, and
//         hole_beacons for its sensor beacon). The factory's spawn info is rewritten before the type is read
//         (rt_shell_swap, from enemy_random.c's rt_enemy_spawn): a SinkingSword item actor with the item's model, flow
//         entry and flag in its type-5 (string pool) parameters.
//   land  a tree / bomb hole launches its drop out of its crown / up out of the hole: that drop makes no actor; the
//         item (that SinkingSword actor) flies out on the vanilla shell's arc instead and comes down on a free tile
//         beside the tree / hole (fly_start)
// Picking it up runs its entry in SinkingSword.bfevfl (LAS's item-get: held up, the game's message box) and sets the
// spot's own flag; a spot whose flag is set gives nothing again, and one whose item already dropped gives no second
// one (a second bonk of the tree, say) until Link has left its screen (a tree's / hole's: and its item is gone). A
// 'shell' row (a Secret Seashell there, also on a digsanity tile) only has its found flag set here once that seashell
// is owned (the tracker).
//
// shell_spots.tsv: location, seashell index (vanilla; a 'shell' row: the placed shell's), kind (dig / drop / land /
// shell), level, room, x, y, z, model path, model name, flow entry, flag symbol, flag index, seashell (1/0), scale,
// turn (degrees) (, a hole: its actor key, drop index); x y z: a dig spot's tile centre, where a 'land' spot's item
// lands
#include "../game_addresses.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../rando.h"

#define MAXSPOTS 160  // (31 spots + a "shell" row per digsanity tile holding a Secret Seashell: up to 81; 18 apple slots)
#define T_SHELL 0x87
#define T_SINKING_SWORD 0x194
#define T_HEART_PIECE 0xb0
#define TAG 200  // seashell_spots.py TAG: a retagged drop's index
#define T_SHELL_DROP_AREA 0x233  // AreaShellDrop: a bomb hole
#define DIG_FLAG_BASE 4096  // digs.c: a digsanity tile's dug flag

bool flags_ext_get(int idx);
uint64_t doorjump_player(uint64_t frame);
bool digs_spawn_item(Cpu *c, const char *room, uint64_t key, uint32_t zone, const float pos[3], float scale, float turn,
                     const char *const strs[5], uint64_t *slot, bool *spawned);
bool digs_actor_alive(uint64_t ctrl);
void digs_drop_ref(Cpu *c, uint64_t *slot);
bool digs_spawn_item_into(Cpu *c, uint64_t room, uint64_t rctrl, uint64_t key, uint32_t zone, const float pos[3],
                          float scale, float turn, const char *const strs[5], uint64_t *slot, bool *spawned);
void digs_hold_weak(uint64_t *slot, uint64_t ctrl);
int digs_pool_index(Cpu *c, const char *str);

typedef struct {
    char name[48], level[32], room[32], model[64], mname[48], entry[48], flag[32];
    int index, flag_idx, dig, land, hole, owned, shell, apple;  // owned: a 'shell' row; apple: an apple tree slot
    float x, y, z, scale, turn;
    uint64_t key;         // a bomb hole's actor (its level key)
    float hx, hz;         // ... where it stands (its spawn), 0: not seen
    int hole_k;           // ... the seashell index it is up with (hole_beacons), -1: not up
    int pool[5];          // the strings' string pool indices (-1: not yet)
    uint64_t item;        // dug-up item actor (control block, weak ref held)
    bool dug;             // dug this visit (the hole lasts until the scene goes)
    bool dropped;         // its item dropped this visit: a second bonk / lift / bomb gives no second one
    bool queued;
    // a tree's / bomb hole's item flies out (fly_start): the drop's launch point and velocity (units/s), its flight
    float from[3], v0[3];
    bool fly_queued;      // launched: spawned at `from` on the next tick
    int fly_t;            // -1: not flying; else ticks since it came down (the settle hop)
    bool landed;
    bool hang;            // Applesanity: the item hangs in its tree's crown at hang_at (apple_trees.c)
    float hang_at[3];
    float fp[3], fv[3];   // its position, velocity (units/tick)
    uint64_t fly_actor;  // the item actor (while s->item is alive)
    uint64_t motion;      // its physics motion (fly_motion)
    uint64_t droom, room_ref;  // the room the drop was made in (weak ref held): the item goes there
    uint32_t zone;
} Spot;
static Spot g_spots[MAXSPOTS];
static int g_n = -1;
static bool g_pool_ready;
static int g_empty;  // pool index of ""
static uint32_t g_zone = 1;
static uint64_t g_scene_was;

RT_ON_RESET(shell_spots_reset) {
    g_n = -1;
    g_pool_ready = false;
    g_zone = 1;
    g_scene_was = 0;
    memset(g_spots, 0, sizeof g_spots);
}

static pthread_mutex_t g_load_lock = PTHREAD_MUTEX_INITIALIZER;

static int load_locked(void);
static int load(void) {  // (first use: the game thread or a room loading thread)
    if (__atomic_load_n(&g_n, __ATOMIC_ACQUIRE) >= 0) return g_n;
    pthread_mutex_lock(&g_load_lock);
    int n = load_locked();
    pthread_mutex_unlock(&g_load_lock);
    return n;
}

static int load_locked(void) {
    if (g_n >= 0) return g_n;
    int n = 0;
    const char *dir = rando_seed_dir();
    if (!dir) return __atomic_store_n(&g_n, 0, __ATOMIC_RELEASE), 0;
    char path[512], line[1024];
    // (apple_trees.tsv: Applesanity's tree slots, the same columns - kind "apple", flown out by apple_trees.c)
    for (int file = 0; file < 2; file++) {
        snprintf(path, sizeof path, "%s/%s", dir, file ? "apple_trees.tsv" : "shell_spots.tsv");
        FILE *f = fopen(path, "r");
        if (!f) continue;
        while (fgets(line, sizeof line, f) && n < MAXSPOTS) {
            if (line[0] == '#') continue;
            char *col[17], *save = NULL;
            int nc = 0;
            for (char *t = strtok_r(line, "\t\n", &save); t && nc < 17; t = strtok_r(NULL, "\t\n", &save)) col[nc++] = t;
            if (nc < 16) continue;
            Spot *s = &g_spots[n++];
            strlcpy(s->name, col[0], sizeof s->name);
            s->index = atoi(col[1]);
            s->dig = !strcmp(col[2], "dig");
            s->hole = !strcmp(col[2], "hole");
            s->apple = !strcmp(col[2], "apple");
            s->land = s->hole || s->apple || !strcmp(col[2], "land");
            s->owned = !strcmp(col[2], "shell");
            strlcpy(s->level, col[3], sizeof s->level);
            strlcpy(s->room, col[4], sizeof s->room);
            s->x = (float)atof(col[5]), s->y = (float)atof(col[6]), s->z = (float)atof(col[7]);
            strlcpy(s->model, col[8], sizeof s->model);
            strlcpy(s->mname, col[9], sizeof s->mname);
            strlcpy(s->entry, col[10], sizeof s->entry);
            strlcpy(s->flag, col[11], sizeof s->flag);
            s->flag_idx = atoi(col[12]);
            s->shell = atoi(col[13]);
            s->scale = (float)atof(col[14]), s->turn = (float)atof(col[15]);
            s->key = nc > 16 && s->hole ? strtoull(col[16], NULL, 16) : 0;
            s->hole_k = -1;
            for (int k = 0; k < 5; k++) s->pool[k] = -1;
            s->fly_t = -1;
        }
        if (n == MAXSPOTS && fgets(line, sizeof line, f)) LOG("[shells] warning: more than %d rows, the rest ignored\n", MAXSPOTS);
        fclose(f);
        LOG("[shells] %d spots hold the seed's items (%s)\n", n, path);
    }
    __atomic_store_n(&g_n, n, __ATOMIC_RELEASE);
    return n;
}

static const char *level_name(void) {
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
    return ps ? (const char *)(uintptr_t)ps : "";
}

// a retagged drop: a Heart Piece (or a bomb hole's shell) with no level key, parameter 0 (spawn info +0x10: u32 value,
// u8 type at +0x14; 3 = integer) TAG + the spot's vanilla seashell index
static Spot *spot_of(uint64_t info) {
    uint32_t type = LD16(info + 8);
    if ((type != T_HEART_PIECE && type != T_SHELL) || LD64(info) || LD8(info + 0x14) != 3 || load() <= 0) return NULL;
    int idx = (int)LD32(info + 0x10) - TAG;
    if (idx < 0 || idx >= 64) return NULL;
    for (int i = 0; i < g_n; i++)
        if (g_spots[i].index == idx && !g_spots[i].owned && !g_spots[i].dig) return &g_spots[i];
    return NULL;
}

static Spot *hole_of(uint64_t info);

// before the factory reads the type (enemy_random.c rt_enemy_spawn): a dropped Secret Seashell becomes the item
void rt_shell_swap(uint64_t info) {
    Spot *s = info ? spot_of(info) : NULL;
    if (!s && info) s = hole_of(info);
    if (!s) return;
    if (flags_ext_get(s->flag_idx) || !g_pool_ready || s->dropped) {
        LOG("[shells] %s: %s\n", s->name, !g_pool_ready ? "not ready (nothing)" : s->dropped ? "already dropped here (nothing)"
                                                                                    : "already found (nothing)");
        return;  // (rt_shell_skip: no actor)
    }
    s->dropped = true;  // (until the scene goes: the item lies there until picked up)
    if (s->land) {  // (out of a crown / a hole: the item flies out on the next tick - fly_start)
        const float *tp = (const float *)(uintptr_t)(info + 0x180);
        memcpy(s->from, tp, sizeof s->from);
        for (int k = 0; k < 3; k++)  // (the drop's parameters 2..4: its launch velocity, floats)
            s->v0[k] = LD8(info + 0x24 + 8 * k) == 2 ? *(const float *)(uintptr_t)(info + 0x20 + 8 * k) : 0;
        s->zone = LD32(info + 0xc);
        // the room the drop is made in (the factory's x1: room, control block) - the item goes there (fly_start)
        Cpu *c = kernel_current_cpu();
        uint64_t pair = c ? c->x[1] : 0;
        s->droom = pair ? LD64(pair) : 0;
        if (s->droom && pair) digs_hold_weak(&s->room_ref, LD64(pair + 8));
        s->fly_queued = true;
        LOG("[shells] %s: the tree / hole launches its item (%s) from (%.2f, %.2f, %.2f) at (%.2f, %.2f, %.2f)/s\n",
            s->name, s->mname, s->from[0], s->from[1], s->from[2], s->v0[0], s->v0[1], s->v0[2]);
        return;  // (rt_shell_skip: the drop makes no actor - ours is the item)
    }
    ST16(info + 8, T_SINKING_SWORD);
    for (int k = 0; k < 8; k++) {
        ST32(info + 0x10 + 8 * k, (uint32_t)(k < 5 ? s->pool[k] : g_empty));
        ST8(info + 0x14 + 8 * k, 5);
    }
    float *tp = (float *)(uintptr_t)(info + 0x180), a = s->turn * (float)M_PI / 360.0f;
    tp[4] = 0, tp[5] = sinf(a), tp[6] = 0, tp[7] = cosf(a);
    tp[8] = tp[9] = tp[10] = s->scale > 0 ? s->scale : 1.0f;
    LOG("[shells] %s: the bush / rock / pot gives its item (%s) at (%.2f, %.2f, %.2f)\n", s->name,
        s->mname, tp[0], tp[1], tp[2]);
}

// A bomb hole (AreaShellDropper) only stays while the Secret Seashell its index names isn't owned (PlayerStatus
// +0x88): main+0x849f0, its set-up, removes it while that shell is owned or the index is 50 or more (the index is kept
// at +0x2c8 and read again whenever it is set up again - the Field streams screens out and back), main+0x85510 as
// soon as it is owned; bombed, it drops ItemDrop Seashell<+0x2c8>. A hole holding something else keeps its vanilla
// index, whose seashell LAS placed somewhere in the seed - found first, the hole was gone for good. So it spawns with
// an index whose shell isn't owned (its own while that one isn't: hole_spawn), is kept on one (hole_beacons), and its
// shell drop is taken for the item (hole_of: Seashell<that index> from where the hole stands).
static uint64_t owned_shells(void) {
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
    return ps ? LD64(ps + 0x88) : 0;
}

static int free_shell(uint64_t owned, int want) {  // want while not owned, else the first not owned (-1: all are)
    if (want >= 0 && want < 50 && !(owned >> want & 1)) return want;
    for (int k = 0; k < 50; k++)
        if (!(owned >> k & 1)) return k;
    return -1;
}

static void hole_spawn(uint64_t info) {
    if (load() <= 0 || LD8(info + 0x14) != 3) return;
    for (int i = 0; i < g_n; i++) {
        Spot *s = &g_spots[i];
        if (!s->hole || !s->key || s->key != LD64(info)) continue;
        const float *tp = (const float *)(uintptr_t)(info + 0x180);
        s->hx = tp[0], s->hz = tp[2];
        int idx = (int)LD32(info + 0x10), k = free_shell(owned_shells(), idx);
        if (k >= 0 && k != idx) {
            ST32(info + 0x10, (uint32_t)k);
            LOG("[shells] %s: seashell %d owned - the hole comes up as %d\n", s->name, idx, k);
        }
        return;
    }
}

// a bomb hole's shell drop: a Secret Seashell (no level key) with the index the hole is up with, from where it stands
static Spot *hole_of(uint64_t info) {
    if (LD16(info + 8) != T_SHELL || LD64(info) || LD8(info + 0x14) != 3 || load() <= 0) return NULL;
    const float *tp = (const float *)(uintptr_t)(info + 0x180);
    for (int i = 0; i < g_n; i++) {
        Spot *s = &g_spots[i];
        if (s->hole && s->hole_k >= 0 && (int)LD32(info + 0x10) == s->hole_k &&
            (!s->hx || (fabsf(tp[0] - s->hx) < 1.0f && fabsf(tp[2] - s->hz) < 1.0f)))
            return s;
    }
    return NULL;
}

// the factory's create-function hook (souls.c): nothing for a retagged drop already found (or before the strings are in)
bool rt_shell_skip(uint32_t type, uint64_t info) {
    if (type == T_SHELL_DROP_AREA) hole_spawn(info);
    if (type != T_HEART_PIECE && type != T_SHELL) return false;
    Spot *s = spot_of(info);
    if (!s) s = hole_of(info);
    return s && (flags_ext_get(s->flag_idx) || !g_pool_ready || s->dropped);
}

static int field_tile(const Spot *s) {  // a Field spot's digsanity tile id (digs.c), -1 elsewhere
    if (strcmp(s->level, "Field")) return -1;
    int col = (int)floorf(s->x / 15.0f), row0 = (int)floorf(s->z / 12.0f);
    int tx = (int)floorf((s->x - col * 15.0f) / 1.5f), tz = (int)floorf((s->z - row0 * 12.0f) / 1.5f);
    return (row0 * 16 + col) * 80 + tz * 10 + tx;
}

static bool dug(const Spot *s) {
    int g = field_tile(s);
    return s->dug || (g >= 0 && flags_ext_get(DIG_FLAG_BASE + g));  // (digsanity keeps its holes open)
}

// shovel dig commit (digs.c rt_dig_hole): x21 the hole's transform (+0x30 the ground point on the tile centre), x20
// +0xb0 the zone id
void shell_spots_dig(Cpu *c) {
    if (load() <= 0) return;
    const float *t = (const float *)(uintptr_t)c->x[21];
    const char *lv = level_name();
    for (int i = 0; i < g_n; i++) {
        Spot *s = &g_spots[i];
        if (!s->dig || strcmp(lv, s->level) || fabsf(t[12] - s->x) > 0.75f || fabsf(t[14] - s->z) > 0.75f) continue;
        // (only a spot holding something else: a Secret Seashell there is the game's own buried shell)
        g_zone = LD32(c->x[20] + 0xb0);
        s->dug = true;
        LOG("[shells] dug %s%s\n", s->name, flags_ext_get(s->flag_idx) ? " (already found)" : "");
        if (!flags_ext_get(s->flag_idx)) s->queued = true;  // (spawned on the next tick, outside the shovel's code)
    }
}

static void spawn(Cpu *c, Spot *s) {
    const char *strs[5] = {s->model, s->mname, s->entry, s->flag, s->shell ? "true" : "false"};
    float pos[3] = {s->x, s->y, s->z};
    bool up;
    if (s->item) digs_drop_ref(c, &s->item);
    if (digs_spawn_item(c, s->room, 0x7d00000000000000ull | (uint64_t)s->index, g_zone, pos, s->scale, s->turn, strs,
                        &s->item, &up))
        LOG("[shells] %s: item %s up out of the hole (%s)\n", s->name, s->mname, up ? "up" : "failed");
}

// A tree's / bomb hole's item flies out the way the Secret Seashell does. The game launches the shell from the drop
// point (a tree: its crown) with the velocity the drop passes (a tree: 2.24 up and 4.19 toward the camera, units/s); in
// flight the shell keeps 0.995 of its velocity each tick, under gravity 9.8 units/s^2, stops where it touches the
// ground and settles with a small hop (traced from the vanilla shell, tail-cave-bonk-tree). The item gets that flight:
// the drop's own upward speed, its sideways speed aimed so it comes down on the free tile beside the tree / hole that
// seashell_spots.py picked (where the shell itself would land may be a wall, water or the tree), then the hop.
// The item actor (SinkingSword) is a physics body, so its flight is written into its physics motion each tick: the
// item's collision object (actor +0x80) +0x170 -> +0x670 is the motion, +0xa0 its centre of mass (0.25 above the item's
// position, +0xc0: that offset), +0xd0 its velocity (units/s); the engine moves the body (and the item) from there.
#define FLY_DRAG 0.995f
#define FLY_G (9.8f / 3600.0f)
static const float k_settle[] = {-0.0495f, -0.0306f, -0.0061f, 0.0072f, 0.0177f, 0.0254f, 0.0304f, 0.0327f,
                                 0.0322f, 0.0291f, 0.0232f, 0.0146f, 0.0034f, 0.0007f, 0.0001f, 0.0f};
#define NSETTLE ((int)(sizeof k_settle / sizeof k_settle[0]))

static bool heap_ptr(uint64_t p) { return p >= HEAP_BASE && p + 0x200 <= HEAP_BASE + HEAP_MAX && !(p & 7); }

// the flying item's physics motion, 0: none (yet); `at` its centre of mass now - the motion found is checked against it
static uint64_t fly_motion(uint64_t actor, const float at[3]) {
    uint64_t col = LD64(actor + 0x80), a = heap_ptr(col) ? LD64(col + 0x170) : 0, m = heap_ptr(a) ? LD64(a + 0x670) : 0;
    if (!heap_ptr(m)) return 0;
    const float *f = (const float *)(uintptr_t)(m + 0xa0);
    return fabsf(f[0] - at[0]) < 0.3f && fabsf(f[1] - at[1]) < 0.6f && fabsf(f[2] - at[2]) < 0.3f ? m : 0;
}

static void fly_start(Cpu *c, Spot *s, uint64_t frame) {
    extern uint64_t digs_last_actor;
    const char *strs[5] = {s->model, s->mname, s->entry, s->flag, s->shell ? "true" : "false"};
    bool up;
    s->fly_queued = false;
    if (s->item) digs_drop_ref(c, &s->item);
    // into the room the drop was made in (where the shell would have gone: loaded, drawn - a screen's room picked
    // by name may be one the game keeps but doesn't show); else the landing tile's screen, the hole's, Link's
    bool loaded = s->droom && digs_spawn_item_into(c, s->droom, s->room_ref, 0x7d00000000000000ull | (uint64_t)s->index,
                                                  s->zone ? s->zone : g_zone, s->from, s->scale, s->turn, strs,
                                                  &s->item, &up);
    if (s->room_ref) digs_drop_ref(c, &s->room_ref);
    s->droom = 0;
    char rooms[3][16] = {"", "", ""};
    int nr = 0, r = 0;
    if (!loaded) {
        if (!strcmp(s->level, "Field")) {
            snprintf(rooms[nr++], 16, "Field_%02d%c", (int)floorf(s->z / 12.0f) + 1, 'A' + (int)floorf(s->x / 15.0f));
            uint64_t pc0 = doorjump_player(frame);
            const float *p = pc0 ? (const float *)(uintptr_t)(pc0 + 0x80) : NULL;
            if (p) snprintf(rooms[2], 16, "Field_%02d%c", (int)floorf(p[2] / 12.0f) + 1, 'A' + (int)floorf(p[0] / 15.0f));
        }
        snprintf(rooms[nr++], 16, "%s", s->room);
        if (rooms[2][0]) memcpy(rooms[nr++], rooms[2], 16);
        for (r = 0; r < nr && !loaded; r++)
            loaded = digs_spawn_item(c, rooms[r], 0x7d00000000000000ull | (uint64_t)s->index, g_zone, s->from,
                                     s->scale, s->turn, strs, &s->item, &up);
    }
    if (!loaded || !up || !digs_last_actor) {
        LOG("[shells] %s: item %s not up (%s)\n", s->name, s->mname, loaded ? "spawn failed" : "no screen loaded");
        return;  // (s->dropped stays: Link leaving the screen lets it drop again)
    }
    if (r) LOG("[shells] %s: (spawned into %s)\n", s->name, rooms[r - 1]);
    // the shell's upward speed (at least enough to come down on the landing tile), sideways aimed at the tile
    float dx = s->x - s->from[0], dz = s->z - s->from[2], d = sqrtf(dx * dx + dz * dz);
    float vy = (s->v0[1] > 0.5f ? s->v0[1] : 2.24133f) / 60.0f, sum = 0;
    for (int tries = 0; tries < 64; tries++, vy += 0.25f / 60.0f) {
        float y = s->from[1], v = vy, k = 1;
        int n;
        for (n = 1, sum = 0; n < 600; n++) {
            v = v * FLY_DRAG - FLY_G, y += v, k *= FLY_DRAG, sum += k;
            if (v < 0 && y <= s->y) break;
        }
        if (n < 600) break;
    }
    float h = sum > 0 ? d / sum : 0;
    s->fv[0] = d > 0 ? h * dx / d : 0, s->fv[1] = vy, s->fv[2] = d > 0 ? h * dz / d : 0;
    memcpy(s->fp, s->from, sizeof s->fp);
    s->fly_actor = digs_last_actor;
    s->motion = 0;
    s->fly_t = 0;
    s->landed = false;
    LOG("[shells] %s: item %s flies out to (%.2f, %.2f, %.2f)\n", s->name, s->mname, s->x, s->y, s->z);
}

static void fly_step(Spot *s) {
    if (!digs_actor_alive(s->item)) { s->fly_t = -1; return; }  // (picked up in the air / its screen went)
    float was[3] = {s->fp[0], s->fp[1], s->fp[2]};
    if (!s->landed) {
        s->fv[0] *= FLY_DRAG, s->fv[2] *= FLY_DRAG;
        s->fv[1] = s->fv[1] * FLY_DRAG - FLY_G;
        s->fp[0] += s->fv[0], s->fp[1] += s->fv[1], s->fp[2] += s->fv[2];
        s->landed = s->fv[1] <= 0 && s->fp[1] <= s->y;  // (down: on the tile)
    }
    if (s->landed) {
        int t = s->fly_t++;
        s->fp[0] = s->x, s->fp[1] = s->y + k_settle[t < NSETTLE ? t : NSETTLE - 1], s->fp[2] = s->z;
        if (t >= NSETTLE - 1) s->fly_t = -1;  // (at rest: the engine has it from here)
    }
    // its motion: found once its body is up (a few ticks), checked each tick (centre of mass where we put it)
    uint64_t m = s->motion;
    if (m) {
        const float *f = (const float *)(uintptr_t)(m + 0xa0), off = f[9];
        float at[3] = {was[0], was[1] + off, was[2]};
        m = fly_motion(s->fly_actor, at) == m ? m : 0;
    } else {
        float at[3] = {s->from[0], s->from[1] + 0.25f, s->from[2]};
        m = fly_motion(s->fly_actor, at);
    }
    s->motion = m;
    if (!m) return;
    float *f = (float *)(uintptr_t)(m + 0xa0), off = f[9];
    f[0] = s->fp[0], f[1] = s->fp[1] + off, f[2] = s->fp[2];
    f[12] = s->landed ? 0 : s->fv[0] * 60, f[13] = s->landed ? 0 : s->fv[1] * 60, f[14] = s->landed ? 0 : s->fv[2] * 60;
}

// A bomb hole holding something else keeps its seashell index (with any other - or that shell owned - it is gone at
// once and drops nothing; ItemDrop Seashell<n> drops the tagged Heart Piece), so it makes its Seashell Sensor beacon
// (AreaShellDrop +0x2b8: the ShellBeacon component). The beacon is switched off here once the hole is up (+0x14c: 0 =
// the sensor's check skips it, f_70008dd860), so the sensor doesn't ring for it. The level's placed actors by key:
// GA(ACTOR_REGISTRY) (pets.c).
static void hole_beacons(const char *lv) {
    uint64_t map = LD64(GA(ACTOR_REGISTRY));
    for (int i = 0; i < g_n && map; i++) {
        Spot *s = &g_spots[i];
        if (!s->hole || !s->key || strcmp(lv, s->level)) continue;
        int n = 0;
        for (uint64_t node = LD64(map + 0x10); node && n < 8192; node = LD64(node), n++) {
            if (LD64(node + 0x10) != s->key) continue;
            uint64_t a = LD64(node + 0x18), b = a ? LD64(a + 0x2b8) : 0;
            int32_t idx = a ? (int32_t)LD32(a + 0x2c8) : -1;
            if (idx >= 0 && idx < 50 && !flags_ext_get(s->flag_idx)) {  // (up: kept on a shell not owned)
                int k = free_shell(owned_shells(), idx);
                if (k >= 0 && k != idx) {
                    ST32(a + 0x2c8, (uint32_t)k);
                    LOG("[shells] %s: seashell %d owned - the hole goes by %d\n", s->name, idx, k);
                }
                s->hole_k = k >= 0 ? k : idx;
            }
            if (b && LD8(b + 0x14c)) {
                ST8(b + 0x14c, 0);
                LOG("[shells] %s: bomb hole's sensor beacon off\n", s->name);
            }
            break;
        }
    }
}

// Applesanity (apple_trees.c): a tree slot's item flies out from `from` (its apple in the crown) to its tile
static Spot *apple_spot(const char *name) {
    for (int i = 0; load() > 0 && i < g_n; i++)
        if (g_spots[i].apple && !strcmp(g_spots[i].name, name)) return &g_spots[i];
    return NULL;
}

static void hang_drop(Cpu *c, Spot *s) {  // (the hanging item goes: a bonk, or its tree went)
    if (s->hang && s->item && digs_actor_alive(s->item)) {
        uint64_t saved[31], sp = c->sp;
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = s->fly_actor;
        guest_call(c, GA(ACTOR_KILL));
        memcpy(c->x, saved, sizeof saved);
        c->sp = sp;
    }
    if (s->hang && s->item) digs_drop_ref(c, &s->item);
    s->hang = false;
}

#define HANG_SCALE 1.15f  // (of its size on the ground: it has to read from the game's camera)

// the slot's item hangs in the crown where its apple was (instead of the apple). false: not yet (the tree's screen
// isn't loaded, or the strings aren't in)
bool shell_spot_hang(Cpu *c, const char *name, const float at[3], uint32_t zone, uint64_t room, uint64_t rctrl) {
    Spot *s = apple_spot(name);
    if (!s || !g_pool_ready || flags_ext_get(s->flag_idx)) return false;
    if (s->hang && s->item && digs_actor_alive(s->item)) return true;
    extern uint64_t digs_last_actor;
    const char *strs[5] = {s->model, s->mname, s->entry, s->flag, s->shell ? "true" : "false"};
    bool up;
    if (s->item) digs_drop_ref(c, &s->item);
    // into the tree's own room (where its apples were made: drawn wherever Link is), else its screen by name
    bool ok = room && digs_spawn_item_into(c, room, rctrl, 0x7d00000000000000ull | (uint64_t)s->index,
                                           zone ? zone : g_zone, at, s->scale * HANG_SCALE, s->turn, strs, &s->item,
                                           &up) && up && digs_last_actor;
    if (!ok)
        ok = digs_spawn_item(c, s->room, 0x7d00000000000000ull | (uint64_t)s->index, zone ? zone : g_zone, at,
                             s->scale * HANG_SCALE, s->turn, strs, &s->item, &up) && up && digs_last_actor;
    if (!ok) return false;
    s->hang = true;
    memcpy(s->hang_at, at, sizeof s->hang_at);
    s->fly_actor = digs_last_actor;
    s->motion = 0;
    LOG("[shells] %s: item %s hangs in the apple tree at (%.2f, %.2f, %.2f)\n", name, s->mname, at[0], at[1], at[2]);
    return true;
}

// a hanging item stays at its spot (its physics body held there, a slow bob)
static void hang_step(Spot *s, uint64_t frame) {
    if (!s->item || !digs_actor_alive(s->item)) { s->hang = false; return; }
    float y = s->hang_at[1] + 0.04f * sinf((float)frame * 0.05f + (float)s->index);
    uint64_t m = s->motion;
    if (m) {
        const float *f = (const float *)(uintptr_t)(m + 0xa0);
        float at[3] = {f[0], f[1], f[2]};
        m = fly_motion(s->fly_actor, at) == m ? m : 0;
    } else {
        float at[3] = {s->hang_at[0], s->hang_at[1] + 0.25f, s->hang_at[2]};
        m = fly_motion(s->fly_actor, at);
    }
    s->motion = m;
    if (!m) return;
    float *f = (float *)(uintptr_t)(m + 0xa0), off = f[9];
    f[0] = s->hang_at[0], f[1] = y + off, f[2] = s->hang_at[2];
    f[12] = f[13] = f[14] = 0;
}

bool shell_spot_launch(Cpu *c, const char *name, const float from[3], uint32_t zone, uint64_t room, uint64_t rctrl) {
    Spot *s = apple_spot(name);
    if (!s || !g_pool_ready || flags_ext_get(s->flag_idx)) return false;
    hang_drop(c, s);
    memcpy(s->from, from, sizeof s->from);
    s->v0[0] = s->v0[1] = s->v0[2] = 0;  // (fly_start: the shell's own upward speed)
    s->zone = zone;
    if (s->room_ref) digs_drop_ref(c, &s->room_ref);
    s->droom = room && rctrl && digs_actor_alive(rctrl) ? room : 0;  // (the tree's room; else the landing tile's screen)
    if (s->droom) digs_hold_weak(&s->room_ref, rctrl);
    s->dropped = true;
    s->fly_queued = true;
    LOG("[shells] %s: the apple tree drops its item (%s) from (%.2f, %.2f, %.2f)\n", name, s->mname, from[0], from[1],
        from[2]);
    return true;
}

// the slot's item is out: about to fly, or its actor lying there (not hanging in the tree)
bool shell_spot_out(const char *name) {
    Spot *s = apple_spot(name);
    return s && !s->hang && (s->fly_queued || (s->item && digs_actor_alive(s->item)));
}

// the hanging item (if any): where it is, false: none
bool shell_spot_hanging(const char *name, float at[3]) {
    Spot *s = apple_spot(name);
    if (!s || !s->hang || !s->item || !digs_actor_alive(s->item)) return false;
    memcpy(at, s->hang_at, sizeof s->hang_at);
    return true;
}

// the item actor hangs in an apple tree: never picked up there, only once a bonk flies it out (quick_pickup.c asks
// before any pickup event)
bool shell_spot_hung_actor(uint64_t actor) {
    for (int i = 0; g_n > 0 && i < g_n; i++)
        if (g_spots[i].hang && g_spots[i].fly_actor == actor && g_spots[i].item && digs_actor_alive(g_spots[i].item))
            return true;
    return false;
}

// its tree is gone (the screen went): the item goes with it
void shell_spot_unhang(Cpu *c, const char *name) {
    Spot *s = apple_spot(name);
    if (s) hang_drop(c, s);
}

// game main thread, once per frame (runtime/hle/input.c)
void shell_spots_tick(Cpu *c, uint64_t frame) {
    if (load() <= 0) return;
    extern int game_in_play(void);
    if (!game_in_play()) return;
    uint64_t saved[31], sp = c->sp, pc = c->pc;
    __uint128_t v0 = c->v[0];
    memcpy(saved, c->x, sizeof saved);
    if (!g_pool_ready) {  // (the strings a dropped spot's item needs: in the string pool before any drop)
        bool ok = true;
        for (int i = 0; i < g_n && ok; i++) {
            Spot *s = &g_spots[i];
            if (s->owned || s->dig) continue;  // (a dug-up item's spawn adds its own)
            const char *strs[5] = {s->model, s->mname, s->entry, s->flag, s->shell ? "true" : "false"};
            for (int k = 0; k < 5 && ok; k++) ok = (s->pool[k] = digs_pool_index(c, strs[k])) >= 0;
        }
        ok = ok && (g_empty = digs_pool_index(c, "")) >= 0;
        g_pool_ready = ok;
        if (!ok) LOG("[shells] string pool full\n");
    }
    uint64_t mgr = LD64(GA(GAME_MGR)), scene = mgr ? LD64(mgr + 0x4a8) : 0;
    if (scene != g_scene_was) {  // a new scene: the holes (and items in them) are gone
        g_scene_was = scene;
        for (int i = 0; i < g_n; i++) {
            g_spots[i].dug = false;
            g_spots[i].dropped = false;
            g_spots[i].fly_queued = false;
            g_spots[i].fly_t = -1;
            g_spots[i].hole_k = -1;
            g_spots[i].droom = 0;
            g_spots[i].hang = false;
            if (g_spots[i].room_ref) digs_drop_ref(c, &g_spots[i].room_ref);
            if (g_spots[i].item && !digs_actor_alive(g_spots[i].item)) digs_drop_ref(c, &g_spots[i].item);
        }
    }
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
    uint64_t owned = ps ? LD64(ps + 0x88) : 0;  // (PlayerStatus +0x88: the Secret Seashells found, one bit per index)
    for (int i = 0; i < g_n && frame % 16 == 0; i++)
        if (g_spots[i].owned && g_spots[i].index < 64 && (owned >> g_spots[i].index & 1) && !flags_ext_get(g_spots[i].flag_idx)) {
            extern void flags_ext_set(int idx);
            flags_ext_set(g_spots[i].flag_idx);
            LOG("[shells] %s: its Secret Seashell (%d) found\n", g_spots[i].name, g_spots[i].index);
        }
    const char *lv = level_name();
    hole_beacons(lv);  // (each frame: a hole just up gets its drop index before a bomb can reach it)
    if (frame % 8 == 0) {  // a dropped item not picked up: once Link has left its screen (or level) it may drop again
        uint64_t pc0 = doorjump_player(frame);  // (a tree's / hole's: once its item is gone too - the screens around
        const float *p = pc0 ? (const float *)(uintptr_t)(pc0 + 0x80) : NULL;  // Link stay loaded, it may lie there)
        for (int i = 0; i < g_n; i++) {
            Spot *s = &g_spots[i];
            if (s->dropped && !(s->land && (s->fly_queued || digs_actor_alive(s->item))) && (strcmp(lv, s->level) ||
                               (!strcmp(s->level, "Field") && p && (floorf(p[0] / 15.0f) != floorf(s->x / 15.0f) ||
                                                                    floorf(p[2] / 12.0f) != floorf(s->z / 12.0f)))))
                s->dropped = false;
        }
    }
    for (int i = 0; i < g_n; i++) {  // a tree's / bomb hole's item: launched, then flying
        Spot *s = &g_spots[i];
        if (s->fly_queued && !strcmp(lv, s->level)) fly_start(c, s, frame);
        else if (s->fly_t >= 0) fly_step(s);
        else if (s->hang) hang_step(s, frame);
    }
    for (int i = 0; i < g_n; i++) {
        Spot *s = &g_spots[i];
        if (!s->dig || strcmp(lv, s->level)) continue;
        if (s->queued) {
            s->queued = false;
            spawn(c, s);
        } else if (frame % 8 == 0 && dug(s) && !flags_ext_get(s->flag_idx) && !digs_actor_alive(s->item)) {
            // (Link came back to an open hole - digsanity keeps it - or the item was left: up again; a room that
            // isn't loaded yet is tried again later)
            uint64_t pc0 = doorjump_player(frame);
            const float *p = pc0 ? (const float *)(uintptr_t)(pc0 + 0x80) : NULL;
            if (p && fabsf(p[0] - s->x) < 20.0f && fabsf(p[2] - s->z) < 16.0f) spawn(c, s);
        }
    }
    memcpy(c->x, saved, sizeof saved);
    c->v[0] = v0;
    c->sp = sp;
    c->pc = pc;
}
