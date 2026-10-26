// The game-address tables of every supported game version and their selection by build ID (game_addresses.h).
#include "game_addresses.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt.h"

#define GA_ADDR(name, value) .name = UINT64_C(value),
static const GameAddrs ga_v1_0_0 = {
#include "game_addresses/patches_v1_0_0.h"
#include "game_addresses/runtime_v1_0_0.h"
};
static const GameAddrs ga_v1_0_1 = {
#include "game_addresses/patches_v1_0_1.h"
#include "game_addresses/runtime_v1_0_1.h"
};
#undef GA_ADDR

static const GameVersion versions[] = {
#define GA_VERSION(key, label, build_id) {#key, label, build_id, &ga_##key},
#define GA_RELEASE(key, label, npdm_sha256)
#include "game_addresses/versions.h"
#undef GA_VERSION
#undef GA_RELEASE
};

const GameAddrs *g_ga;
static const GameVersion *g_version;

static int hexval(char c) { return c <= '9' ? c - '0' : c - 'A' + 10; }

const GameVersion *game_version_by_build_id(const uint8_t id[20]) {
    for (size_t i = 0; i < sizeof versions / sizeof versions[0]; i++) {
        const char *h = versions[i].build_id;
        int k = 0;
        while (k < 20 && id[k] == (uint8_t)(hexval(h[2 * k]) << 4 | hexval(h[2 * k + 1]))) k++;
        if (k == 20) return &versions[i];
    }
    return NULL;
}

// The main program's build ID (0x40 in the file). false: missing or not a program.
static bool read_build_id(const char *path, uint8_t id[20]) {
    uint8_t hdr[0x60];
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "NSO0", 4);
    fclose(f);
    if (ok) memcpy(id, hdr + 0x40, 20);
    return ok;
}

const GameVersion *game_version_of_main(const char *main_path) {
    uint8_t id[20];
    return read_build_id(main_path, id) ? game_version_by_build_id(id) : NULL;
}

const GameVersion *game_version_of(const char *game_dir) {
    char path[1100];
    snprintf(path, sizeof path, "%s/exefs/main", game_dir);
    return game_version_of_main(path);
}

bool game_version_select(const char *game_dir, char *why, size_t why_size) {
    uint8_t id[20];
    char exefs[1100], path[1100];
    snprintf(exefs, sizeof exefs, "%s/exefs", game_dir);
    snprintf(path, sizeof path, "%s/main", exefs);
    if (!read_build_id(path, id)) {
        snprintf(why, why_size, "the game files' exefs/main is missing or unreadable");
        return false;
    }
    const GameVersion *v = game_version_by_build_id(id);
    if (!v) {
        snprintf(why, why_size, "these game files are an unsupported version of the game (main build ID "
                 "%02X%02X%02X%02X%02X%02X%02X%02X): LAS Recomp needs version 1.0.0, 1.0.1 or 1.1.0",
                 id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
        return false;
    }
    g_version = v;
    g_ga = v->addrs;
    char label[32];
    LOG("[game] version %s (main build ID %.16s, address table %s)\n",
        game_release_label(exefs, label, sizeof label), v->build_id, v->key);
    return true;
}

const GameVersion *game_version(void) { return g_version; }

uint64_t game_romfs_bytes(const GameVersion *v) {
    if (!v) return 0;
    if (!strcmp(v->key, "v1_0_0")) return 6232003695ull;
    if (!strcmp(v->key, "v1_0_1")) return 6232003903ull;  // (1.0.1 and 1.1.0 have the same RomFS)
    return 0;
}

static const struct { uint64_t hash; uint8_t versions; } romfs_files[] = {
#include "game_addresses/romfs_files.h"
};
enum { ROMFS_ENTRIES = sizeof romfs_files / sizeof *romfs_files };
struct GameRomfsCheck {
    uint8_t version;              // romfs_files' bit for the version
    uint8_t seen[ROMFS_ENTRIES];  // (a file can be passed once: a folder has one file of a name)
};

GameRomfsCheck *game_romfs_check_begin(const GameVersion *v) {
    uint8_t bit = !v ? 0 : !strcmp(v->key, "v1_0_0") ? 1 : !strcmp(v->key, "v1_0_1") ? 2 : 0;
    if (!bit) return NULL;
    GameRomfsCheck *c = calloc(1, sizeof *c);
    if (c) c->version = bit;
    return c;
}

uint64_t game_romfs_hash(const char *rel, uint64_t size) {
    char digits[24];
    snprintf(digits, sizeof digits, "%llu", (unsigned long long)size);
    uint64_t h = 0xcbf29ce484222325ull;  // FNV-1a 64 of "<rel>\0<size>", as tools/romfs_files.py
    for (const unsigned char *p = (const unsigned char *)rel; ; p++) {
        h = (h ^ *p) * 0x100000001b3ull;
        if (!*p) break;
    }
    for (const char *p = digits; *p; p++) h = (h ^ (unsigned char)*p) * 0x100000001b3ull;
    return h;
}

bool game_romfs_check_file(GameRomfsCheck *c, const char *rel, uint64_t size) {
    if (!c) return true;
    uint64_t h = game_romfs_hash(rel, size);
    size_t lo = 0, hi = ROMFS_ENTRIES;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (romfs_files[mid].hash < h) lo = mid + 1;
        else hi = mid;
    }
    if (lo == ROMFS_ENTRIES || romfs_files[lo].hash != h || !(romfs_files[lo].versions & c->version)) return false;
    c->seen[lo] = 1;
    return true;
}

int game_romfs_check_end(GameRomfsCheck *c) {
    if (!c) return 0;
    int missing = 0;
    for (size_t i = 0; i < ROMFS_ENTRIES; i++) missing += (romfs_files[i].versions & c->version) && !c->seen[i];
    free(c);
    return missing;
}

bool game_romfs_exefs_copy(const char *game_dir, const char *rel, uint64_t size) {
    static const char *const names[] = {"main", "main.npdm", "rtld", "sdk", "subsdk0", "subsdk1"};
    bool named = false;
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) named |= !strcmp(rel, names[i]);
    if (!named || !game_dir) return false;
    char path[1200];
    snprintf(path, sizeof path, "%s/exefs/%s", game_dir, rel);
    FILE *a = fopen(path, "rb");
    snprintf(path, sizeof path, "%s/romfs/%s", game_dir, rel);
    FILE *b = fopen(path, "rb");
    enum { CHUNK = 1 << 16 };
    char *x = malloc(2 * CHUNK), *y = x ? x + CHUNK : NULL;
    bool same = a && b && x;
    uint64_t n = 0;
    while (same) {
        size_t r = fread(x, 1, CHUNK, a), s = fread(y, 1, CHUNK, b);
        same = r == s && !memcmp(x, y, r);
        n += r;
        if (r < CHUNK) break;
    }
    free(x);
    if (a) fclose(a);
    if (b) fclose(b);
    return same && n == size;
}
