#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { GAME_UNSUPPORTED = 0, GAME_100, GAME_110 } GameLayout;
// The first 20 NSO Build ID bytes, rendered as 40 lowercase hexadecimal digits.
bool game_build_id(const char *main_path, char out[41]);
GameLayout game_version_file(const char *main_path);
GameLayout game_version_id(const char *build_id);
const char *game_version_name(GameLayout version);
void game_layout_select(const char *main_path);
GameLayout game_version_current(void);
uint64_t game_main_address(uint64_t canonical);
uint64_t game_canonical_address(uint64_t actual);

#ifdef __cplusplus
}
#endif
