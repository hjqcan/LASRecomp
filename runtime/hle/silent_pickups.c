// Silent pickups (prototype): minor items picked up from the ground skip the item-get cutscene, in dungeons too
// (tools/rando/silent_pickups.py takes Link.GenericItemGetSequenceByKey out of their SinkingSword.bfevfl chains and plays
// a short pickup sound of the game's own instead). Heart pieces, Heart Containers and Secret Seashells get a card
// (runtime/ui/pickup_flyin.mm); rupees and recovery hearts none. The card items are added with index SILENT_BASE + 1 + their index:
// the AddItem hook (keysanity.c rt_add_item) calls rt_silent_add first, which gives the game back the real index (the
// add itself is the game's own, unchanged) and queues the fly-in. The game's own Secret Seashell actors (buried on a
// digsanity tile, at a Seashell spot, vanilla shells) get theirs through ItemCommon.bfevfl, which the generator gives a
// silent branch for Secret Seashells: their add comes with event flag unused0432 (MARK_FLAG) set. A Heart Container
// refills Link's health as its get sequence would have.
#include "../game_addresses.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../hle.h"
#include "../rando.h"

#define SILENT_BASE 3000  // (tools/rando/silent_pickups.py)
#define MARK_FLAG 432     // unused0432: set around the add of a native Secret Seashell (silent_pickups.py MARK_FLAG)
enum { IT_SEASHELL = 22, IT_HEART_PIECE = 58, IT_HEART_CONTAINER = 59 };
#define HP (GA(LIVE_SAVE) + 0x5544)          // s16 quarter hearts
#define SHELLS (GA(LIVE_SAVE) + 0x5548)      // u64 bits 0-49
#define PIECES (GA(LIVE_SAVE) + 0x5550)      // u64 bits 0-31
#define CONTAINERS (GA(LIVE_SAVE) + 0x5558)  // u16 bits 0-8

typedef struct { int item, count, index, held; } Pick;  // held: heart pieces / seashells once it is added
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Pick g_q[16];
static int g_qn;
static volatile int g_heal;  // a Heart Container came in: refill on the next game tick

RT_ON_RESET(silent_pickups_reset) {
    g_qn = 0;
    g_heal = 0;
}

// (keysanity.c rt_add_item, before anything else looks at the index)
void rt_silent_add(Cpu *c) {
    int index = (int)(int32_t)c->x[2], item = (int)(int32_t)c->x[0], count = (int)(int32_t)c->x[1];
    extern bool flags_ext_get(int idx);
    if (index >= SILENT_BASE && index < SILENT_BASE + 1000) {
        index -= SILENT_BASE + 1;
        c->x[2] = (uint64_t)(int64_t)index;
    } else if (!(item == IT_SEASHELL && flags_ext_get(MARK_FLAG))) {  // (a native shell's add: ItemCommon.bfevfl)
        return;
    }
    if (item == IT_HEART_CONTAINER) g_heal = 1;
    int held = 0;  // (counted now: a card shows its own count, even with the next pickup already in)
    if (item == IT_HEART_PIECE)
        held = __builtin_popcountll((LD64(PIECES) | (index >= 0 && index < 32 ? 1ull << index : 0)) & 0xFFFFFFFFull);
    else if (item == IT_SEASHELL)
        held = __builtin_popcountll((LD64(SHELLS) | (index >= 0 && index < 50 ? 1ull << index : 0)) & ((1ull << 50) - 1));
    pthread_mutex_lock(&g_lock);
    if (g_qn < (int)(sizeof g_q / sizeof g_q[0])) g_q[g_qn++] = (Pick){item, count, index, held};
    pthread_mutex_unlock(&g_lock);
    LOG("[silent] item %d x%d (index %d): no item-get, fly-in\n", item, count, index);
}

