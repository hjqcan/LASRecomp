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
//         (ItemDrop LARShellSpot<n>; a bomb hole drops through ItemDrop Seashell<its index>, retagged - see hole_beacons
//         for its sensor beacon). The
//         factory's spawn info is rewritten before the type is read (rt_shell_swap, from enemy_random.c's
//         rt_enemy_spawn): a SinkingSword item actor with the item's model, flow entry and flag in its type-5 (string
//         pool) parameters. (kind 'land': the shell falls out of a tree's crown / jumps out of a bomb hole - the
//         item, which doesn't, is put on the ground beside it)
// Picking it up runs its entry in SinkingSword.bfevfl (LAS's item-get: held up, the game's message box) and sets the
// spot's own flag; a spot whose flag is set gives nothing again, and one whose item already dropped gives no second
// one (a second bonk of the tree, say) until Link has left its screen. A 'shell' row (a Secret Seashell there, also on a
// digsanity tile) only has its found flag set here once that seashell is owned (the tracker).
//
// shell_spots.tsv: location, seashell index (vanilla; a 'shell' row: the placed shell's), kind (dig / drop / land /
// shell), level, room, x, y, z, model path, model name, flow entry, flag symbol, flag index, seashell (1/0), scale,
// turn (degrees); x y z: a dig spot's tile centre, where a 'land' spot's item lands
#include "../game_addresses.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../rando.h"

#define MAXSPOTS 128  // (31 spots + a "shell" row per digsanity tile holding a Secret Seashell: up to 81)
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
int digs_pool_index(Cpu *c, const char *str);

typedef struct {
    char name[48], level[32], room[32], model[64], mname[48], entry[48], flag[32];
    int index, flag_idx, dig, land, hole, owned, shell;  // owned: a 'shell' row
    float x, y, z, scale, turn;
    uint64_t key;         // a bomb hole's actor (its level key)
    int pool[5];          // the strings' string pool indices (-1: not yet)
    uint64_t item;        // dug-up item actor (control block, weak ref held)
    bool dug;             // dug this visit (the hole lasts until the scene goes)
    bool dropped;         // its item dropped this visit: a second bonk / lift / bomb gives no second one
    bool queued;
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
    snprintf(path, sizeof path, "%s/shell_spots.tsv", dir);
    FILE *f = fopen(path, "r");
    if (!f) return __atomic_store_n(&g_n, 0, __ATOMIC_RELEASE), 0;
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
        s->land = s->hole || !strcmp(col[2], "land");
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
        s->key = nc > 16 ? strtoull(col[16], NULL, 16) : 0;
        for (int k = 0; k < 5; k++) s->pool[k] = -1;
    }
    if (n == MAXSPOTS && fgets(line, sizeof line, f)) LOG("[shells] warning: more than %d rows, the rest ignored\n", MAXSPOTS);
    fclose(f);
    LOG("[shells] %d Secret Seashell spots hold the seed's items (%s)\n", n, path);
    __atomic_store_n(&g_n, n, __ATOMIC_RELEASE);
    return n;
}

