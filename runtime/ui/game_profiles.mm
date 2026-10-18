#include "../game_profiles.h"
#include "../game_install.h"
extern "C" {
#include "../settings.h"
}
#include <filesystem>
#include <string>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#endif
namespace fs = std::filesystem;
static int active;
static int bounded(int p) {
    return p >= 0 && p < GAME_PROFILE_COUNT ? p : 0;
}
static bool speedfix_profile(int p) {
    return game_installations[p].title && game_installations[p].version &&
           !strcmp(game_installations[p].title,"acnh") && !strcmp(game_installations[p].version,"3.0.1");
}
extern "C" const char *game_profile_base_dir(int p) {
    static thread_local std::string resolved[GAME_PROFILE_COUNT];
    p = bounded(p);
    char path[2048], why[256];
    if (game_install_resolve(game_profile_install_dir(p), path, sizeof path, why, sizeof why)) {
        if (fs::u8path(path).lexically_normal() == fs::absolute(fs::u8path(game_profile_install_dir(p))).lexically_normal())
            return game_profile_install_dir(p);
        resolved[p] = path; return resolved[p].c_str();
    }
    return game_profile_install_dir(p); // a missing/corrupt publication opens the repair flow
}
extern "C" const char *game_profile_dir(int p) {
    p=bounded(p);
    const char *base=game_profile_base_dir(p);
    if (!g_settings.acnh_speedfix60 || !speedfix_profile(p)) return base;
    static thread_local std::string variant;
    variant=std::string(base)+"/.acnh60";
    return variant.c_str();
}
extern "C" const char *game_profile_install_dir(int p) {
    return game_installations[bounded(p)].directory;
}
extern "C" const char *game_profile_save_dir(int p) {
    return game_installations[bounded(p)].save_directory;
}
extern "C" const char *game_profile_name(int p) {
    return game_installations[bounded(p)].name;
}
extern "C" const char *game_profile_version(int p) {
    return game_installations[bounded(p)].version;
}
extern "C" const char *game_profile_title(int p) {
    return game_installations[bounded(p)].title;
}
extern "C" const char *game_profile_id(int p) { return game_installations[bounded(p)].id; }
extern "C" int game_profile_from_id(const char *id) {
    if (id) for (int i = 0; i < GAME_PROFILE_COUNT; i++) if (!strcmp(id, game_profile_id(i))) return i;
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
            auto want = (root / fs::u8path(game_profile_install_dir(i))).lexically_normal();
            auto matches=[&](const fs::path &candidate) {
#ifdef _WIN32
                if (_wcsicmp(normalized.c_str(),candidate.c_str())==0) return true;
                return speedfix_profile(i) && _wcsicmp(normalized.c_str(),(candidate/".acnh60").c_str())==0;
#else
                return normalized==candidate || (speedfix_profile(i) && normalized==candidate/".acnh60");
#endif
            };
            if (matches(want)) return i;
            char resolved[2048], why[256];
            if (game_install_resolve(want.u8string().c_str(), resolved, sizeof resolved, why, sizeof why)) {
                auto published = fs::u8path(resolved).lexically_normal();
                if (matches(published)) return i;
            }
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
