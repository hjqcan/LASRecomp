#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct GameInstall GameInstall;
typedef bool (*GameInstallValidator)(const char *candidate, char *why, size_t size);
// The stable slot keeps legacy data until a complete candidate is published.
bool game_install_resolve(const char *slot, char *out, size_t size, char *why, size_t why_size);
bool game_install_pending(const char *slot);
bool game_install_pending_path(const char *slot, char *out, size_t size);
// Holds the installation's OS lock through import, compilation and publication.
GameInstall *game_install_begin(const char *slot, bool fresh, char *why, size_t size);
const char *game_install_candidate(GameInstall *install);
bool game_install_clone(GameInstall *install, const char *source, char *why, size_t size);
// Resume an interrupted clone before validation/build. Completed clones and
// imported candidates are left alone, including their generated build cache.
bool game_install_resume_clone(GameInstall *install, char *why, size_t size);
bool game_install_publish(GameInstall *install, const char *launch_id, GameInstallValidator validate, char *why, size_t size);
void game_install_close(GameInstall *install);
// Explicit removal of owned generations only. Legacy data and saves are handled separately.
bool game_install_clear(const char *slot, char *why, size_t size);
#ifdef __cplusplus
}
#endif