static const char *level_name(void) {
    uint64_t ps = LD64(GA_PLAYER_STATUS_PTR);
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

// before the factory reads the type (enemy_random.c rt_enemy_spawn): a dropped Secret Seashell becomes the item
void rt_shell_swap(uint64_t info) {
    Spot *s = info ? spot_of(info) : NULL;
    if (!s) return;
    if (flags_ext_get(s->flag_idx) || !g_pool_ready || s->dropped) {
        LOG("[shells] %s: %s\n", s->name, !g_pool_ready ? "not ready (nothing)" : s->dropped ? "already dropped here (nothing)"
                                                                                    : "already found (nothing)");
        return;  // (rt_shell_skip: no actor)
    }
    s->dropped = true;  // (until the scene goes: the item lies there until picked up)
    ST16(info + 8, T_SINKING_SWORD);
    for (int k = 0; k < 8; k++) {
        ST32(info + 0x10 + 8 * k, (uint32_t)(k < 5 ? s->pool[k] : g_empty));
        ST8(info + 0x14 + 8 * k, 5);
    }
    float *tp = (float *)(uintptr_t)(info + 0x180), a = s->turn * (float)M_PI / 360.0f;
    if (s->land) tp[0] = s->x, tp[1] = s->y, tp[2] = s->z;  // (out of a crown / a hole: on the ground beside it)
    tp[4] = 0, tp[5] = sinf(a), tp[6] = 0, tp[7] = cosf(a);
    tp[8] = tp[9] = tp[10] = s->scale > 0 ? s->scale : 1.0f;
    LOG("[shells] %s: the bush / rock / pot / tree / hole gives its item (%s) at (%.2f, %.2f, %.2f)\n", s->name,
        s->mname, tp[0], tp[1], tp[2]);
}

// the factory's create-function hook (souls.c): nothing for a retagged drop already found (or before the strings are in)
bool rt_shell_skip(uint32_t type, uint64_t info) {
    if (type != T_HEART_PIECE && type != T_SHELL) return false;
    Spot *s = spot_of(info);
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

// A bomb hole holding something else keeps its seashell index (with any other - or that shell owned - it is gone at
// once and drops nothing; ItemDrop Seashell<n> drops the tagged Heart Piece), so it makes its Seashell Sensor beacon
// (AreaShellDrop +0x2b8: the ShellBeacon component). The beacon is switched off here once the hole is up (+0x14c: 0 =
// the sensor's check skips it, f_70008dd860), so the sensor doesn't ring for it. The level's placed actors by key:
// GA_ACTOR_REGISTRY (pets.c).
static void hole_beacons(const char *lv) {
    uint64_t map = LD64(GA_ACTOR_REGISTRY);
    for (int i = 0; i < g_n && map; i++) {
        Spot *s = &g_spots[i];
        if (!s->hole || !s->key || strcmp(lv, s->level)) continue;
        int n = 0;
        for (uint64_t node = LD64(map + 0x10); node && n < 8192; node = LD64(node), n++) {
            if (LD64(node + 0x10) != s->key) continue;
            uint64_t a = LD64(node + 0x18), b = a ? LD64(a + 0x2b8) : 0;
            if (b && LD8(b + 0x14c)) {
                ST8(b + 0x14c, 0);
                LOG("[shells] %s: bomb hole's sensor beacon off\n", s->name);
            }
            break;
        }
    }
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
    uint64_t mgr = LD64(GA_GAME_MGR), scene = mgr ? LD64(mgr + 0x4a8) : 0;
    if (scene != g_scene_was) {  // a new scene: the holes (and items in them) are gone
        g_scene_was = scene;
        for (int i = 0; i < g_n; i++) {
            g_spots[i].dug = false;
            g_spots[i].dropped = false;
            if (g_spots[i].item && !digs_actor_alive(g_spots[i].item)) digs_drop_ref(c, &g_spots[i].item);
        }
    }
    uint64_t ps = LD64(GA_PLAYER_STATUS_PTR);
    uint64_t owned = ps ? LD64(ps + 0x88) : 0;  // (PlayerStatus +0x88: the Secret Seashells found, one bit per index)
    for (int i = 0; i < g_n && frame % 16 == 0; i++)
        if (g_spots[i].owned && g_spots[i].index < 64 && (owned >> g_spots[i].index & 1) && !flags_ext_get(g_spots[i].flag_idx)) {
            extern void flags_ext_set(int idx);
            flags_ext_set(g_spots[i].flag_idx);
            LOG("[shells] %s: its Secret Seashell (%d) found\n", g_spots[i].name, g_spots[i].index);
        }
    const char *lv = level_name();
    if (frame % 8 == 0) hole_beacons(lv);
    if (frame % 8 == 0) {  // a dropped item not picked up: once Link has left its screen (or level) it may drop again
        uint64_t pc0 = doorjump_player(frame);
        const float *p = pc0 ? (const float *)(uintptr_t)(pc0 + 0x80) : NULL;
        for (int i = 0; i < g_n; i++) {
            Spot *s = &g_spots[i];
            if (s->dropped && (strcmp(lv, s->level) ||
                               (!strcmp(s->level, "Field") && p && (floorf(p[0] / 15.0f) != floorf(s->x / 15.0f) ||
                                                                    floorf(p[2] / 12.0f) != floorf(s->z / 12.0f)))))
                s->dropped = false;
        }
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
