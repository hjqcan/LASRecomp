// The Cosmetics tab's shared colour palette: see cosmetic_colour.h.
#include "cosmetic_colour.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

const char *const g_col_names[N_COL] = {"Default", "Red",   "Orange", "Gold",  "Brown",  "Green",  "Teal", "Blue",
                                        "Purple",  "Pink",  "White",  "Black", "Silver", "Random", "Custom"};
const uint32_t g_col_rgb[N_COL] = {0,        0xD63A3A, 0xF08A24, 0xE8B830, 0x7A4A28, 0x3FA34D, 0x22A7A0, 0x2F6FD6,
                                   0x8A4FD8, 0xE86AA8, 0xF2F0EA, 0x2A2A2E, 0xA9B0B8, 0,        0};

void col_parse(CosColour *c, const char *val) {
    c->pick = COL_DEFAULT;
    if (val[0] == '#' && strlen(val) == 7) {
        char *end;
        unsigned long v = strtoul(val + 1, &end, 16);
        if (!*end) { c->pick = COL_CUSTOM; c->rgb = (uint32_t)v; }
        return;
    }
    for (int i = 0; i < N_COL; i++)
        if (i != COL_CUSTOM && !strcasecmp(val, g_col_names[i])) { c->pick = i; return; }
    // older versions' names (tunic: Yellow, Sky, Grey, Lime, Magenta; hair: Blond)
    static const char *const old[][2] = {{"Yellow", "Gold"}, {"Sky", "Blue"}, {"Grey", "Silver"}, {"Lime", "Green"},
                                         {"Magenta", "Pink"}, {"Blond", "Gold"}};
    for (size_t k = 0; k < sizeof old / sizeof *old; k++)
        if (!strcasecmp(val, old[k][0]))
            for (int i = 0; i < N_COL; i++)
                if (!strcmp(old[k][1], g_col_names[i])) c->pick = i;
}

void col_format(const CosColour *c, char *out, size_t n) {
    if (c->pick == COL_CUSTOM) snprintf(out, n, "#%06X", (unsigned)(c->rgb & 0xFFFFFF));
    else snprintf(out, n, "%s", g_col_names[c->pick >= 0 && c->pick < N_COL ? c->pick : 0]);
}

int (*g_col_seed_pick)(int slot, uint32_t *rgb);

static int seed_pick(const CosColour *c, int slot, uint32_t *rgb) {
    if (c->pick != COL_DEFAULT || !g_col_seed_pick) return COL_DEFAULT;
    uint32_t v = 0;
    int p = g_col_seed_pick(slot, &v);
    if (p > COL_DEFAULT && p < COL_RANDOM) v = g_col_rgb[p];
    else if (p != COL_CUSTOM) return COL_DEFAULT;
    if (rgb) *rgb = v & 0xFFFFFF;
    return p;
}

bool col_seed_rgb(const CosColour *c, int slot, uint32_t *rgb) { return seed_pick(c, slot, rgb) != COL_DEFAULT; }

int col_pick(const CosColour *c, int slot) {
    int p = seed_pick(c, slot, NULL);
    return p ? p : c->pick;
}

bool col_target(const CosColour *c, int slot, uint32_t *rgb) {
    static int picks[COL_SLOTS];
    static unsigned seed;
    if (slot < 0 || slot >= COL_SLOTS) return false;
    if (c->pick != COL_RANDOM) picks[slot] = 0;
    if (c->pick == COL_CUSTOM) { *rgb = c->rgb & 0xFFFFFF; return true; }
    if (c->pick == COL_RANDOM) {
        if (!picks[slot]) {
            if (!seed) seed = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)&seed;
            seed = seed * 1103515245u + 12345u;
            picks[slot] = 1 + (int)((seed >> 16) % (COL_RANDOM - 1));
        }
        *rgb = g_col_rgb[picks[slot]];
        return true;
    }
    if (col_seed_rgb(c, slot, rgb)) return true;  // (Default: the seed's colour, if it has one)
    int p = c->pick;
    if (p <= COL_DEFAULT || p >= COL_RANDOM) return false;
    *rgb = g_col_rgb[p];
    return true;
}

void col_hsv(uint32_t rgb, float *h, float *s, float *v) {
    float r = (rgb >> 16 & 255) / 255.0f, g = (rgb >> 8 & 255) / 255.0f, b = (rgb & 255) / 255.0f;
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn, hh = 0;
    if (d > 0) hh = mx == r ? fmodf((g - b) / d + 6, 6) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
    *h = hh / 6;
    *s = mx > 0 ? d / mx : 0;
    *v = mx;
}

void col_mapping(float src_h, float src_s, float src_v, uint32_t dst, float *shift, float *sat, float *val) {
    float h, s, v;
    col_hsv(dst, &h, &s, &v);
    *shift = s < 0.08f ? 0 : h - src_h;
    *sat = src_s > 0.01f ? s / src_s : 1;
    *val = src_v > 0.01f ? v / src_v : 1;
}
