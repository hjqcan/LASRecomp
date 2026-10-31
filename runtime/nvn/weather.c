// Weather: the backend-neutral part (see weather.h).
#include "weather.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../game_addresses.h"
#include "../hle.h"
#include "../menu_sfx.h"
#include "../cosmetic_colour.h"
#include "../settings.h"

extern int game_in_play(void);                 // runtime/hle/warp.c
extern int cosmetics_seed_weather(void);       // runtime/cosmetics_seed.c
extern bool cosmetics_seed_on(void);
extern bool widescreen_frame_boxed(uint64_t frame);  // runtime/hle/widescreen.c (indoors: the interior backdrop)
extern void input_rumble_pulse(uint16_t lo, uint16_t hi, uint32_t ms);  // runtime/hle/input.c

static float g_cam[80];  // the frame's camera block (weather_scene_camera)
static uint64_t g_cam_frame = ~0ull;
static bool g_cam_view;  // (g_cam's view and projection are there - not only its view-projection: weather_scene_vp)

void weather_scene_camera(const float *u, uint64_t size, uint64_t frame) {
    if (size != 320 || g_cam_frame == frame) return;  // (the frame's first scene draw)
    if (u[12] != 0 || u[13] != 0 || u[14] != 0 || u[15] != 1 || u[30] != -1 || u[31] != 0) return;  // perspective
    memcpy(g_cam, u, sizeof g_cam);
    g_cam_frame = frame, g_cam_view = true;
}

void weather_scene_vp(const float *vp, uint64_t frame) {
    if (g_cam_frame == frame || vp[15] == 1) return;  // (perspective: its w row isn't 0 0 0 1)
    memcpy(g_cam + 32, vp, 16 * sizeof(float));
    g_cam_frame = frame, g_cam_view = false;
}

// LARECOMP_WEATHER=off|rain|snow|storm|blizzard|random (test): instead of the setting, and of a seed's own (-1: none)
static int env_weather(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("LARECOMP_WEATHER");
        static const char *const k[] = {"off", "rain", "snow", "storm", "blizzard", "random"};
        env = -1;
        for (int i = 0; e && i < 6; i++) if (!strcmp(e, k[i])) env = i;
    }
    return env;
}
static int setting(void) { return env_weather() >= 0 ? env_weather() : g_settings.weather; }

static uint32_t mix32(uint32_t h) {
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h;
}

static int g_random = WEATHER_OFF;  // Random: rolled each time the overworld is entered
static bool snowy(int kind) { return kind == WEATHER_SNOW || kind == WEATHER_BLIZZARD; }
static bool g_outdoors;

int weather_current(void) {
    if (weather_cycle_on()) return weather_cycle_kind(NULL);
    static int seed_on, seed_w;
    static uint64_t at;  // (the seed's pick: looked up once a second, it only changes with the seed)
    uint64_t now = (uint64_t)time(NULL);
    if (now != at) at = now, seed_on = cosmetics_seed_on(), seed_w = cosmetics_seed_weather();
    if (seed_on && env_weather() < 0) return seed_w;  // (a seed with its own cosmetics: its weather, over the setting -
                                                      // the row is locked)
    int s = setting();
    return s == WEATHER_RANDOM ? g_random : s;
}

static bool level_is_field(void) {
    if (!game_in_play()) return false;
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));  // PlayerStatus*: the current level's name at +0
    if (!ps) return false;
    return !strcmp((const char *)(uintptr_t)ps, "Field");
}

// rows: out = a * b
static void mat_mul(const float *a, const float *b, float *out) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

static bool mat_inverse(const float *m, float *inv) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-12) return false;
        if (p != c) for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c && a[r][c] != 0) {
                double f = a[r][c];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) inv[r * 4 + c] = (float)a[r][c + 4];
    return true;
}

// The world point at clip-space (x, y) on the plane y = py (false: the ray misses it)
static bool unproject_floor(const float *inv, float x, float y, float py, float out[3]) {
    float n[4], f[4];
    for (int r = 0; r < 4; r++) {
        n[r] = inv[r * 4 + 0] * x + inv[r * 4 + 1] * y + inv[r * 4 + 2] * -1 + inv[r * 4 + 3];
        f[r] = inv[r * 4 + 0] * x + inv[r * 4 + 1] * y + inv[r * 4 + 2] * 1 + inv[r * 4 + 3];
    }
    if (fabsf(n[3]) < 1e-9f || fabsf(f[3]) < 1e-9f) return false;
    for (int i = 0; i < 3; i++) n[i] /= n[3], f[i] /= f[3];
    float dy = f[1] - n[1];
    if (fabsf(dy) < 1e-6f) return false;
    float t = (py - n[1]) / dy;
    if (t < 0) return false;
    for (int i = 0; i < 3; i++) out[i] = n[i] + (f[i] - n[i]) * t;
    return true;
}

// ---- the storm: lightning flashes and thunder
static double g_next_flash = 4, g_flash_at = -100, g_thunder_at = -1;
static float g_thunder_gain, g_flash_k = 1;  // (k: the flash's strength - a strike here, 1.6)
static uint32_t g_rng = 0x1234567;
static float frand(void) { g_rng = mix32(g_rng + 0x9E3779B9u); return (g_rng >> 8) / 16777216.0f; }

// A thunderclap: the stone rumble (menu_sfx.c), each one its own - a little higher or lower, darker the farther, the
// roll from its start or from further in, cut shorter or left long, now and then a second roll on top; one close by
// (near: a strike here) a crack first, the roll right on it, bright
static void thunder(float gain, bool near) {
    float rate = 0.82f + frand() * 0.36f;  // (about 3 semitones either way)
    if (near) {
        menu_sfx_play_ex(MENU_SFX_THUNDER_CRACK, gain, 0.85f + frand() * 0.25f, 2200 + frand() * 2000, 0, 0, 0);
        menu_sfx_play_ex(MENU_SFX_THUNDER, gain, rate, 0, 0, 0, 0.05f);
        return;
    }
    float dark = 220 + frand() * 380, from = frand() * 0.3f, len = frand() < 0.4f ? 1.6f + frand() * 1.4f : 0;
    menu_sfx_play_ex(MENU_SFX_THUNDER, gain, rate, dark, from, len, 0);
    if (frand() < 0.35f)  // (a second roll, rolling on)
        menu_sfx_play_ex(MENU_SFX_THUNDER, gain * (0.4f + frand() * 0.3f), 0.8f + frand() * 0.3f, dark * 0.8f,
                         frand() * 0.4f, 0, 0.5f + frand() * 0.9f);
}

