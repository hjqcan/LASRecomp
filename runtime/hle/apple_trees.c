// Applesanity: each overworld apple tree holds 3 checks, one per apple (tools/rando/apple_trees.py writes
// rando/<seed>/apple_trees.tsv; no file: vanilla trees).
//
// An apple tree (ObjTree, +0x4c0 set) rolls its ItemDrop row 3 times when it loads; with Applesanity every roll is a
// red apple. Its apples hang from +0x4c8: three handles (actor, control block) at +0x4f8, +0x508, +0x518, one per
// slot. A bonk (the tree's handler, main+0xcb04e0) knocks every live apple down (main+0xcb18f0) and sets +0x528 until
// the tree loads again (its screen streams out and back in: a new tree, 3 new apples).
//   - a slot already found, or whose item still lies out there, has its apple taken away as soon as it hangs there:
//     the apples left are the checks left, a finished tree is bare (the bonk skips a dead apple)
//   - an open slot's apple is swapped for its item, hanging where the apple was (shell_spots.c holds it there): the
//     tree shows what it holds (a trap shows another item's look: apple_trees.py)
//   - on a bonk (+0x528 goes 0 -> 1) each hanging item flies out from the crown to its own tile (shell_spots.c: the
//     same fly-out as a Shellsanity tree, the rows are its spots)
// apple_trees.tsv: shell_spots.tsv's columns (kind "apple") + tree actor key (hex), slot.
#include "../game_addresses.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../rando.h"

#define T_APPLE 0x136
#define T_GREEN_APPLE 0x135
#define MAXROWS 24
#define MAXTREES 8
#define DEAD (1u << 30)  // actor +0x40: killed

bool flags_ext_get(int idx);
bool shell_spot_launch(Cpu *c, const char *name, const float from[3], uint32_t zone, uint64_t room, uint64_t rctrl);
bool shell_spot_out(const char *name);
bool shell_spot_hang(Cpu *c, const char *name, const float at[3], uint32_t zone, uint64_t room, uint64_t rctrl);

bool shell_spot_hanging(const char *name, float at[3]);
void shell_spot_unhang(Cpu *c, const char *name);

typedef struct {
    char name[48];
    int flag_idx, slot, tree;
} Row;
typedef struct {
    uint64_t key, actor;
    uint8_t dropped;
    int row[3];      // the slot's row (-1: none)
    bool has_foot;   // where its apples were made (the tree's foot, 0.5 up): its items hang from there
    float foot[3];
    uint32_t zone;
} Tree;
// (the tree's own room: actor +0x48 room, +0x50 its control block - its items go there, drawn with the tree even
// on a screen the game doesn't count as Link's)
// where each slot's item hangs, from the tree's foot: on the crown's front (toward the camera) and top, spread like
// the apples, so the leaves don't hide it - and out of Link's reach (it can't be picked up there anyway: shell_spots.c)
static const float k_hang[3][3] = {{0.15f, 1.7f, 1.6f}, {-1.0f, 2.1f, 1.25f}, {1.05f, 2.35f, 1.05f}};
static Row g_rows[MAXROWS];
static Tree g_trees[MAXTREES];
static int g_nrows = -1, g_ntrees;
static pthread_mutex_t g_load_lock = PTHREAD_MUTEX_INITIALIZER;

// where apples were just made (the factory, any thread) and their zone: a tree's items go into the same one
static struct { float x, y, z; uint32_t zone; } g_made[32];
static unsigned g_made_n;

RT_ON_RESET(apple_trees_reset) {
    g_nrows = -1;
    g_ntrees = 0;
    memset(g_trees, 0, sizeof g_trees);
    memset(g_made, 0, sizeof g_made);
}

