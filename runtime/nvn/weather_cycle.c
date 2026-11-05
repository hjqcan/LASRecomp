// The randomizer's weather cycle (seed option "weather-cycle"): the overworld's weather changes on its own, Sunny ->
// Rain / Snow -> Storm / Blizzard and back, and the Cosmetics tab's Weather row is locked while it's on (the seed
// has it). Its lightning shocks Link and a blizzard's gusts push him (weather_lightning_harm / weather_wind_push).
//   Sunny 6-12 min, Rain / Snow 6-12 min, Storm / Blizzard 4-8 min (each phase its own random length)
//   Sunny          -> Rain or Snow (even)
//   Rain / Snow    -> its storm (Rain: Storm, Snow: Blizzard) 80%, less 20 for each storm this spell had so far (0
//                     after four); the other of Rain / Snow 10% (5% after a storm); Sunny the rest - Sunny ends the spell
//   Storm/Blizzard -> its own kind again (Storm: Rain, Blizzard: Snow)
// A change fades over FADE s: the old weather eases down, the new one up (down to nothing to or from Sunny). Not
// seeded: random every time. The clock runs while the game is in play and draws its scene - indoors and in
// dungeons too (the weather goes on outside), not on the file select, the pause menu or ours (no scene composite).
// Saved with each save slot: savedataN.bin -> savedataN.bin.weather, written with the save and matched to it by the
// save's body CRC like the extra event flags (runtime/hle/flags_ext.c); a save without one (a new file, an older
// save) starts Sunny.
// Tests: LARECOMP_WEATHER_CYCLE=1 on without a seed; LARECOMP_WEATHER_CYCLE_SPEED=<x> the clock x times faster;
// LARECOMP_WEATHER_CYCLE_START=rain|snow|storm|blizzard (a new file starts there, 20 s left)
#include "weather.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../game_addresses.h"
#include "../hle.h"
#include "../rando.h"
#include "../settings.h"

extern int game_in_play(void);  // runtime/hle/warp.c

#define BODY_CRC 0x5EC4
#define FADE 24.0f
#define MAGIC 0x31435857u  // "WXC1"

typedef struct {
    uint32_t magic, crc;   // (the save's body CRC this goes with)
    int32_t kind, from;    // the weather (WEATHER_OFF: Sunny) and the one fading out (-1: none)
    int32_t storms;        // storms this spell so far
    float left, fade;      // s left in this phase; s into the change (FADE: done)
    uint32_t rng;
} Cycle;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Cycle g_c;
static bool g_have;          // (g_c is set up)
static double g_scene_at;    // host time of the last scene composite (weather_cycle_scene)
static uint32_t g_live_crc;  // body CRC of the save in play when we last matched it
static int g_was_in_play;
static struct { char path[1024]; Cycle c; } g_slots[8];  // (what each slot's file held, as read)
static int g_nslots;

static double host_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static float frand(void) {
    uint32_t h = g_c.rng += 0x9E3779B9u;
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return (h >> 8) / 16777216.0f;
}
static float length(int kind) {  // (s)
    return 60 * (kind == WEATHER_STORM || kind == WEATHER_BLIZZARD ? 4 + 4 * frand() : 6 + 6 * frand());
}
static const char *name(int kind) {
    return (const char *const[]){"sunny", "rain", "snow", "storm", "blizzard"}[kind >= 0 && kind < 5 ? kind : 0];
}

static void fresh(void) {
    memset(&g_c, 0, sizeof g_c);
    g_c.magic = MAGIC, g_c.from = -1, g_c.fade = FADE;
    g_c.rng = (uint32_t)(host_now() * 1000) ^ (uint32_t)(uintptr_t)&g_c;
    g_c.kind = WEATHER_OFF, g_c.left = length(WEATHER_OFF);
    const char *e = getenv("LARECOMP_WEATHER_CYCLE_START");
    for (int i = 1; e && i < 5; i++) if (!strcmp(e, name(i))) g_c.kind = i, g_c.left = 20;
    g_have = true;
}

