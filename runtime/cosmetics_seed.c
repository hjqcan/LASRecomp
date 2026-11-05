// The randomizer seed's own cosmetics (Randomizer tab "Random cosmetics", seed option "random-cosmetics"): a seed
// made with it has its own set, picked from its seed (the same every time it's played): a fully random colour (any
// hue, saturation and brightness) for each of the tunic, hearts, hair, skin, shield, leaves, grass, sand, water,
// fences, walls and houses, enemy colours "per seed" and a weather (clear, rain, snow or storm: runtime/nvn/weather.c;
// the screen filter off: it isn't a colour). The first time such a seed is played its set is copied into the Cosmetics
// tab's rows (g_settings.cosmetics_seed remembers which seed did it), and from then on they are the player's own to
// change; the tab's "Seed's look" copies them in again.
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "cosmetic_colour.h"
#include "hle.h"
#include "rando.h"
#include "settings.h"

static char g_dir[300] = "\1";  // (the seed the picks were made for; seeds switch in place)
static uint32_t g_rgb[COL_SLOTS];  // (0xRRGGBB, one per row)
static bool g_on;
static int g_weather;  // (0 clear, 1 rain, 2 snow, 3 storm)

static uint32_t mix32(uint32_t h) {
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  // (draws on the render thread, the menu on the UI's)

// On the start screen: the seed PLAY would start (g_settings.rando_seed), read from its folder - rando_seed_dir() would
// apply its patches before the game is loaded (the Cosmetics tab's colour rows crashed there; hookshot.c does the same)
extern int g_start_screen;  // warp.c
static void refresh_locked(void) {
    char sdir[256];
    const char *dir = NULL;
    if (!g_start_screen) dir = rando_seed_dir();
    else if (g_settings.rando_seed[0]) dir = (snprintf(sdir, sizeof sdir, "rando/%s", g_settings.rando_seed), sdir);
    if (!strcmp(dir ? dir : "", g_dir)) return;
    snprintf(g_dir, sizeof g_dir, "%s", dir ? dir : "");
    g_on = false;
    g_weather = 0;
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
    g_weather = (int)(mix32(h ^ 0x57EA7E12u) % 4);
    LOG("[cosmetics] seed's own cosmetics (%s)\n", dir);
}

// The seed's set -> the Cosmetics tab's rows (the weather's 0..3 are WEATHER_OFF..WEATHER_STORM)
static void apply_locked(void) {
    CosColour *rows[COL_SLOTS] = {&g_settings.tunic, &g_settings.hearts, &g_settings.hair, &g_settings.skin,
                                  &g_settings.shield, &g_settings.leaves, &g_settings.grass, &g_settings.sand,
                                  &g_settings.water, &g_settings.fences, &g_settings.walls, &g_settings.houses};
    for (int slot = 0; slot < COL_SLOTS; slot++) rows[slot]->pick = COL_CUSTOM, rows[slot]->rgb = g_rgb[slot];
    g_settings.enemy_colours = 1;
    g_settings.screen_filter = 0;
    g_settings.weather = g_weather;
    snprintf(g_settings.cosmetics_seed, sizeof g_settings.cosmetics_seed, "%s", g_dir);
    LOG("[cosmetics] the seed's own cosmetics copied into the Cosmetics tab (%s)\n", g_dir);
}

static void check_locked(void) {
    refresh_locked();
    if (g_on && strcmp(g_settings.cosmetics_seed, g_dir)) apply_locked();  // (a seed played for the first time)
}

static void check(void) {
    pthread_mutex_lock(&g_lock);
    check_locked();
    pthread_mutex_unlock(&g_lock);
}

// The seed being played (or PLAY's, on the start screen) has its own cosmetics
bool cosmetics_seed_on(void) {
    pthread_mutex_lock(&g_lock);
    check_locked();
    bool on = g_on;
    pthread_mutex_unlock(&g_lock);
    return on;
}

// "Seed's look": its set into the rows again
void cosmetics_seed_apply(void) {
    pthread_mutex_lock(&g_lock);
    refresh_locked();
    if (g_on) apply_locked();
    pthread_mutex_unlock(&g_lock);
}

__attribute__((constructor)) static void install(void) { g_col_seed_check = check; }
