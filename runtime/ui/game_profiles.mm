#include "../game_profiles.h"
#include <filesystem>
#include <string>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif
namespace fs = std::filesystem;
static int active;
static const char *dirs[] = {"extracted", "games/v1.0.0", "games/v1.1.0", "games/acnh/v1.0.0"};
static const char *saves[] = {"save", "save/v1.0.0", "save/v1.1.0", "save/acnh/v1.0.0"};
static const char *names[] = {"Existing installation", "Link's Awakening 1.0.0", "Link's Awakening 1.1.0", "Animal Crossing: New Horizons 1.0.0"};
static const char *ids[] = {"existing", "las/1.0.0", "las/1.1.0", "acnh/1.0.0"};
static int bounded(int p) {
    return p >= 0 && p < GAME_PROFILE_COUNT ? p : 0;
}
extern "C" const char *game_profile_dir(int p) {
    return dirs[bounded(p)];
}
extern "C" const char *game_profile_save_dir(int p) {
    return saves[bounded(p)];
}
extern "C" const char *game_profile_name(int p) {
    return names[bounded(p)];
}
extern "C" const char *game_profile_version(int p) {
    return p == GAME_PROFILE_BASE || p == GAME_PROFILE_ACNH ? "1.0.0" : p == GAME_PROFILE_UPDATED ? "1.1.0" : NULL;
}
extern "C" const char *game_profile_title(int p) {
    return p == GAME_PROFILE_ACNH ? "acnh" : p == GAME_PROFILE_BASE || p == GAME_PROFILE_UPDATED ? "las" : NULL;
}
extern "C" const char *game_profile_id(int p) { return ids[bounded(p)]; }
extern "C" int game_profile_from_id(const char *id) {
    if (id) for (int i = 0; i < GAME_PROFILE_COUNT; i++) if (!strcmp(id, ids[i])) return i;
    return -1;
}
extern "C" int game_profile_match(const char *title, const char *version) {
    if (title && version) for (int i = 1; i < GAME_PROFILE_COUNT; i++)
        if (!strcmp(title, game_profile_title(i)) && !strcmp(version, game_profile_version(i))) return i;
    return -1;
}
static bool plain(const fs::path &path) {
    fs::path part;
    for (const auto &c : fs::absolute(path)) {
        part /= c;
        if (fs::is_symlink(fs::symlink_status(part)))
            return false;
#ifdef _WIN32
        auto attr = GetFileAttributesW(part.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
#endif
    }
    return true;
}
extern "C" int game_profile_index(const char *game_dir, const char *data_root) {
    try {
        if (!game_dir || !data_root || !*game_dir || !*data_root)
            return -1;
        auto dir = fs::absolute(fs::u8path(game_dir)), root = fs::absolute(fs::u8path(data_root));
        if (!plain(dir) || !plain(root))
            return -1;
        auto normalized = dir.lexically_normal();
        for (int i = 0; i < GAME_PROFILE_COUNT; ++i) {
            auto want = (root / fs::u8path(dirs[i])).lexically_normal();
#ifdef _WIN32
            if (_wcsicmp(normalized.c_str(), want.c_str()) == 0)
                return i;
#else
            if (normalized == want)
                return i;
#endif
        }
    } catch (...) {
    }
    return -1;
}
extern "C" void game_profile_activate(const char *game_dir, const char *data_root) {
    active = bounded(game_profile_index(game_dir, data_root));
}
extern "C" int game_profile_active(void) {
    return active;
}