static void next(void) {
    int k = g_c.kind, n;
    if (k == WEATHER_OFF) n = frand() < 0.5f ? WEATHER_RAIN : WEATHER_SNOW, g_c.storms = 0;
    else if (k == WEATHER_STORM) n = WEATHER_RAIN, g_c.storms++;
    else if (k == WEATHER_BLIZZARD) n = WEATHER_SNOW, g_c.storms++;
    else {
        float storm = g_c.storms >= 4 ? 0 : 0.8f - 0.2f * g_c.storms, swap = g_c.storms ? 0.05f : 0.1f, r = frand();
        n = r < storm ? (k == WEATHER_RAIN ? WEATHER_STORM : WEATHER_BLIZZARD)
          : r < storm + swap ? (k == WEATHER_RAIN ? WEATHER_SNOW : WEATHER_RAIN) : WEATHER_OFF;
    }
    LOG("[weather] cycle: %s -> %s (storms this spell %d)\n", name(k), name(n), g_c.storms);
    g_c.from = k, g_c.kind = n, g_c.fade = 0, g_c.left = length(n);
}

static int g_on = -1;
static char g_dir[300] = "\1";
bool weather_cycle_on(void) {
    static int env = -1;
    if (env < 0) env = getenv("LARECOMP_WEATHER_CYCLE") && atoi(getenv("LARECOMP_WEATHER_CYCLE"));
    if (env) return true;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;  // (the render and game threads both ask)
    pthread_mutex_lock(&lock);
    // (on the start screen: the seed PLAY would start, from its folder - rando_seed_dir() would apply its patches
    // before the game is loaded, as runtime/cosmetics_seed.c says)
    extern int g_start_screen;  // warp.c
    char sdir[256];
    const char *dir = NULL;
    if (!g_start_screen) dir = rando_seed_dir();
    else if (g_settings.rando_seed[0]) dir = (snprintf(sdir, sizeof sdir, "rando/%s", g_settings.rando_seed), sdir);
    if (strcmp(dir ? dir : "", g_dir)) {  // (the seed's settings.json, read again when the seed changes)
        snprintf(g_dir, sizeof g_dir, "%s", dir ? dir : "");
        g_on = 0;
        char p[600];
        static char buf[1 << 16];
        FILE *f = dir ? (snprintf(p, sizeof p, "%s/settings.json", dir), fopen(p, "rb")) : NULL;
        if (f) {
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
            g_on = strstr(buf, "\"weather-cycle\": true") != NULL;
        }
        if (g_on) LOG("[weather] the seed's weather cycle is on\n");
    }
    bool on = g_on > 0;
    pthread_mutex_unlock(&lock);
    return on;
}

RT_ON_RESET(weather_cycle_reset) {
    pthread_mutex_lock(&g_lock);
    g_have = false, g_live_crc = 0, g_was_in_play = 0, g_nslots = 0, g_on = -1, g_dir[0] = 1, g_dir[1] = 0;
    pthread_mutex_unlock(&g_lock);
}

// The renderer: a scene composite was drawn (the clock runs only while the game draws its scene)
void weather_cycle_scene(void) { g_scene_at = host_now(); }

static uint32_t live_crc(void) { return *(const uint32_t *)(uintptr_t)(GA(LIVE_SAVE) + BODY_CRC); }

// a save came into play: its slot's weather, or a fresh Sunny
static void adopt(uint32_t crc) {
    g_live_crc = crc;
    for (int i = 0; i < g_nslots; i++)
        if (g_slots[i].c.crc == crc) {
            g_c = g_slots[i].c, g_c.from = -1, g_c.fade = FADE, g_have = true;
            LOG("[weather] cycle: %s from %s (%.0f s left, storms this spell %d)\n", name(g_c.kind), g_slots[i].path,
                g_c.left, g_c.storms);
            return;
        }
    fresh();
    LOG("[weather] cycle: a save without one - %s (%.0f s)\n", name(g_c.kind), g_c.left);
}

// Game thread, every frame
void weather_cycle_tick(void) {
    if (!weather_cycle_on()) return;
    static double last;
    double now = host_now(), dt = now - last;
    last = now;
    static float speed = -1;
    if (speed < 0) speed = getenv("LARECOMP_WEATHER_CYCLE_SPEED") ? (float)atof(getenv("LARECOMP_WEATHER_CYCLE_SPEED")) : 1;
    pthread_mutex_lock(&g_lock);
    int play = game_in_play();
    uint32_t crc = play ? live_crc() : 0;
    if (play && (!g_have || crc != g_live_crc)) {  // another save in play: a file loaded, a new game, a Load from the
        bool slot = false;                          // pause menu (mid-play, only a slot's: else the game is saving)
        for (int i = 0; i < g_nslots; i++) slot |= g_slots[i].c.crc == crc;
        if (!g_have || !g_was_in_play || slot) adopt(crc);
        else g_live_crc = crc;
    }
    g_was_in_play = play;
    bool run = play && g_have && now - g_scene_at < 0.5;
    static int log = -1, was = -1;  // LARECOMP_LOG_WEATHER=1 (test): the clock stopping and going on
    if (log < 0) log = getenv("LARECOMP_LOG_WEATHER") != NULL;
    if (log && run != was) LOG("[weather] cycle clock %s (%s %.1f s left)\n", run ? "runs" : "stopped", name(g_c.kind), g_c.left);
    was = run;
    if (run && dt > 0 && dt < 0.25) {
        float s = (float)dt * speed;
        if (g_c.fade < FADE) g_c.fade = g_c.fade + s < FADE ? g_c.fade + s : FADE;
        if ((g_c.left -= s) <= 0) next();
    }
    pthread_mutex_unlock(&g_lock);
}

