// The randomizer seed's own cosmetics (Randomizer tab "Random cosmetics", seed option "random-cosmetics"): a seed
// made with it has its own set, picked from its seed (the same every time it's played): a fully random colour (any
// hue, saturation and brightness) for each of the tunic, hearts, hair, skin, shield, leaves, grass and sand, and enemy
// colours "per seed" (the screen filter stays off: it isn't a colour). Only while that seed is being played (never in vanilla), and only for
// Cosmetics rows left on Default / Off: whatever the player chose in the Cosmetics tab wins.
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "cosmetic_colour.h"
#include "hle.h"
#include "rando.h"

static char g_dir[300] = "\1";  // (the seed the picks were made for; seeds switch in place)
static uint32_t g_rgb[COL_SLOTS];  // (0xRRGGBB, one per row)
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
    memset(g_rgb, 0, sizeof g_rgb);
    char p[600];
    static char buf[1 << 16];
    FILE *f = dir ? (snprintf(p, sizeof p, "%s/settings.json", dir), fopen(p, "rb")) : NULL;
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        g_on = strstr(buf, "\"random-cosmetics\": true") != NULL;
    }
    if (!g_on) return;
    // (the seed's "seed" value: its RNG seed; an older settings.json without one: the folder)
    const char *name = dir, *end = dir + strlen(dir), *k = strstr(buf, "\"seed\": \"");
    if (k && strchr(k + 9, '"')) name = k + 9, end = strchr(name, '"');
    uint32_t h = 2166136261u;
    for (const char *s = name; s < end; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    for (int slot = 0; slot < COL_SLOTS; slot++)
        g_rgb[slot] = mix32(h ^ (uint32_t)slot * 0x9E3779B1u) & 0xFFFFFF;
    LOG("[cosmetics] seed's own cosmetics (%s)\n", dir);
}

static int seed_pick(int slot, uint32_t *rgb) {
    pthread_mutex_lock(&g_lock);
    refresh_locked();
    int p = g_on && slot >= 0 && slot < COL_SLOTS ? COL_CUSTOM : COL_DEFAULT;
    if (p) *rgb = g_rgb[slot];
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
