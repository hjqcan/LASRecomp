// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"
#include "gameversion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "paths.h"
#include "rt.h"

static const GameLibTables *g_tables;
static char g_error[256];
static char g_research_stores[65];
const char *gamelib_research_stores(void) { return g_research_stores[0]?g_research_stores:NULL; }

// The library's id file (<library>.id, written next to it by the build): its GAMELIB_ID, read without loading it.
static bool id_matches(const char *lib, const char *game_dir) {
    char path[1100], id[128] = {0};
    snprintf(path, sizeof path, "%s.id", lib);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(id, 1, sizeof id - 1, f);
    fclose(f);
    while (n && (id[n - 1] == '\n' || id[n - 1] == '\r')) id[--n] = 0;
    char *build = strchr(id, '\n');
    if (!build || !game_dir) return false;
    *build++ = 0;
    id[strcspn(id, "\r")] = 0;
    char main_path[1100], current_id[41];
    snprintf(main_path, sizeof main_path, "%s/exefs/main", game_dir);
    return !strcmp(id, GAMELIB_ID) && game_build_id(main_path, current_id) && !strcmp(build, current_id);
}

static bool exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// The places to look, in order (gamelib.h). Returns how many were filled.
static int candidates(const char *game_dir, char out[3][1100]) {
    int n = 0;
    const char *env = getenv("LARECOMP_GAME_LIB");
    if (env && *env) snprintf(out[n++], 1100, "%s", env);
    char exe_side[1024];
    if (rt_resource(GAMELIB_NAME, NULL, exe_side, sizeof exe_side) && exists(exe_side))
        snprintf(out[n++], 1100, "%s", exe_side);
    if (game_dir) snprintf(out[n++], 1100, "%s/%s", game_dir, GAMELIB_NAME);
    return n;
}

bool gamelib_present(const char *game_dir) {
    if (g_tables) return true;
    char c[3][1100];
    int n = candidates(game_dir, c);
    for (int i = 0; i < n; i++)
        if (exists(c[i]) && id_matches(c[i], game_dir)) return true;
    return false;
}

static const GameLibTables *open_lib(const char *path, const char *game_dir) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h) { snprintf(g_error, sizeof g_error, "can't load %s (error %lu)", path, GetLastError()); return NULL; }
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(h, GAMELIB_ENTRY);
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(g_error, sizeof g_error, "%s", dlerror()); return NULL; }
    GameLibEntry entry = (GameLibEntry)dlsym(h, GAMELIB_ENTRY);
#endif
    if (!entry) { snprintf(g_error, sizeof g_error, "%s: not game code", path); return NULL; }
    const GameLibTables *t = entry();
    if (!t || t->abi != GAMELIB_ABI || !t->id || strcmp(t->id, GAMELIB_ID)) {
        snprintf(g_error, sizeof g_error, "%s was built for another version", path);
        return NULL;  // (left loaded: nothing of it runs)
    }
    char main_path[1100], current_id[41];
    snprintf(main_path, sizeof main_path, "%s/exefs/main", game_dir);
    if (!t->main_build_id || !game_build_id(main_path, current_id) || strcmp(t->main_build_id, current_id)) {
        snprintf(g_error, sizeof g_error, "%s was built for a different game executable", path);
        return NULL;
    }
    typedef const char *(*ResearchStoresEntry)(void);
#ifdef _WIN32
    ResearchStoresEntry research = (ResearchStoresEntry)(void *)GetProcAddress(h, "lasgame_research_stores");
#else
    ResearchStoresEntry research = (ResearchStoresEntry)dlsym(h, "lasgame_research_stores");
#endif
    g_research_stores[0]=0;
    if(research) {
        const char *manifest=research();
        if(!manifest||strnlen(manifest,65)!=64||strspn(manifest,"0123456789abcdef")!=64) {
            snprintf(g_error,sizeof g_error,"%s has invalid research-library metadata",path);return NULL;
        }
        memcpy(g_research_stores,manifest,65);
    }
    return t;
}

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    static bool tried;
    if (!tried) {
        tried = true;
        snprintf(g_error, sizeof g_error, "the game code hasn't been built: set up the game files on the start screen");
        char c[3][1100];
        int n = candidates(game_dir, c);
        const char *env = getenv("LARECOMP_GAME_LIB");
        for (int i = 0; i < n && !g_tables; i++) {
            if (!exists(c[i])) continue;
            bool given = i == 0 && env && *env;  // LARECOMP_GAME_LIB: loaded as given, the library's own id decides
            if (!given && !id_matches(c[i], game_dir)) {
                snprintf(g_error, sizeof g_error, "%s was built for another version", c[i]);
                continue;
            }
            g_tables = open_lib(c[i], game_dir);
            if (g_tables) LOG("[gamelib] %s (%llu + %llu functions)\n", c[i], (unsigned long long)*g_tables->main_count,
                              (unsigned long long)*g_tables->sdk_count);
        }
    }
    if (!g_tables && why) snprintf(why, why_size, "%s", g_error);
    return g_tables != NULL;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