// The weather showing now and how strongly (0..1): half way through a change the new one takes over
int weather_cycle_kind(float *level) {
    pthread_mutex_lock(&g_lock);
    int k = g_have ? g_c.kind : WEATHER_OFF;
    float lv = 1;
    if (g_have && g_c.from >= 0 && g_c.fade < FADE) {
        float dip = g_c.from == WEATHER_OFF || g_c.kind == WEATHER_OFF ? 0 : 0.3f;
        float h = g_c.fade / (FADE / 2);  // 0..2
        if (h < 1) k = g_c.from, lv = 1 - (1 - dip) * h;
        else lv = dip + (1 - dip) * (h - 1);
        lv = lv * lv * (3 - 2 * lv);
    }
    pthread_mutex_unlock(&g_lock);
    if (level) *level = lv;
    return k;
}

// ---- the save slots (runtime/services/fs.c)
static bool slot_path(const char *path, char *out, size_t n) {  // (the save's own path; a working copy's ".pending" off)
    size_t len = strlen(path);
    bool pending = len > 8 && !strcmp(path + len - 8, ".pending");
    snprintf(out, n, "%.*s.weather", (int)(len - (pending ? 8 : 0)), path);
    return pending;
}

// a save file was read into guest memory: its weather, kept for when that save comes into play
void weather_cycle_save_read(const char *path, uint64_t addr, int64_t off, int64_t size) {
    if (off != 0 || size < BODY_CRC + 4 || !weather_cycle_on()) return;
    char p[1100];
    slot_path(path, p, sizeof p);
    Cycle c;
    FILE *f = fopen(p, "rb");
    bool ok = f && fread(&c, sizeof c, 1, f) == 1 && c.magic == MAGIC &&
              c.crc == *(const uint32_t *)(uintptr_t)(addr + BODY_CRC);  // (else: from an older save of that slot)
    if (f) fclose(f);
    pthread_mutex_lock(&g_lock);
    int i = 0;
    while (i < g_nslots && strcmp(g_slots[i].path, p)) i++;
    if (i == g_nslots && g_nslots < 8) snprintf(g_slots[g_nslots++].path, sizeof g_slots[0].path, "%s", p);
    if (i < g_nslots) {
        if (ok) g_slots[i].c = c;
        else memset(&g_slots[i].c, 0, sizeof g_slots[i].c);  // (crc 0 matches no save)
    }
    pthread_mutex_unlock(&g_lock);
}

// a save file was written from guest memory: the weather now goes with it (the file is replaced whole, so a save
// that isn't committed leaves one whose CRC matches no save - read as none)
void weather_cycle_save_written(const char *path, uint64_t addr, int64_t off, int64_t size) {
    if (off != 0 || size < BODY_CRC + 4 || !weather_cycle_on()) return;
    pthread_mutex_lock(&g_lock);
    if (!g_have) { pthread_mutex_unlock(&g_lock); return; }
    Cycle c = g_c;
    c.crc = *(const uint32_t *)(uintptr_t)(addr + BODY_CRC);
    g_live_crc = live_crc();  // (the game writes from a copy: the live save keeps its own CRC - not a new save)
    char p[1100], tmp[1200];
    slot_path(path, p, sizeof p);
    int i = 0;
    while (i < g_nslots && strcmp(g_slots[i].path, p)) i++;
    if (i == g_nslots && g_nslots < 8) snprintf(g_slots[g_nslots++].path, sizeof g_slots[0].path, "%s", p);
    if (i < g_nslots) g_slots[i].c = c;
    pthread_mutex_unlock(&g_lock);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(&c, sizeof c, 1, f) == 1;
    ok = !fclose(f) && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING);  // (rename() won't replace a file there)
#else
    ok = ok && !rename(tmp, p);
#endif
    if (!ok) { unlink(tmp); LOG("[weather] cycle: could not write %s\n", p); }
}
