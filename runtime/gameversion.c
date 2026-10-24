#include "gameversion.h"

#include <stdio.h>
#include <string.h>

#define GA_ADDR(name, value) enum { canonical_##name = value };
#include "game_addresses/patches_v1_0_0.h"
#undef GA_ADDR

static GameLayout current;

bool game_build_id(const char *path, char out[41]) {
    unsigned char header[0x60];
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(header, 1, sizeof header, f);
    fclose(f);
    if (n != sizeof header || memcmp(header, "NSO0", 4)) return false;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        out[2 * i] = hex[header[0x40 + i] >> 4];
        out[2 * i + 1] = hex[header[0x40 + i] & 15];
    }
    out[40] = 0;
    return true;
}

GameLayout game_version_id(const char *id) {
    if (!strcmp(id, "ae16f71e002af8cb059a9a74c4d90f34ba984892")) return GAME_100;
    if (!strcmp(id, "909e904af78ac1b8deefe97ab2ccdb51968f0ec7")) return GAME_110;
    return GAME_UNSUPPORTED;
}

GameLayout game_version_file(const char *path) {
    char id[41];
    return game_build_id(path, id) ? game_version_id(id) : GAME_UNSUPPORTED;
}

const char *game_version_name(GameLayout version) {
    return version == GAME_100 ? "1.0.0" : version == GAME_110 ? "1.1.0" : "unsupported";
}

void game_layout_select(const char *path) { current = game_version_file(path); }
GameLayout game_version_current(void) { return current; }

uint64_t game_main_address(uint64_t canonical) {
    // See tools/game_versions.py. Data/BSS and earlier functions retain their layout.
    if (current == GAME_110 && canonical >= canonical_VERSION_TEXT_CUT && canonical < canonical_VERSION_TEXT_END)
        return canonical + 0xd0;
    return canonical;
}

uint64_t game_canonical_address(uint64_t actual) {
    if (current == GAME_110 && actual >= canonical_VERSION_TEXT_CUT + 0xd0 && actual < canonical_VERSION_TEXT_END + 0xd0)
        return actual - 0xd0;
    return actual;
}