// the next queued pickup (render thread: pickup_flyin.mm); false: none
bool silent_pickup_next(int *item, int *count, int *held) {
    bool got = false;
    pthread_mutex_lock(&g_lock);
    if (g_qn) {
        *item = g_q[0].item, *count = g_q[0].count, *held = g_q[0].held;
        memmove(g_q, g_q + 1, sizeof(Pick) * (size_t)--g_qn);
        got = true;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

static int max_hp(void) {  // quarter hearts, as GetMaxHealth() computes it
    return 12 + 4 * (__builtin_popcount(LD16(CONTAINERS) & 0x1FF) + __builtin_popcountll(LD64(PIECES) & 0xFFFFFFFFull) / 4);
}


// Secret Seashells this seed has in all (settings.json "seashell-total", written by tools/rando/las_headless.py;
// older seeds and vanilla: 50)
int silent_seashell_total(void) {
    static char seed[512];
    static int total = 50;
    const char *dir = rando_seed_dir();
    if (!dir) return 50;
    if (strcmp(seed, dir)) {
        snprintf(seed, sizeof seed, "%s", dir);
        total = 50;
        char p[600];
        snprintf(p, sizeof p, "%s/settings.json", dir);
        FILE *f = fopen(p, "rb");
        if (f) {
            static char buf[1 << 17];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
            const char *k = strstr(buf, "\"seashell-total\":");
            if (k && atoi(k + 17) > 0) total = atoi(k + 17);
        }
    }
    return total;
}

// game thread, every frame (warp.c give_item_tick): a silent Heart Container's refill.
// Dev mode only (a source checkout: rt_dev_mode): LARECOMP_FLYIN_SHOTS=<from>:<every>:<count> (test): window shots (game + overlays) into build/frames/flyin_<frame>.png
void silent_pickups_tick(uint64_t frame) {
    static long from = -2, every, count;
    if (from == -2) {
        from = -1;
        extern bool rt_dev_mode(void);
        const char *e = rt_dev_mode() ? ENV("LARECOMP_FLYIN_SHOTS") : NULL;
        if (e && sscanf(e, "%ld:%ld:%ld", &from, &every, &count) != 3) from = -1;
        if (from >= 0) mkdir("build/frames", 0755);
    }
    if (from >= 0 && (long)frame >= from && every > 0 && ((long)frame - from) % every == 0 && ((long)frame - from) / every < count) {
        extern char g_window_shot_request[256];  // (runtime/nvn/metal.m: saved with the next frame)
        snprintf(g_window_shot_request, sizeof g_window_shot_request, "build/frames/flyin_%05llu.png", (unsigned long long)frame);
    }
    static long dump_at = -2;  // LARECOMP_SILENT_DUMP_AT=<frame>[:<flag>,...] (test, dev mode): what the save holds then
    if (dump_at == -2) {
        extern bool rt_dev_mode(void);
        dump_at = rt_dev_mode() && ENV("LARECOMP_SILENT_DUMP_AT") ? atol(ENV("LARECOMP_SILENT_DUMP_AT")) : -1;
    }
    if (dump_at >= 0 && (long)frame == dump_at) {
        extern bool flags_ext_get(int idx);
        LOG("[silent] save: pieces %#llx (%d) shells %#llx (%d) containers %#x rupees %u health %d / %d\n",
            (unsigned long long)LD64(PIECES), __builtin_popcountll(LD64(PIECES) & 0xFFFFFFFFull), (unsigned long long)LD64(SHELLS),
            __builtin_popcountll(LD64(SHELLS) & ((1ull << 50) - 1)), LD16(CONTAINERS), LD32(GA(LIVE_SAVE) + 0x5540),
            (int)(int16_t)LD16(HP), max_hp());
        const char *f = strchr(ENV("LARECOMP_SILENT_DUMP_AT"), ':');
        for (; f && *f; f = strchr(f, ',')) {
            int idx = atoi(++f);
            LOG("[silent] flag %d: %d\n", idx, flags_ext_get(idx));
        }
    }
    if (!g_heal) return;
    g_heal = 0;
    int16_t *hp = (int16_t *)(uintptr_t)HP;
    if (*hp > 0) *hp = (int16_t)max_hp();
    LOG("[silent] heart container: health refilled to %d\n", *hp);
}