static float storm_flash(double t, uint64_t frame) {
    static long force_at = -2;  // LARECOMP_WEATHER_FLASH_AT=<frame> (test): a flash then
    if (force_at == -2) force_at = getenv("LARECOMP_WEATHER_FLASH_AT") ? atol(getenv("LARECOMP_WEATHER_FLASH_AT")) : -1;
    if (force_at >= 0 && (long)frame >= force_at) force_at = -1, g_next_flash = t;
    if (t >= g_next_flash) {
        g_flash_at = t, g_flash_k = 1;
        g_next_flash = t + 7 + frand() * 14;  // (every 7-21 s)
        float dist = frand();                  // near (0) .. far (1): the thunder's delay and volume
        g_thunder_at = t + 0.3 + dist * 2.2;
        g_thunder_gain = 0.9f - dist * 0.5f;
    }
    if (g_thunder_at >= 0 && t >= g_thunder_at) {
        g_thunder_at = -1;
        thunder(g_thunder_gain, false);
        input_rumble_pulse((uint16_t)(g_thunder_gain * 22000), 0, 350);  // (a short low rumble; off with Rumble off)
    }
    // the flash: two quick strobes, then a fade (~0.6 s)
    double a = t - g_flash_at;
    if (a < 0 || a > 0.7) return 0;
    float f = a < 0.06 ? 1.0f : a < 0.12 ? 0.2f : a < 0.18 ? 0.75f : (float)exp(-(a - 0.18) * 12);
    return f * g_flash_k;
}

// The probe points (uv in the depth texture: its row 0 at the top) and the world point a probe's depth gives
const float g_weather_probe_uv[WEATHER_PROBES][2] = {{0.5f, 0.5f}, {0.3f, 0.35f}, {0.7f, 0.35f}, {0.3f, 0.65f}, {0.7f, 0.65f}};
bool weather_probe_world(const WeatherFrame *f, int i, float d, float out[3]) {
    if (!(d > 0) || d >= 0.99999f || !(f->zr[1] != 0)) return false;  // (sky / nothing / no probe yet)
    float x = g_weather_probe_uv[i][0] * 2 - 1, y = 1 - g_weather_probe_uv[i][1] * 2;
    if (f->flags[0] > 0.5f) y = -y;
    float z = (d - f->zr[0]) / f->zr[1];
    if (f->flags[1] > 0.5f) z = z * 2 - 1;
    float q[4] = {x, y, z, 1}, r[4];
    for (int k = 0; k < 4; k++) r[k] = f->inv_vp[k * 4] * q[0] + f->inv_vp[k * 4 + 1] * q[1] + f->inv_vp[k * 4 + 2] * q[2] + f->inv_vp[k * 4 + 3];
    if (fabsf(r[3]) < 1e-9f) return false;
    for (int k = 0; k < 3; k++) out[k] = r[k] / r[3];
    return true;
}

// ---- snow on the ground (weather.h): the cover, Link's footprints and the snow he kicks up
static double t = 0;   // the weather's time (s): counted only while it shows (weather_frame)
static float ground;   // the ground's height (weather_frame: the scene depth at its probe points)
static float g_cover;  // how much snow lies (0..1)
static float g_build = -1;  // (s from none to all: LARECOMP_SNOW_BUILD, 90)
static float env_f(const char *name, float def) { const char *e = getenv(name); return e ? (float)atof(e) : def; }

static pthread_mutex_t g_link_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_link[3];       // Link's position as the game thread read it last (PlayerCollision +0x80: his feet)
static double g_link_at;      // (host time then)
static uint64_t g_link_n;     // (samples so far)
static float g_wind[2];       // a blizzard gust's velocity now - not its steady wind (world x, z per s: weather_wind, render thread)
static double g_wind_at;      // (host time then)

