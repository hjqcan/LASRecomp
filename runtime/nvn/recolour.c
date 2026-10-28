// Live cosmetic recolours by draw: see recolour.h.
//
// The textures are found in the user's own game files (each option names its files and which of their textures it
// wants), once, on the first texture upload: only their sizes and a hash of their bytes are kept. An uploaded texture
// of the same size, mip count and bytes is that texture (the game uploads the files' block-linear data as is, the way
// texpatch.c recognises the tunic). The renderers ask recolour_class_of for each texture a draw samples and, for a
// draw with a class, set the per-draw tint from recolour_tint.
#include "recolour.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../hle.h"

// Each recolouring option (one per Cosmetics row; dropping one = removing its line and its file)
extern const RecolourOption g_recolour_link;  // hair / shield colours (recolour_link.c)
extern const RecolourOption g_recolour_seasons;  // leaves / grass colours (recolour_seasons.c)
extern const RecolourOption g_recolour_water;  // water colour (recolour_water.c)
extern const RecolourOption g_recolour_buildings;  // fence / wall / house colours (recolour_buildings.c)
extern const RecolourOption g_recolour_enemies;  // random enemy colours (recolour_enemies.c)
// LARECOMP_RECOLOUR_NAMES=1 (test): every albedo map of the overworld's files and map models is recognised too (class
// RECOLOUR_NAMED, never tinted), so LARECOMP_LOG_RECOLOUR_AT can name what each draw samples
enum { RECOLOUR_NAMED = 15 };
static int named(int source, const char *t) {
    (void)source;
    size_t n = strlen(t);
    return n > 4 && !strcmp(t + n - 4, "_alb") ? RECOLOUR_NAMED : 0;
}
static bool named_tint(int cls, float t[8]) { (void)cls, (void)t; return false; }
static RecolourOption g_recolour_named = {"names (test)", NULL, 0, RECOLOUR_NAMED, RECOLOUR_NAMED, named_tint};
static void named_sources(void) {  // Field.bntx, every actor's file and every map model's (houses, caves)
    enum { MAXN = 4096 };
    static RecolourSource src[MAXN];
    static char paths[MAXN][96];
    int n = 0;
    src[n++] = (RecolourSource){"region_common/map/Field.bntx", named};
    static const char *const dirs[] = {"actor", "map"};
    for (int k = 0; k < 2; k++) {
        char dir[1200];
        snprintf(dir, sizeof dir, "%s/region_common/%s", g_romfs_dir ? g_romfs_dir : "extracted/romfs", dirs[k]);
        DIR *d = opendir(dir);
        for (struct dirent *e; d && (e = readdir(d)) && n < MAXN;) {
            size_t l = strlen(e->d_name);
            if (l < 7 || l > 70 || strcmp(e->d_name + l - 6, ".bfres")) continue;
            snprintf(paths[n], sizeof paths[n], "region_common/%s/%s", dirs[k], e->d_name);
            src[n] = (RecolourSource){paths[n], named};
            n++;
        }
        if (d) closedir(d);
    }
    g_recolour_named.sources = src;
    g_recolour_named.nsources = n;
}

static const RecolourOption *g_options[] = {
    &g_recolour_link,
    &g_recolour_seasons,
    &g_recolour_water,
    &g_recolour_buildings,
    &g_recolour_enemies,
    NULL,
    NULL,
};

typedef struct {
    uint16_t w, h, levels, cls;
    uint32_t size;
    uint64_t hash;
    char name[40];  // (the texture's name, for the test logs)
} CRef;

