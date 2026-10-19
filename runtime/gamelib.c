// Loads the recompiled game code library (runtime/gamelib.h).
#include "gamelib.h"
#include "gameversion.h"
#include "acnh_speedfix.h"

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
    char path[1100], id[256] = {0};
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
    char *adapter = strchr(build, '\n');
    if (!adapter) return false;
    *adapter++ = 0;
    build[strcspn(build, "\r")] = 0;
    char main_path[1100], current_id[65], why[256];
    snprintf(main_path, sizeof main_path, "%s/exefs/main", game_dir);
    const TitleProfile *p = acnh60_profile(title_profile_validate(game_dir, why, sizeof why), game_dir, why, sizeof why);
    return p && !strcmp(id, GAMELIB_ID) && title_build_id(main_path, current_id) && !strcmp(build, current_id) &&
           !strcmp(adapter, p->code_identity);
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
    if (g_tables) {
        char why[256];
        const TitleProfile *p=acnh60_profile(title_profile_validate(game_dir,why,sizeof why),game_dir,why,sizeof why);
        return p && !strcmp(g_tables->modules[0].build_id,p->modules[0].build_id) &&
               g_tables->adapter_id && !strcmp(g_tables->adapter_id,p->code_identity);
    }
    char c[3][1100];
    int n = candidates(game_dir, c);
    for (int i = 0; i < n; i++)
        if (exists(c[i]) && id_matches(c[i], game_dir)) return true;
    return false;
}

static const GameLibTables *validate_tables(const GameLibTables *t, const char *path, const char *game_dir) {
    if (!t || t->abi != GAMELIB_ABI || !t->id || strcmp(t->id, GAMELIB_ID)) {
        snprintf(g_error, sizeof g_error, "%s was built for another version", path);
        return NULL;  // (left loaded: nothing of it runs)
    }
    const TitleProfile *p=acnh60_profile(title_profile_validate(game_dir,g_error,sizeof g_error),game_dir,g_error,sizeof g_error);
    if (!p) return NULL;
    if (!t->game_id || !t->version || strcmp(t->game_id,p->key) || strcmp(t->version,p->version) ||
        t->adapter_revision!=p->adapter_revision || t->module_count!=p->module_count || !t->modules ||
        !t->adapter_id || strcmp(t->adapter_id,p->code_identity)) {
        snprintf(g_error,sizeof g_error,"The game library belongs to another adapter or version"); return NULL;
    }
    for (unsigned i=0;i<p->module_count;i++) {
        const GameLibModule *m=&t->modules[i]; const TitleModule *expected=&p->modules[i];
        if (!m->name || strcmp(m->name,expected->name) || m->base!=expected->base ||
            !m->build_id || strcmp(m->build_id,expected->build_id) || !m->addrs || !m->fns || !m->count ||
            !*m->count || *m->count>UINT64_C(10000000)) {
            snprintf(g_error,sizeof g_error,"Invalid compiled module: %s",expected->name); return NULL;
        }
        uint64_t end=i+1<p->module_count?p->modules[i+1].base:HLE_BASE;
        for (uint64_t j=0;j<*m->count;j++) {
            if (!m->fns[j] || (m->addrs[j]&3) || m->addrs[j]<m->base || m->addrs[j]>=end ||
                (j && m->addrs[j]<=m->addrs[j-1])) {
                snprintf(g_error,sizeof g_error,"Invalid function table: %s",expected->name); return NULL;
            }
        }
    }
    return t;
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
    if (!validate_tables(t, path, game_dir)) return NULL;
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

bool gamelib_verify(const char *game_dir, char *why, size_t size) {
    if (!acnh60_validate(game_dir, why, size)) return false;
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", game_dir, GAMELIB_NAME);
    if (!id_matches(path, game_dir)) { snprintf(why, size, "The candidate library does not match this release and runtime"); return false; }
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path);
    if (!handle) { snprintf(why, size, "Cannot load candidate library (error %lu)", GetLastError()); return false; }
    GameLibEntry entry = (GameLibEntry)(void *)GetProcAddress(handle, GAMELIB_ENTRY);
#else
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) { snprintf(why, size, "Cannot load candidate library: %s", dlerror()); return false; }
    GameLibEntry entry = (GameLibEntry)dlsym(handle, GAMELIB_ENTRY);
#endif
    bool ok = entry && validate_tables(entry(), path, game_dir);
    if (!ok) snprintf(why, size, "%s", entry ? g_error : "Candidate library entry is missing");
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
    return ok;
}

bool gamelib_load(const char *game_dir, char *why, size_t why_size) {
    if (g_tables && !gamelib_present(game_dir)) {
        snprintf(why,why_size,"Start a new process to switch to another game installation");
        return false;
    }
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
            if (g_tables) LOG("[gamelib] %s (%u modules, %s %s)\n", c[i], g_tables->module_count,
                              g_tables->game_id,g_tables->version);
        }
    }
    if (!g_tables && why) snprintf(why, why_size, "%s", g_error);
    return g_tables != NULL;
}

const GameLibTables *gamelib_tables(void) { return g_tables; }
