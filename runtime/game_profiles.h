#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { GAME_PROFILE_EXISTING, GAME_PROFILE_BASE, GAME_PROFILE_UPDATED, GAME_PROFILE_COUNT };
const char *game_profile_dir(int profile);
const char *game_profile_save_dir(int profile);
const char *game_profile_name(int profile);
const char *game_profile_version(int profile); // NULL for the existing/custom installation
// Match only these exact managed directories under data_root; links and outside directories return -1.
int game_profile_index(const char *game_dir, const char *data_root);
void game_profile_activate(const char *game_dir, const char *data_root);
int game_profile_active(void);
#ifdef __cplusplus
}
#endif
