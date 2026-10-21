// The randomizer seed's own cosmetics (Randomizer tab "Random cosmetics", seed option "random-cosmetics"): a seed
// made with it has its own set, picked from its name (the same every time it's played): a palette colour for the
// tunic, hearts, hair, skin and shield, one of the Season presets for leaves / grass / sand, and enemy colours "per
// seed". Only while that seed is being played (never in vanilla), and only for Cosmetics rows left on Default / Off:
// whatever the player chose in the Cosmetics tab wins.
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "cosmetic_colour.h"
#include "hle.h"
#include "rando.h"

static char g_dir[300] = "\1";  // (the seed the picks were made for; seeds switch in place)
static int g_picks[COL_SLOTS];
static bool g_on;

static uint32_t mix32(uint32_t h) {
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  // (draws on the render thread, the menu on the UI's)

static void refresh_locked(void) {
    const char *dir = rando_seed_dir();
    if (!strcmp(dir ? dir : "", g_dir)) return;
    snprintf(g_dir, sizeof g_dir, "%s", dir ? dir : "");
    g_on = false;
    memset(g_picks, 0, sizeof g_picks);
    char p[600];
    FILE *f = dir ? (snprintf(p, sizeof p, "%s/settings.json", dir), fopen(p, "rb")) : NULL;
    if (f) {
        static char buf[1 << 16];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        g_on = strstr(buf, "\"random-cosmetics\": true") != NULL;
    }
    if (!g_on) return;
    uint32_t h = 2166136261u;  // (the seed's folder name)
    for (const char *s = dir; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (int slot = COL_SLOT_TUNIC; slot <= COL_SLOT_SHIELD; slot++)
        g_picks[slot] = 1 + (int)(mix32(h ^ (uint32_t)slot * 0x9E3779B1u) % (COL_RANDOM - 1));
    // a Season preset (the menu's: Spring, Autumn, Winter, Cherry blossom) for leaves / grass / sand
    static const int season[4][3] = {{0, 0, 0}, {2, 3, 3}, {10, 10, 10}, {9, 0, 0}};
    const int *s = season[mix32(h ^ 0x5EA5u) % 4];
    g_picks[COL_SLOT_LEAVES] = s[0], g_picks[COL_SLOT_GRASS] = s[1], g_picks[COL_SLOT_SAND] = s[2];
    LOG("[cosmetics] seed's own cosmetics (%s)\n", dir);
}

static int seed_pick(int slot) {
    pthread_mutex_lock(&g_lock);
    refresh_locked();
    int p = slot >= 0 && slot < COL_SLOTS ? g_picks[slot] : 0;
    pthread_mutex_unlock(&g_lock);
    return p;
}

// The seed has its own cosmetics (enemy colours "per seed" while the row is Off)
bool cosmetics_seed_on(void) {
    pthread_mutex_lock(&g_lock);
    refresh_locked();
    bool on = g_on;
    pthread_mutex_unlock(&g_lock);
    return on;
}

__attribute__((constructor)) static void install(void) { g_col_seed_pick = seed_pick; }