static uint64_t cpu_ns(void) {  // (LARECOMP_SNOW_TIME's timings: a monotonic clock, every platform)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static double host_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static bool level_is_field(void);
void weather_player_tick(Cpu *c, uint64_t frame) {
    static int tm = -1;  // LARECOMP_SNOW_TIME=1 (test): this tick's CPU time (game thread), mean every 120 frames
    if (tm < 0) tm = getenv("LARECOMP_SNOW_TIME") != NULL;
    uint64_t t0 = tm ? cpu_ns() : 0;
    void weather_player_tick_(Cpu *c, uint64_t frame);
    weather_player_tick_(c, frame);
    if (tm) {
        static uint64_t sum;
        static int n;
        sum += cpu_ns() - t0;
        if (++n == 120) LOG("[snow] game thread CPU %.2f us a frame\n", sum / 120 / 1e3), sum = 0, n = 0;
    }
}
static void strike_tick(Cpu *c);
void weather_player_tick_(Cpu *c, uint64_t frame) {
    weather_cycle_tick();
    int kind = weather_current();
    if (!g_outdoors || !(snowy(kind) || kind == WEATHER_STORM) || !level_is_field()) return;
    if (kind == WEATHER_STORM) strike_tick(c);
    extern uint64_t doorjump_player(uint64_t frame);  // runtime/hle/doorjump.c (the PlayerCollision component)
    uint64_t pc = doorjump_player(frame);
    if (!pc) return;
    const float *p = (const float *)(uintptr_t)(pc + 0x80);
    pthread_mutex_lock(&g_link_lock);
    memcpy(g_link, p, sizeof g_link);
    g_link_at = host_now();
    g_link_n++;
    pthread_mutex_unlock(&g_link_lock);
}

static SnowPrint g_prints[64];  // made, not stamped yet (weather_snow_prints)
static int g_nprints;
static SnowPuff g_puffs[SNOW_PUFFS];  // a ring
static int g_puff_next;
static float frand(void);

// ---- trees shaken bare: Link bonks a tree (dashing into it): its snow falls off in a burst and grows back
typedef struct { float x, y, z; double at; } Shaken;
static Shaken g_shaken[8];  // slot 1..7 (0 unused)
static float g_trees[64][3];  // trees drawn this frame (weather_snow_tree), for the next bonk to pick from
static int g_ntrees, g_trees_next;

int weather_snow_tree(const float pos[3]) {
    if (g_trees_next < 64) memcpy(g_trees[g_trees_next++], pos, sizeof g_trees[0]);
    for (int i = 1; i < 7; i++)
        if (g_shaken[i].at > 0 && fabsf(g_shaken[i].x - pos[0]) < 0.3f && fabsf(g_shaken[i].z - pos[2]) < 0.3f) return i;
    return 0;
}

static void shake_tree(float x, float z, float within) {  // (the tree nearest x, z: its snow off, a burst falling)
    int best = -1;
    float bd = within * within;
    for (int i = 0; i < g_ntrees; i++) {
        float dx = g_trees[i][0] - x, dz = g_trees[i][2] - z;
        if (dx * dx + dz * dz < bd) bd = dx * dx + dz * dz, best = i;
    }
    if (best < 0) {
        if (getenv("LARECOMP_SNOW_BONK")) LOG("[snow] bonk at %.2f %.2f: no tree near (%d drawn)\n", x, z, g_ntrees);
        return;
    }
    const float *tp = g_trees[best];
    int s = 1;
    for (int i = 1; i < 7; i++) {  // (the same tree again, else a free slot, else the oldest)
        if (fabsf(g_shaken[i].x - tp[0]) < 0.3f && fabsf(g_shaken[i].z - tp[2]) < 0.3f) { s = i; break; }
        if (g_shaken[i].at < g_shaken[s].at) s = i;
    }
    g_shaken[s].x = tp[0], g_shaken[s].y = tp[1], g_shaken[s].z = tp[2], g_shaken[s].at = t;
    float share = g_cover;  // (a burst as big as the snow it held)
    for (int k = 0; k < (int)(70 * share); k++) {  // clumps falling from the canopy to the ground around it
        SnowPuff *q = &g_puffs[g_puff_next++ % SNOW_PUFFS];
        float a = frand() * 6.2832f, r = 0.3f + frand() * 1.3f;
        q->p[0] = tp[0] + cosf(a) * r, q->p[1] = tp[1] + 1.6f + frand() * 1.4f, q->p[2] = tp[2] + sinf(a) * r * 0.8f;
        q->p[3] = (float)t + frand() * 0.25f;
        q->v[0] = cosf(a) * (0.3f + frand() * 0.8f), q->v[1] = frand() * 0.8f, q->v[2] = sinf(a) * (0.3f + frand() * 0.8f);
        q->v[3] = 7 + frand() * 8;
        q->g[0] = tp[1] + 0.05f, q->g[1] = 1.2f + frand() * 0.5f, q->g[2] = 4.5f;
    }
    LOG("[snow] tree at %.2f %.2f %.2f shaken bare (slot %d)\n", tp[0], tp[1], tp[2], s);
}

static void puff(const float at[3], float dx, float dz, bool dash) {
    SnowPuff *q = &g_puffs[g_puff_next++ % SNOW_PUFFS];
    q->g[0] = at[1], q->g[1] = 0.8f, q->g[2] = 2;
    float a = frand() * 6.2832f, r = frand() * (dash ? 0.25f : 0.15f);
    q->p[0] = at[0] + cosf(a) * r, q->p[1] = at[1] + 0.05f, q->p[2] = at[2] + sinf(a) * r, q->p[3] = (float)t;
    float back = dash ? 1.2f + frand() * 1.6f : 0.3f + frand() * 0.6f, side = (frand() - 0.5f) * (dash ? 2.0f : 1.0f);
    q->v[0] = -dx * back - dz * side, q->v[2] = -dz * back + dx * side;  // (behind him, spread sideways)
    q->v[1] = dash ? 1.4f + frand() * 1.6f : 0.7f + frand() * 0.8f;
    q->v[3] = dash ? 9 + frand() * 7 : 6 + frand() * 4;
}

// Footprints and kicked-up snow from Link's latest positions (render thread, once a frame while it shows)
static void snow_link(void) {
    static float lp[3], stride, spd;
    static double lat, acc;
    static uint64_t seen;
    static int side = 1;
    float p[3];
    double at;
    uint64_t n;
    pthread_mutex_lock(&g_link_lock);
    memcpy(p, g_link, sizeof p), at = g_link_at, n = g_link_n;
    pthread_mutex_unlock(&g_link_lock);
    if (n == seen) return;
    double dt = at - lat;
    float dx = p[0] - lp[0], dy = p[1] - lp[1], dz = p[2] - lp[2], d = sqrtf(dx * dx + dz * dz);
    bool fresh = n != seen + 1 && (dt <= 0 || dt > 0.25);
    seen = n;
    memcpy(lp, p, sizeof lp), lat = at;
    if (fresh || dt <= 0 || dt > 0.25 || d > 2) { stride = 0; return; }  // (first sample, a pause, a warp)
    float v = d / (float)dt;
    {   // a bonk: dashing (the boots), then stopped dead - the tree just ahead is shaken. LARECOMP_SNOW_BONK=<s>
        // (test): one then, at the tree nearest Link
        static float last_ux, last_uz;
        static double test_at = -2;
        if (test_at == -2) test_at = getenv("LARECOMP_SNOW_BONK") ? atof(getenv("LARECOMP_SNOW_BONK")) : -1;
        static float dash_v0 = -1;
        if (dash_v0 < 0) dash_v0 = env_f("LARECOMP_SNOW_DASH", 6.5f);
        if (spd > dash_v0 && v < 1.5f && d < 0.05f) shake_tree(p[0] + last_ux * 1.0f, p[2] + last_uz * 1.0f, 2.5f);
        if (test_at >= 0 && t >= test_at) test_at = -1, shake_tree(p[0], p[2], 12);
        if (d > 0.01f) last_ux = dx / d, last_uz = dz / d;
    }
    spd += (fminf(v, 14) - spd) * 0.1f;  // (the samples' host times jitter: walking reads 4.5 +- 3)
    bool ground = fabsf(dy) / (float)dt < 0.8f;  // (not jumping / falling)
    static float dash_v = -1;
    if (dash_v < 0) dash_v = env_f("LARECOMP_SNOW_DASH", 6.5f);  // (world units / s: walking is 4.5, faster the boots)
    bool dash = spd > dash_v;
    static int logged;  // LARECOMP_LOG_SNOW: Link's speed (calibration)
    if (getenv("LARECOMP_LOG_SNOW") && logged++ % 20 == 0)
        LOG("[snow] link %.2f %.2f %.2f speed %.2f ground %d dash %d cover %.2f\n", p[0], p[1], p[2], spd, ground, dash, g_cover);
    if (!ground || d < 1e-4f || g_cover < 0.05f) { stride = 0; return; }
    float ux = dx / d, uz = dz / d;
    // a print every half stride, left and right of his path, along his way
    const float step = 0.5f, foot = 0.13f;
    float s0 = stride;
    stride += d;
    for (float k = step - s0; stride >= step && g_nprints < 64; k += step) {
        stride -= step;
        side = -side;
        float f = k / d;  // (where along this frame's move)
        SnowPrint *q = &g_prints[g_nprints++];
        q->x = p[0] - dx * (1 - f) - uz * foot * side, q->z = p[2] - dz * (1 - f) + ux * foot * side;
        q->y = p[1], q->t = (float)t;
        q->ang = atan2f(ux, uz), q->side = (float)side;
        q->len = dash ? 0.42f : 0.36f, q->wid = 0.19f;
    }
    // kicked-up snow: a few puffs a second walking, a stream with the boots
    if (g_cover < 0.15f || v < 0.6f) return;
    acc += dt * (dash ? 80 : 16) * fminf(g_cover * 1.5f, 1.0f);
    while (acc >= 1) acc -= 1, puff(p, ux, uz, dash);
}

int weather_snow_prints(SnowPrint *out, int max) {
    int n = g_nprints < max ? g_nprints : max;
    memcpy(out, g_prints, n * sizeof *out);
    g_nprints = 0;
    return n;
}

int weather_snow_puffs(SnowPuff out[SNOW_PUFFS]) {
    memcpy(out, g_puffs, sizeof g_puffs);
    return SNOW_PUFFS;
}

float weather_snow_now(void) { return (float)t; }
bool weather_snow_on(void) { return g_outdoors && g_cover > 0.002f; }

bool weather_snow_cover(SnowFrame *s, const float vp[16]) {
    static int dbg = -1;  // LARECOMP_SNOW_DEBUG=1 (test): the cover's mask in flat red
    if (dbg < 0) dbg = (int)env_f("LARECOMP_SNOW_DEBUG", 0);
    if (!g_outdoors || g_cover <= 0.002f) return false;
    memcpy(s->vp, vp, sizeof s->vp);
    if (!mat_inverse(s->vp, s->inv_vp)) return false;
    s->flags[2] = g_cover, s->flags[3] = (float)t;
    s->zr[2] = 32, s->zr[3] = 1024;  // (the trail map: 1024 texels over 32 units, ~3 cm each, wrapping)
    s->trail[0] = env_f("LARECOMP_SNOW_REFILL", 30), s->trail[1] = (float)dbg;
    // (the scene's colours are linear and before the game's tone mapping: its sunlit white ground is ~1)
    static float tone[4] = {-1};
    if (tone[0] < 0) {
        tone[0] = 0.84f, tone[1] = 0.87f, tone[2] = 0.93f, tone[3] = 0.8f;
        const char *e = getenv("LARECOMP_SNOW_TONE");  // r,g,b,ref (test)
        if (e) sscanf(e, "%f,%f,%f,%f", &tone[0], &tone[1], &tone[2], &tone[3]);
    }
    memcpy(s->tone, tone, sizeof tone);
    s->shake[7] = 1;  // (slot 7: the face trees, never shaken - renderer.m's sc_tree)
    for (int i = 1; i < 7; i++) {  // (a shaken tree grows its snow back at the rate it fell, from none)
        if (g_shaken[i].at <= 0) continue;
        float grown = (float)(t - g_shaken[i].at) / (weather_current() == WEATHER_BLIZZARD ? g_build * 0.45f : g_build);
        s->shake[i] = g_cover > 0 ? fminf(1, grown / g_cover) : 1;
        if (s->shake[i] >= 1) g_shaken[i].at = 0;
    }
    return true;
}

// ---- a storm's lightning strikes (weather.h): every 20-40 s a bolt at a spot near Link - half the time his own,
// ~1 s ahead (a glow and crackling sparks there). Looks only (the Cosmetics tab's weather, a seed's own); with
// weather_lightning_harm on (for a randomizer weather option to come), Link still standing there when it strikes
// gets a Buzz Blob's shock (the game's own: strike_tick, game thread)
static struct {
    float x, y, z;
    double glow, hit;  // (weather time: the glow began, the bolt strikes; glow 0: none)
    bool struck;
    uint32_t seed;
    int n;
    float pts[STRIKE_PTS][4];
} g_strike;
static double g_next_strike = -1;
static int g_shock;    // (the render thread asks the game thread to shock Link: strike_tick)
static int g_link_ok;  // (the game thread: Link can be struck now - not in a cutscene, swimming, ...: strike_tick)
static bool g_harm = false;  // (weather_lightning_harm)

void weather_lightning_harm(bool on) { g_harm = on; }

static int bolt_line(float (*pts)[4], int n, int max, const float from[3], const float to[3], int steps, float wild,
                     float w0, float w1) {
    float off[2] = {0, 0};
    for (int k = 0; k <= steps && n < max; k++) {
        float f = (float)k / steps;
        // (a random walk across the line, pulled back to it toward the end: every bolt its own jags)
        off[0] = off[0] * 0.75f + (frand() - 0.5f) * wild, off[1] = off[1] * 0.75f + (frand() - 0.5f) * wild * 0.6f;
        float pull = k == steps ? 0 : 1 - f * f;
        pts[n][0] = from[0] + (to[0] - from[0]) * f + off[0] * pull;
        pts[n][1] = from[1] + (to[1] - from[1]) * f;
        pts[n][2] = from[2] + (to[2] - from[2]) * f + off[1] * pull;
        pts[n][3] = w0 + (w1 - w0) * f;
        n++;
    }
    return n;
}

static void strike_build(void) {
    // the main channel from high up (leaning away from the camera: up the screen) down to the target, then two
    // branches off it that end in the air; a 0-width point between them (a break)
    float top[3] = {(frand() - 0.5f) * 4, 14, -3 - frand() * 3}, end[3] = {0, 0, 0};
    int n = bolt_line(g_strike.pts, 0, STRIKE_PTS, top, end, 21, 1.4f, 5.5f, 4.5f);
    for (int b = 0; b < 2 && n < STRIKE_PTS - 2; b++) {
        g_strike.pts[n][3] = 0, n++;
        const float *at = g_strike.pts[4 + b * 6 + (int)(frand() * 3)];
        float side = frand() < 0.5f ? -1 : 1, len = 2 + frand() * 2.5f;
        float to[3] = {at[0] + side * len, at[1] - len * (0.8f + frand() * 0.6f), at[2] + (frand() - 0.5f) * len};
        n = bolt_line(g_strike.pts, n, STRIKE_PTS, at, to, 7, 0.9f, 2.5f, 0.6f);
    }
    g_strike.n = n;
}

static bool link_now(float p[3]) {  // (Link's feet as the game thread read them last, if recent)
    pthread_mutex_lock(&g_link_lock);
    memcpy(p, g_link, sizeof g_link);
    double at = g_link_at;
    pthread_mutex_unlock(&g_link_lock);
    return g_link_n && host_now() - at < 0.5;
}

// The render thread, every frame of a storm on screen (weather time t)
static void storm_strikes(void) {
    static double test = -2, every;  // LARECOMP_WEATHER_STRIKE=<s>[,<every s>][,link] (test): the first then, the next
    static bool test_link;           // every so often (default as normal), ",link": every one at Link
    if (test == -2) {
        const char *e = getenv("LARECOMP_WEATHER_STRIKE");
        test = -1, every = 0;
        if (e) test = atof(e), every = strchr(e, ',') ? atof(strchr(e, ',') + 1) : 0, test_link = strstr(e, "link") != NULL;
        if (test >= 0) g_next_strike = test;
    }
    if (g_next_strike < 0) g_next_strike = t + 12 + frand() * 10;
    if (g_strike.glow > 0 && t > g_strike.hit + 14) g_strike.glow = 0;  // (the scorch has faded)
    if (t >= g_next_strike && (g_strike.glow <= 0 || t > g_strike.hit + 1.5)) {
        float p[3];
        if (!link_now(p) || !__atomic_load_n(&g_link_ok, __ATOMIC_ACQUIRE)) g_next_strike = t + 2;  // (later)
        else {
            bool at_link = test_link || frand() < 0.5f;
            float a = frand() * 6.2832f, r = at_link ? frand() * 0.3f : 2.5f + frand() * 4.5f;
            g_strike.x = p[0] + cosf(a) * r, g_strike.y = p[1], g_strike.z = p[2] + sinf(a) * r * 0.8f;
            g_strike.glow = t, g_strike.hit = t + 1.0, g_strike.struck = false;
            g_strike.seed = (uint32_t)(frand() * 16777216.0f);
            strike_build();
            g_next_strike = t + (every > 0 ? every : 20 + frand() * 20);
            for (int k = 0; k < 3; k++)  // (the crackle on the ground: the Buzz Blob's spark, three times as it grows)
                menu_sfx_play_ex(MENU_SFX_SPARK, 0.7f + 0.3f * k, 0.85f + frand() * 0.3f, 0, 0, 0, k * 0.33f + frand() * 0.1f);
            LOG("[weather] lightning at %.2f %.2f %.2f (%s Link)\n", g_strike.x, g_strike.y, g_strike.z, at_link ? "at" : "near");
        }
    }
    if (g_strike.glow > 0 && !g_strike.struck && t >= g_strike.hit) {
        g_strike.struck = true;
        g_flash_at = t, g_flash_k = 1.6f;  // (a bright flash, the thunder right away and loud: it is here)
        g_thunder_at = -1;
        g_next_flash = fmax(g_next_flash, t + 4);
        thunder(1.0f, true);
        input_rumble_pulse(30000, 12000, 450);
        float p[3];
        bool hit = link_now(p) && hypotf(p[0] - g_strike.x, p[2] - g_strike.z) < 0.9f &&
                   __atomic_load_n(&g_link_ok, __ATOMIC_ACQUIRE);
        static int harm_env = -1;  // LARECOMP_WEATHER_HARM=1 (test): the shock on, as the randomizer's option will
        if (harm_env < 0) harm_env = getenv("LARECOMP_WEATHER_HARM") != NULL;
        bool harm = g_harm || harm_env || weather_cycle_on();  // (the seed's weather cycle: its storms hurt)
        if (hit && harm) __atomic_store_n(&g_shock, 1, __ATOMIC_RELEASE);
        if (hit) LOG("[weather] lightning hit Link%s\n", harm ? "" : " (looks only: no shock)");
    }
}

bool weather_strike(StrikeFrame *s) {
    if (g_strike.glow <= 0 || weather_current() != WEATHER_STORM) return false;
    s->at[0] = g_strike.x, s->at[1] = g_strike.y, s->at[2] = g_strike.z, s->at[3] = (float)t;
    s->tm[0] = (float)g_strike.glow, s->tm[1] = (float)g_strike.hit, s->tm[2] = (float)g_strike.seed, s->tm[3] = (float)g_strike.n;
    memcpy(s->pts, g_strike.pts, sizeof s->pts);
    return true;
}

// The game thread (weather_player_tick, a storm on the overworld): whether Link can be struck now, and the shock
// Link (GA(GAME_MGR) +0x4b8: his actor) and whether the game would let a hit land now: its own check (the filter his
// damage component runs on every hit: his state, his blink after a hit at +0x1e80, +0x1b8c bit 0 no hits), then
// the Buzz Blob's own hit (its attack data: 1 heart, reaction 1 - the electric shock, "dmg_elec") through his damage
// callback, as the collision system calls it: the damage (the Gameplay tab's multiplier too), the sound, the blink;
// his next update plays the shock. No attacker: no knockback. The game thread only (weather_player_tick)
static uint64_t g_shock_mem;  // (guest: the callbacks' lambda {0, Link}, an empty attacker, the hit info)
static uint64_t link_actor(void) {
    extern int game_in_play(void);
    uint64_t gm = game_in_play() ? LD64(GA(GAME_MGR)) : 0, link = gm ? LD64(gm + 0x4b8) : 0;
    return link && LD64(link + 8) == link ? link : 0;  // (its own weak pointer: a live actor)
}

static uint32_t link_hit(Cpu *c, uint64_t link, bool damage) {
    if (!g_shock_mem) g_shock_mem = (uint64_t)(uintptr_t)hle_alloc(0x500);
    uint64_t lam = g_shock_mem, pair = lam + 0x10, hit = lam + 0x30, cmp = lam + 0x100;
    memset((void *)(uintptr_t)lam, 0, 0x500);
    ST64(lam + 8, link);
    ST64(hit + 0x10, link), ST64(hit + 0x18, LD64(link + 0x10));
    ST64(hit + 0x20, GA(BUZZBLOB_ATTACK)), ST64(hit + 0x28, GA(LINK_DEFENCE));
    uint64_t saved[31], sp = c->sp, pc = c->pc;
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = lam, c->x[1] = damage ? pair : cmp, c->x[2] = hit;
    guest_call(c, damage ? GA(LINK_DAMAGE_CB) : GA(LINK_HIT_FILTER));
    uint32_t r = (uint32_t)c->x[0];
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp, c->pc = pc;
    return r;
}

// His state (+0x4ed: an index into the state objects at +0x1340): only these let lightning aim at him or hit him -
// 0 standing / walking, 1 jumping, 2 a sword swing, 10 the shield up, 26 the hookshot (seen in test runs); any other
// (swimming, falling, carrying, talking, an item get, a cutscene, a level's start) is left alone.
// LARECOMP_LOG_LINK_STATE=1 (test): each new state, its vtable and what the game's hit check says then
static uint32_t link_hit(Cpu *c, uint64_t link, bool damage);
static bool link_free(Cpu *c, uint64_t link) {
    uint8_t id = LD8(link + 0x4ed);
    static int lg = -1, last = -1;
    if (lg < 0) lg = getenv("LARECOMP_LOG_LINK_STATE") != NULL;
    if (lg && id != last) {
        last = id;
        uint64_t so = id < 0x40 ? LD64(link + 0x1340 + 8 * (uint64_t)id) : 0, vt = so ? LD64(so) : 0, ti = vt ? LD64(vt - 8) : 0;
        const char *name = ti ? (const char *)(uintptr_t)LD64(ti + 8) : NULL;
        LOG("[weather] link state %u (%s) vt main+%llx iframes %g nohit %u filter %#x\n", id, name ? name : "?",
            (unsigned long long)(vt ? vt - 0x7000000000ull : 0), *(const float *)(uintptr_t)(link + 0x1e80),
            LD8(link + 0x1b8c) & 1, link_hit(c, link, false));
    }
    return id < 32 && ((1u << id) & (1u << 0 | 1u << 1 | 1u << 2 | 1u << 10 | 1u << 26));
}

static bool link_shockable(Cpu *c) {
    uint64_t link = link_actor();
    if (!link || !link_free(c, link) || *(const float *)(uintptr_t)(link + 0x1e80) > 0 || (LD8(link + 0x1b8c) & 1)) return false;
    return true;
}

static void link_shock(Cpu *c) {
    uint64_t link = link_actor();
    if (!link) return;
    uint32_t f = link_hit(c, link, false);  // (the game's own check: 3 - no hit now)
    LOG("[weather] lightning shocks Link (filter %#x)\n", f);
    if ((f & 0xff) == 3) return;
    link_hit(c, link, true);
}

static void strike_tick(Cpu *c) {
    bool ok = link_shockable(c);
    __atomic_store_n(&g_link_ok, ok, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(&g_shock, 0, __ATOMIC_ACQ_REL) && ok) link_shock(c);
}

// ---- a blizzard's gusts pushing Link (for a randomizer option to come: off by default - the Cosmetics tab's weather is
// looks only). tools/patches.py WIND_PUSH_SITE: in his move, the frame's displacement summed, before the collision
// move - a share of the wind's velocity added there (walls and ledges stop it like his walking; the stick works
// against it). Only on the overworld in states 0/1/2/10/26 (link_free), not in a pit's pull or a knockback; weaker
// on slow ground (tall grass, shallow water) so a headwind never walks him backwards there
static bool g_push = false;
void weather_wind_push(bool on) { g_push = on; }

void rt_wind_push(Cpu *c) {
    static int env = -1;  // LARECOMP_WEATHER_PUSH=1 (test): on, as the randomizer's option will
    if (env < 0) env = getenv("LARECOMP_WEATHER_PUSH") != NULL;
    if (!(g_push || env || weather_cycle_on()) || weather_current() != WEATHER_BLIZZARD || !g_outdoors) return;
    uint64_t link = c->x[19];
    uint8_t id = LD8(link + 0x4ed);
    if (id >= 32 || !((1u << id) & (1u << 0 | 1u << 1 | 1u << 2 | 1u << 10 | 1u << 26))) return;
    uint16_t ground = LD16(link + 0x1700);
    uint64_t kb = LD64(link + 0x1578);
    if ((ground & 0x3c0) == 0x180 || (kb && LD8(kb + 0x1a) == 2) || !level_is_field()) return;
    float w[2];
    pthread_mutex_lock(&g_link_lock);
    memcpy(w, g_wind, sizeof w);
    double at = g_wind_at;
    pthread_mutex_unlock(&g_link_lock);
    if (host_now() - at > 0.5) return;  // (the weather not drawn lately: a menu, a door)
    float dt;
    memcpy(&dt, &c->v[1], 4);
    if (!(dt > 0 && dt < 0.1f)) dt = 1 / 60.0f;
    static float k = -1;  // LARECOMP_WEATHER_PUSH_K (test): the share of the wind (0.14: a gust 1-1.5 / s, Link walks 4.5)
    if (k < 0) k = env_f("LARECOMP_WEATHER_PUSH_K", 0.14f);
    float s = k * ((unsigned)((ground & 0x3f) - 0x15) <= 1 ? 0.3f : 1);  // (slow ground: walking x0.25 / x0.75)
    float *d = (float *)(uintptr_t)(c->x[29] - 0x80);
    d[0] += w[0] * s * dt, d[2] += w[1] * s * dt;
}

// The wind: rain and storm blow steadily along x, snow drifts gently; a blizzard has a wind of its own and gusts -
// 0.5-2.5 s after one dies down the next blows hard (8-12 world units / s) from a new direction for 6-11 s, ramped
// up and back down smoothly. wind[0..1]: the drift so far (wrapped at the volume's size: the particles' positions wrap with it),
// wind[2..3]: the velocity now. LARECOMP_BLIZZARD_GUST=<s> (test): the first gust then, from LARECOMP_BLIZZARD_DIR
// (degrees, 0 = screen right, 90 = screen up; every gust)
static void weather_wind(WeatherFrame *f, int kind, double dt, float size, float level) {
    static double dx, dz, gust_at = -1, gust_len;
    static float gust_ang, gust_str, base_ang;
    float vx, vz, gx = 0, gz = 0;  // (gx, gz: a blizzard gust's share - what pushes Link)
    if (kind == WEATHER_BLIZZARD) {
        if (gust_at < 0) {
            gust_at = getenv("LARECOMP_BLIZZARD_GUST") ? atof(getenv("LARECOMP_BLIZZARD_GUST")) : t + 3;
            gust_ang = getenv("LARECOMP_BLIZZARD_DIR") ? (float)atof(getenv("LARECOMP_BLIZZARD_DIR")) * 0.0174533f : frand() * 6.2832f;
            gust_len = 7, gust_str = 10, base_ang = frand() * 6.2832f;
        }
        double a = t - gust_at;
        if (a > gust_len + 3.5) {  // (the next gust, from somewhere else)
            gust_at = t + 0.5 + frand() * 2, gust_len = 6 + frand() * 5, gust_str = 8 + frand() * 4;
            gust_ang += 1.2f + frand() * 3.9f;  // (at least ~70 degrees off the last)
            if (getenv("LARECOMP_BLIZZARD_DIR")) gust_ang = (float)atof(getenv("LARECOMP_BLIZZARD_DIR")) * 0.0174533f;
            a = t - gust_at;
        }
        float env = a < 0 ? 0 : a < 1.5 ? (float)(a / 1.5) : a < 1.5 + gust_len ? 1 : (float)fmax(0, 1 - (a - 1.5 - gust_len) / 2);
        env = env * env * (3 - 2 * env);  // (smooth ramps)
        base_ang += (float)dt * 0.05f;
        // screen right = world +x, screen up = world -z (the overworld's camera looks north)
        env *= level;  // (a blizzard fading in or out: weaker gusts)
        gx = env * gust_str * cosf(gust_ang), gz = -env * gust_str * sinf(gust_ang);
        vx = 2 * cosf(base_ang) + gx, vz = -2 * sinf(base_ang) + gz;
    } else {
        vx = kind == WEATHER_SNOW ? 0.6f : kind == WEATHER_STORM ? 8 : 3, vz = 0;
        gust_at = -1;
    }
    dx = fmod(dx + vx * dt, size), dz = fmod(dz + vz * dt, size);
    f->wind[0] = (float)dx, f->wind[1] = (float)dz, f->wind[2] = vx, f->wind[3] = vz;
    pthread_mutex_lock(&g_link_lock);  // (for the game thread: a blizzard's push on Link, wind_tick)
    g_wind[0] = gx, g_wind[1] = gz, g_wind_at = host_now();
    pthread_mutex_unlock(&g_link_lock);
}

// LARECOMP_LOG_WEATHER=1 (test): why the weather isn't drawn, each time that changes
static bool wx_off(int why, uint64_t frame) {
    static int env = -1, last = -1;
    if (env < 0) env = getenv("LARECOMP_LOG_WEATHER") != NULL;
    if (env && why != last) LOG("[weather] frame %llu: %s\n", (unsigned long long)frame,
                               (const char *const[]){"drawn", "not field", "boxed", "off", "no camera", "camera", "volume", "camera height"}[why]);
    last = why;
    return why == 0;
}

bool weather_frame(WeatherFrame *f, uint64_t frame, const float probe[WEATHER_PROBES]) {
    g_ntrees = g_trees_next, g_trees_next = 0;  // (the trees the scene drew this frame: weather_snow_tree)
    // host time, counted only while the overworld is on screen (a menu or a door stops it)
    static double last = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec * 1e-9, dt = now - last;
    last = now;
    bool field = level_is_field() && !widescreen_frame_boxed(frame);
    if (field && !g_outdoors) {  // (back outside: Random rolls again - clear, rain, snow, storm or blizzard)
        g_rng ^= (uint32_t)(now * 1000);
        g_random = (int)(frand() * 5) % 5;
        if (setting() == WEATHER_RANDOM) LOG("[weather] random: %d\n", g_random);
    }
    g_outdoors = field;
    float level = 1;  // (the weather cycle: a change fading the old weather out and the new one in)
    int kind = weather_cycle_on() ? weather_cycle_kind(&level) : weather_current();
    if (field && dt > 0 && dt < 0.25) {  // the snow on the ground: builds while it snows, melts otherwise
        if (g_build < 0) g_build = env_f("LARECOMP_SNOW_BUILD", 90), g_cover = env_f("LARECOMP_SNOW_COVER", 0);  // (test)
        g_cover = snowy(kind) ? fminf(1, g_cover + (float)dt / (kind == WEATHER_BLIZZARD ? g_build * 0.45f : g_build))  // (a blizzard: faster)
                              : fmaxf(0, g_cover - (float)dt / 30);
    }
    if (!field || kind == WEATHER_OFF || g_cam_frame != frame)
        return wx_off(!level_is_field() ? 1 : !field ? 2 : kind == WEATHER_OFF ? 3 : 4, frame);
    if (dt > 0 && dt < 0.25) t += dt;
    if (snowy(kind)) {
        static int tm = -1;  // LARECOMP_SNOW_TIME=1 (test): the footprints' and kicked-up snow's CPU time (render thread)
        if (tm < 0) tm = getenv("LARECOMP_SNOW_TIME") != NULL;
        uint64_t t0 = tm ? cpu_ns() : 0;
        snow_link();
        if (tm) {
            static uint64_t sum;
            static int n;
            sum += cpu_ns() - t0;
            if (++n == 120) LOG("[snow] footprints CPU %.2f us a frame\n", sum / 120 / 1e3), sum = 0, n = 0;
        }
    }

    // (screen, flags, zr: filled by the renderer)
    memset(f->origin, 0, sizeof f->origin), memset(f->box, 0, sizeof f->box), memset(f->shape, 0, sizeof f->shape);
    memset(f->color, 0, sizeof f->color), memset(f->fill, 0, sizeof f->fill), memset(f->cam, 0, sizeof f->cam);
    memset(f->leaf, 0, sizeof f->leaf);
    f->count = f->splashes = 0;
    f->kind = kind;
    memcpy(f->vp, g_cam + 32, sizeof f->vp);
    if (!mat_inverse(f->vp, f->inv_vp)) return wx_off(5, frame);
    // the ground's height: the scene depth at a few screen points (probe: the last frame's), their median
    static uint64_t ground_frame;
    float ys[WEATHER_PROBES];
    int ny = 0;
    for (int i = 0; i < WEATHER_PROBES; i++) {
        float p[3];
        if (probe && weather_probe_world(f, i, probe[i], p)) ys[ny++] = p[1];
    }
    if (ny) {
        for (int i = 1; i < ny; i++)
            for (int j = i; j > 0 && ys[j] < ys[j - 1]; j--) { float x = ys[j]; ys[j] = ys[j - 1]; ys[j - 1] = x; }
        float g = ys[ny / 2];
        // (eased, so a bush or a roof under a probe doesn't jolt the rain; a jump - a new screen - is taken at once)
        ground = fabsf(g - ground) > 6 || frame > ground_frame + 4 ? g : ground + (g - ground) * 0.1f;
        ground_frame = frame;
    }
    // the volume: around the ground point at the screen's centre, of a fixed size (a drop's place in the world comes
    // from its hash, the time and the size only: the volume can follow the view and the ground without moving one)
    float c[3], a[3], b[3];
    if (!unproject_floor(f->inv_vp, 0, 0, ground, c) || !unproject_floor(f->inv_vp, -1, -1, ground, a) ||
        !unproject_floor(f->inv_vp, 1, 1, ground, b))
        return wx_off(6, frame);
    float w = fabsf(b[0] - a[0]), d = fabsf(b[2] - a[2]);
    // the camera (from the view matrix: -R^T t); the volume stays well below it (drops near the lens would be huge)
    // and leans toward it as the view does (its middle on the centre ray)
    float cp[3];
    if (g_cam_view)
        for (int i = 0; i < 3; i++) cp[i] = -(g_cam[0 * 4 + i] * g_cam[3] + g_cam[1 * 4 + i] * g_cam[7] + g_cam[2 * 4 + i] * g_cam[11]);
    else {  // (only the view-projection: the camera is the point it takes to clip x = y = w = 0 - inv_vp (0, 0, 1, 0))
        float h = f->inv_vp[14];
        if (!(fabsf(h) > 1e-9f)) return wx_off(5, frame);
        for (int i = 0; i < 3; i++) cp[i] = f->inv_vp[i * 4 + 2] / h;
    }
    float cam_h = cp[1] - c[1];
    if (!(cam_h > 1)) return wx_off(7, frame);
    // (the overworld camera: ~17.7 above the ground, its view ~24 x 19 of it; the volume from 1.5 under the ground to
    // 8.5 over it, half way to the camera)
    const float size = 34, height = 10, mid = height * 0.5f - 1.5f;
    bool snow = snowy(kind), storm = kind == WEATHER_STORM, bliz = kind == WEATHER_BLIZZARD;
    for (int i = 0; i < 3; i++) f->origin[i] = c[i] + (cp[i] - c[i]) * fminf(mid / cam_h, 0.5f);
    f->origin[3] = ground;
    f->box[0] = size, f->box[1] = height, f->box[2] = (float)fmod(t, 3600.0);
    for (int i = 0; i < 3; i++) f->cam[i] = cp[i];
    // (fall speeds fixed: with the time in them, any change would move every drop. The wind's drift is integrated
    // over time instead - weather_wind - so a gust carries the flakes along without moving them back or ahead)
    weather_wind(f, kind, dt > 0 && dt < 0.25 ? dt : 0, size, level);
    if (snow) {
        f->count = bliz ? 5200 : 2000;
        f->shape[0] = 0, f->shape[1] = bliz ? 8.0f : 9.0f, f->shape[2] = bliz ? 3.2f : 2.2f, f->shape[3] = bliz ? 0.3f : 0.5f;
        f->box[3] = 0.6f;
        f->splashes = f->count;  // (every flake lies where it landed)
        f->cam[3] = 1.5f;  // (for 1.5 s, fading)
        f->color[0] = 0.97f, f->color[1] = 0.98f, f->color[2] = 1.0f, f->color[3] = 0.9f;
        f->fill[3] = 1;  // (no overcast: snow is bright)
        if (bliz) {  // (a white haze: the far less clear, more so in a gust)
            float g = sqrtf(f->wind[2] * f->wind[2] + f->wind[3] * f->wind[3]) / 9;
            float k = 0.055f + 0.065f * fminf(g, 1);
            f->fill[0] = 0.92f * k, f->fill[1] = 0.94f * k, f->fill[2] = 0.98f * k, f->fill[3] = 1 - k;
        }
    } else {
        f->count = storm ? 2000 : 1300;
        f->splashes = f->count / 3;  // (a ring for every third drop)
        f->shape[0] = storm ? 2.6f : 2.2f, f->shape[1] = 1.9f, f->shape[2] = storm ? 36 : 30;
        f->box[3] = storm ? 8 : 3;
        f->cam[3] = 0.35f;
        f->color[0] = 0.80f, f->color[1] = 0.85f, f->color[2] = 0.95f, f->color[3] = storm ? 0.34f : 0.28f;
        f->fill[3] = storm ? 0.74f : 0.93f;  // (overcast: a storm's heavier clouds)
        if (storm) {  // (a few leaves blowing about: the Leaves colour if one is set, else the texture's own)
            uint32_t rgb;
            if (col_target(&g_settings.leaves, COL_SLOT_LEAVES, &rgb))  // (sRGB to the scene's linear)
                for (int i = 0; i < 3; i++) f->leaf[i] = powf((float)((rgb >> (16 - 8 * i)) & 0xFF) / 255.0f, 2.2f);
            else f->leaf[0] = -1, f->leaf[1] = f->leaf[2] = 1;  // (-1: the game's own green / the texture's colour)
            f->leaf[3] = 40;  // (in the volume, 34 x 34: ~15 on screen)
        }
        if (storm && level > 0.9f) {  // (not while a storm is fading in or out)
            storm_strikes();
            float fl = storm_flash(t, frame);
            f->fill[0] = fl * 0.22f, f->fill[1] = fl * 0.25f, f->fill[2] = fl * 0.30f;
            f->fill[3] += (1 - f->fill[3]) * fl;  // (the flash lifts the overcast too)
        }
    }
    if (level < 1) {  // (fading: fewer drops, less overcast and haze; lightning is left for the full storm)
        f->count = (uint32_t)(f->count * level), f->splashes = (uint32_t)(f->splashes * level);
        f->leaf[3] *= level;
        for (int i = 0; i < 3; i++) f->fill[i] *= level;
        f->fill[3] = 1 - (1 - f->fill[3]) * level;
    }
    f->flags[3] = snow ? 2 : 1;
    static int logged;  // LARECOMP_LOG_WEATHER: the camera and volume (calibration)
    if (getenv("LARECOMP_LOG_WEATHER") && logged++ % 120 == 0)
        LOG("[weather] kind %d ground %.2f (%d probes) centre %.2f %.2f %.2f floor %.2f x %.2f size %.2f height %.2f cam %.2f %.2f %.2f\n", kind, ground, ny, c[0], c[1],
            c[2], w, d, size, height, cp[0], cp[1], cp[2]);
    return wx_off(0, frame);
}