static int load(void) {
    if (__atomic_load_n(&g_nrows, __ATOMIC_ACQUIRE) >= 0) return g_nrows;
    pthread_mutex_lock(&g_load_lock);
    if (g_nrows < 0) {
        int n = 0;
        const char *dir = rando_seed_dir();
        char path[512], line[1024];
        snprintf(path, sizeof path, "%s/apple_trees.tsv", dir ? dir : ".");
        FILE *f = dir ? fopen(path, "r") : NULL;
        while (f && fgets(line, sizeof line, f) && n < MAXROWS) {
            if (line[0] == '#') continue;
            char *col[18], *save = NULL;
            int nc = 0;
            for (char *t = strtok_r(line, "\t\n", &save); t && nc < 18; t = strtok_r(NULL, "\t\n", &save)) col[nc++] = t;
            if (nc < 18) continue;
            Row *r = &g_rows[n];
            strlcpy(r->name, col[0], sizeof r->name);
            r->flag_idx = atoi(col[12]);
            r->slot = atoi(col[17]);
            uint64_t key = strtoull(col[16], NULL, 16);
            if (r->slot < 0 || r->slot > 2) continue;
            int t = 0;
            while (t < g_ntrees && g_trees[t].key != key) t++;
            if (t == g_ntrees) {
                if (t == MAXTREES) continue;
                g_trees[t].key = key;
                g_trees[t].row[0] = g_trees[t].row[1] = g_trees[t].row[2] = -1;
                g_ntrees++;
            }
            r->tree = t;
            g_trees[t].row[r->slot] = n++;
        }
        if (f) fclose(f);
        if (n) LOG("[apples] %d apple tree slots hold the seed's items (%d trees)\n", n, g_ntrees);
        __atomic_store_n(&g_nrows, n, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_load_lock);
    return g_nrows;
}

// the factory's create hook (souls.c): note where an apple is made (its tree's foot) and its zone
bool rt_apple_skip(uint32_t type, uint64_t info) {
    if ((type != T_APPLE && type != T_GREEN_APPLE) || LD64(info) || load() <= 0) return false;
    const float *p = (const float *)(uintptr_t)(info + 0x180);
    unsigned i = __atomic_fetch_add(&g_made_n, 1, __ATOMIC_RELAXED) % 32;
    g_made[i].x = p[0], g_made[i].y = p[1], g_made[i].z = p[2], g_made[i].zone = LD32(info + 0xc);
    return false;
}

static uint32_t zone_near(const float at[3]) {
    for (int i = 0; i < 32; i++)
        if (g_made[i].zone && fabsf(g_made[i].x - at[0]) < 3 && fabsf(g_made[i].z - at[2]) < 3) return g_made[i].zone;
    return 0;
}

// the tree's foot: where its apples were made (the factory notes them), found from one of its apples
static bool foot_of(const float at[3], Tree *tr) {
    for (int i = 0; i < 32; i++)
        if (g_made[i].zone && fabsf(g_made[i].x - at[0]) < 3 && fabsf(g_made[i].z - at[2]) < 3) {
            tr->foot[0] = g_made[i].x, tr->foot[1] = g_made[i].y, tr->foot[2] = g_made[i].z;
            tr->zone = g_made[i].zone;
            return true;
        }
    return false;
}

static bool live(uint64_t a) { return a && !(LD32(a + 0x40) & DEAD); }

// an apple's place: its physics body's centre (actor +0x80 -> +0x170 -> +0x670, +0xa0), as for the flying items
static bool apple_pos(uint64_t a, float out[3]) {
    uint64_t col = LD64(a + 0x80), b = col ? LD64(col + 0x170) : 0, m = b ? LD64(b + 0x670) : 0;
    if (!m) return false;
    memcpy(out, (const void *)(uintptr_t)(m + 0xa0), 3 * sizeof(float));
    return true;
}

static void kill(Cpu *c, uint64_t a) {
    uint64_t saved[31], sp = c->sp, pc = c->pc;
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = a;
    guest_call(c, GA(ACTOR_KILL));
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp;
    c->pc = pc;
}

// LARECOMP_APPLE_BONK=<frame>,... (test): the tree's bonk handler (GA(TREE_BONK)), as Link dashing into it, on each
// loaded apple tree at those frames
static void bonk_test(Cpu *c, uint64_t frame, uint64_t tree) {
    const char *e = ENV("LARECOMP_APPLE_BONK");
    for (const char *q = e; q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : "") {
        if ((uint64_t)strtoull(q, NULL, 10) != frame) continue;
        uint64_t saved[31], sp = c->sp, pc = c->pc;
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = tree;
        guest_call(c, GA(TREE_BONK));
        memcpy(c->x, saved, sizeof saved);
        c->sp = sp;
        c->pc = pc;
        LOG("[apples] f%llu test bonk\n", (unsigned long long)frame);
    }
}

// game main thread, once per frame (runtime/hle/input.c)
void apple_trees_tick(Cpu *c, uint64_t frame) {
    extern int game_in_play(void);
    if (load() <= 0 || !game_in_play()) return;
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR)), map = LD64(GA(ACTOR_REGISTRY));
    if (!ps || strcmp((const char *)(uintptr_t)ps, "Field") || !map) {
        for (int t = 0; t < g_ntrees; t++) g_trees[t].actor = 0;
        return;
    }
    uint64_t found[MAXTREES] = {0};
    int n = 0;
    for (uint64_t node = LD64(map + 0x10); node && n < 8192; node = LD64(node), n++)
        for (int t = 0; t < g_ntrees; t++)
            if (LD64(node + 0x10) == g_trees[t].key) found[t] = LD64(node + 0x18);
    for (int t = 0; t < g_ntrees; t++) {
        Tree *tr = &g_trees[t];
        uint64_t tree = found[t];
        if (tree != tr->actor) {  // (a new tree: new apples, not dropped yet; the old one's hanging items go)
            for (int k = 0; k < 3; k++)
                if (tr->row[k] >= 0) shell_spot_unhang(c, g_rows[tr->row[k]].name);
            tr->actor = tree;
            tr->dropped = tree ? LD8(tree + 0x528) : 0;
            tr->has_foot = false;
        }
        if (!tree || !LD8(tree + 0x4c0)) continue;
        if (ENV("LARECOMP_APPLE_BONK")) bonk_test(c, frame, tree);
        uint8_t d = LD8(tree + 0x528);
        bool bonk = d && !tr->dropped;
        tr->dropped = d;
        for (int k = 0; k < 3; k++) {
            uint64_t a = LD64(tree + 0x4f8 + 0x10 * k);
            const Row *r = tr->row[k] >= 0 ? &g_rows[tr->row[k]] : NULL;
            if (!r) continue;
            float at[3];
            if (bonk && shell_spot_hanging(r->name, at)) {  // (its item falls out of the crown)
                shell_spot_launch(c, r->name, at, zone_near(at), LD64(tree + 0x48), LD64(tree + 0x50));
                continue;
            }
            bool done = flags_ext_get(r->flag_idx) || shell_spot_out(r->name);  // (found, or its item still out)
            if (done) {
                if (live(a)) {
                    kill(c, a);
                    LOG("[apples] %s: %s - its apple taken away\n", r->name,
                        flags_ext_get(r->flag_idx) ? "found" : "item still out");
                }
                continue;
            }
            if (bonk) {  // (no item hanging: it flies out from the falling apple)
                if (live(a) && apple_pos(a, at) && shell_spot_launch(c, r->name, at, zone_near(at), LD64(tree + 0x48), LD64(tree + 0x50))) kill(c, a);
                continue;
            }
            if (!tr->has_foot && live(a) && apple_pos(a, at)) tr->has_foot = foot_of(at, tr);
            if (!tr->has_foot) continue;
            // its item hangs in the crown instead of the apple, as soon as the tree is there (on any loaded screen)
            if (!shell_spot_hanging(r->name, at)) {
                float p[3] = {tr->foot[0] + k_hang[k][0], tr->foot[1] + k_hang[k][1], tr->foot[2] + k_hang[k][2]};
                shell_spot_hang(c, r->name, p, tr->zone, LD64(tree + 0x48), LD64(tree + 0x50));
            }
            if (live(a) && shell_spot_hanging(r->name, at)) kill(c, a);
        }
    }
}