static CRef *g_cr;
static int g_ncr, g_cap;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static uint64_t hash64(const uint8_t *p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

// Random access into the BNTX at `base` of an open file (Field.bntx is 270 MB: only what is needed is read)
static bool rd_at(FILE *f, uint64_t at, void *buf, size_t n) {
#ifdef _WIN32
    if (_fseeki64(f, (long long)at, SEEK_SET)) return false;
#else
    if (fseeko(f, (off_t)at, SEEK_SET)) return false;
#endif
    return fread(buf, 1, n, f) == n;
}

// Where the file's BNTX starts (a model / effect archive embeds one, 4-aligned), or -1
static int64_t find_bntx(FILE *f) {
    enum { CH = 1 << 20 };
    uint8_t *b = malloc(CH + 8);
    int64_t found = -1;
    uint64_t at = 0;
    size_t keep = 0, n;
    while (found < 0 && (n = fread(b + keep, 1, CH, f)) > 0) {
        size_t have = keep + n;
        for (size_t i = 0; i + 8 <= have; i += 4)
            if (!memcmp(b + i, "BNTX\0\0\0\0", 8)) { found = (int64_t)(at + i); break; }
        if (have >= 8) {  // (keep the last 8 bytes: a header across chunks; `at` stays 4-aligned)
            memmove(b, b + have - 8, 8);
            at += have - 8;
            keep = 8;
        }
    }
    free(b);
    return found;
}

static void add(uint16_t w, uint16_t h, uint16_t levels, uint32_t size, uint64_t hash, int cls, const char *name) {
    for (int i = 0; i < g_ncr; i++)  // (files share textures: the first class wins)
        if (g_cr[i].hash == hash && g_cr[i].size == size && g_cr[i].w == w && g_cr[i].h == h) return;
    if (g_ncr == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 256;
        g_cr = realloc(g_cr, sizeof *g_cr * (size_t)g_cap);
    }
    g_cr[g_ncr] = (CRef){w, h, levels, (uint16_t)cls, size, hash, {0}};
    snprintf(g_cr[g_ncr++].name, sizeof g_cr->name, "%s", name);
}

static int scan_file(const RecolourOption *o, int src) {
    char p[1200];
    snprintf(p, sizeof p, "%s/%s", g_romfs_dir ? g_romfs_dir : "extracted/romfs", o->sources[src].file);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    int got = 0;
    int64_t base = find_bntx(f);
    uint8_t hd[0x30];
    if (base >= 0 && rd_at(f, (uint64_t)base, hd, sizeof hd) && !memcmp(hd + 0x20, "NX  ", 4)) {
        uint32_t count = rd32(hd + 0x24);
        uint64_t infos = rd64(hd + 0x28);
        uint8_t *ptrs = count < 100000 ? malloc((size_t)count * 8) : NULL;
        if (ptrs && rd_at(f, (uint64_t)base + infos, ptrs, (size_t)count * 8))
            for (uint32_t i = 0; i < count; i++) {
                uint8_t t[0x90], nl[2];
                char name[96];
                uint64_t brti = rd64(ptrs + 8 * i);
                if (!rd_at(f, (uint64_t)base + brti, t, sizeof t) || memcmp(t, "BRTI", 4)) continue;
                const uint8_t *ti = t + 16;
                uint64_t name_at = rd64(ti + 80), data_ptrs = rd64(ti + 96);
                if (!rd_at(f, (uint64_t)base + name_at, nl, 2) || rd16(nl) >= sizeof name) continue;
                uint16_t n = rd16(nl);
                if (!rd_at(f, (uint64_t)base + name_at + 2, name, n)) continue;
                name[n] = 0;
                int cls = o->sources[src].classify(src, name);
                if (cls <= 0) continue;
                uint32_t w = rd32(ti + 20), h = rd32(ti + 24), size = rd32(ti + 64), levels = rd16(ti + 6);
                uint8_t dp[8];
                if (!size || size > (64u << 20) || !rd_at(f, (uint64_t)base + data_ptrs, dp, 8)) continue;
                uint8_t *data = malloc(size);
                if (rd_at(f, (uint64_t)base + rd64(dp), data, size)) {
                    add((uint16_t)w, (uint16_t)h, (uint16_t)levels, size, hash64(data, size), cls, name);
                    got++;
                }
                free(data);
            }
        free(ptrs);
    }
    fclose(f);
    return got;
}

static void load(void) {
    if (getenv("LARECOMP_RECOLOUR_NAMES")) named_sources(), g_options[5] = &g_recolour_named;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int k = 0; g_options[k]; k++) {
        const RecolourOption *o = g_options[k];
        int n = 0;
        for (int s = 0; s < o->nsources; s++) n += scan_file(o, s);
        LOG("[recolour] %s: %d textures from %d files\n", o->name, n, o->nsources);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    LOG("[recolour] game files read in %.0f ms\n", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
}

int recolour_identify(const NvnTexture *t, uint64_t total) {
    pthread_once(&g_once, load);
    uint32_t hsize[4] = {0};
    uint64_t hval[4] = {0};
    int nh = 0;
    for (int i = 0; i < g_ncr; i++) {
        const CRef *r = &g_cr[i];
        if (r->w != t->width || r->h != t->height || r->levels != t->levels || r->size > total) continue;
        uint64_t h = 0;
        int k = 0;
        for (; k < nh && hsize[k] != r->size; k++) {}
        if (k < nh) h = hval[k];
        else {
            h = hash64((const uint8_t *)(uintptr_t)t->addr, r->size);
            if (nh < 4) hsize[nh] = r->size, hval[nh] = h, nh++;
        }
        if (h == r->hash) {
            static int nlog;
            if (nlog++ < 48 && r->cls != RECOLOUR_NAMED)
                LOG("[recolour] %ux%u (%u levels) uploaded: class %d (%s)\n", r->w, r->h, r->levels, r->cls, r->name);
            return i;
        }
    }
    return -1;
}

const char *recolour_name(int id) {
    if (id < TEXPATCH_RECOLOUR_BASE || id - TEXPATCH_RECOLOUR_BASE >= g_ncr) return NULL;
    return g_cr[id - TEXPATCH_RECOLOUR_BASE].name;
}

int recolour_class_of(int id) {
    if (id < TEXPATCH_RECOLOUR_BASE || id - TEXPATCH_RECOLOUR_BASE >= g_ncr) return 0;
    return g_cr[id - TEXPATCH_RECOLOUR_BASE].cls;
}

static bool active(int cls) {
    float t[8] = {0};
    for (int k = 0; g_options[k]; k++)
        if (cls >= g_options[k]->cls_lo && cls <= g_options[k]->cls_hi) return g_options[k]->tint(cls, t);
    return false;
}

int recolour_pick(int a, int b) {
    if (!a || a == b) return b ? b : a;
    if (!b) return a;
    bool aa = active(a), bb = active(b);
    if (aa != bb) return aa ? a : b;
    return a > b ? a : b;
}

void recolour_add(int cls[2], int c) {
    if (!c || c == cls[0] || c == cls[1]) return;
    int p = recolour_pick(cls[0], c), other = p == c ? cls[0] : c;  // (other: the one not picked, or 0)
    cls[0] = p;
    if (other) cls[1] = recolour_pick(cls[1], other);
}

void recolour_tint_draw(const int cls[2], float tint[RECOLOUR_TINT_FLOATS]) {
    if (!cls[0]) return;
    recolour_tint(cls[0], tint);
    if (!cls[1] || tint[3] >= 0) return;  // (a second hue window only beside a first)
    float t2[8] = {0};
    recolour_tint(cls[1], t2);
    if (t2[3] >= 0 || !memcmp(t2, tint, sizeof t2)) return;
    memcpy(tint + 8, t2, sizeof t2);
}

void recolour_tint(int cls, float tint[8]) {
    for (int k = 0; g_options[k]; k++)
        if (cls >= g_options[k]->cls_lo && cls <= g_options[k]->cls_hi) {
            g_options[k]->tint(cls, tint);
            return;
        }
}

void recolour_pack(float tint[8], float shift, float sat, float val, float centre, float width, float min_sat,
                   float spread) {
    tint[0] = shift - (float)(int)shift;
    if (tint[0] < 0) tint[0] += 1;
    tint[1] = sat;
    tint[2] = val;
    tint[3] = -1;  // (mode: recolour; > 0 is the luminance tint)
    tint[4] = centre;
    tint[5] = width;
    tint[6] = min_sat;
    tint[7] = spread;
}
