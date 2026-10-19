#include "gameversion.h"
#include "game_addresses.h"
#include <stdio.h>
#include <string.h>

static GameVersion current;

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

GameVersion game_version_id(const char *id) {
    if (!strcmp(id, "ae16f71e002af8cb059a9a74c4d90f34ba984892")) return GAME_100;
    if (!strcmp(id, "909e904af78ac1b8deefe97ab2ccdb51968f0ec7")) return GAME_110;
    return GAME_UNSUPPORTED;
}

GameVersion game_version_file(const char *path) {
    char id[41];
    return game_build_id(path, id) ? game_version_id(id) : GAME_UNSUPPORTED;
}

const char *game_version_name(GameVersion version) {
    return version == GAME_100 ? "1.0.0" : version == GAME_110 ? "1.1.0" : "unsupported";
}

void game_version_select(const char *path) { current = game_version_file(path); }
GameVersion game_version_current(void) { return current; }

uint64_t game_main_address(uint64_t canonical) {
    // See tools/game_versions.py. Data/BSS and earlier functions retain their layout.
    if (current == GAME_110 && canonical >= GA_VERSION_TEXT_CUT && canonical < GA_VERSION_TEXT_END)
        return canonical + 0xd0;
    return canonical;
}

uint64_t game_canonical_address(uint64_t actual) {
    if (current == GAME_110 && actual >= GA_VERSION_TEXT_CUT + 0xd0 && actual < GA_VERSION_TEXT_END + 0xd0)
        return actual - 0xd0;
    return actual;
}
