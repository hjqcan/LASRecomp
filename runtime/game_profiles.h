#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *id, *directory, *save_directory, *name, *title, *version, *setting;
    bool las_extensions;
} GameInstallation;
#include "game_installations.inc"
static inline int game_profile_parse_setting(const char *value) {
    for (int i = 0; i < GAME_PROFILE_COUNT; ++i)
        if (!strcmp(value, game_installations[i].id) || !strcmp(value, game_installations[i].setting)) return i;
    return GAME_PROFILE_EXISTING;
}
static inline const char *game_profile_setting(int profile) {
    return game_installations[profile >= 0 && profile < GAME_PROFILE_COUNT ? profile : 0].setting;
}
static inline bool game_profile_las_extensions(int profile) {
    return profile >= 0 && profile < GAME_PROFILE_COUNT && game_installations[profile].las_extensions;
}
const char *game_profile_dir(int profile);
const char *game_profile_install_dir(int profile); // stable slot; may contain a published generation
const char *game_profile_save_dir(int profile);
const char *game_profile_name(int profile);
const char *game_profile_version(int profile); // NULL for the existing/custom installation
const char *game_profile_title(int profile);   // NULL for an existing/custom installation
const char *game_profile_id(int profile);      // stable launch identifier
int game_profile_from_id(const char *id);
int game_profile_match(const char *title, const char *version);
// Match only these exact managed directories under data_root; links and outside directories return -1.
int game_profile_index(const char *game_dir, const char *data_root);
void game_profile_activate(const char *game_dir, const char *data_root);
int game_profile_active(void);
#ifdef __cplusplus
}
#endif
