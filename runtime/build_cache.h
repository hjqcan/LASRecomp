#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct BuildCache BuildCache;
// Holds an exclusive OS lock until closed. Inputs are hashed, never identified by timestamps alone.
BuildCache *build_cache_open(const char *game_dir, const char *spec, const char *const *inputs, int n, char *why,
                             size_t cap);
// Generator/input changes retain compiled objects and invalidate only generated source receipts.
bool build_cache_prepare_generation(BuildCache *cache, const char *spec, const char *const *inputs, int n,
                                     char *why, size_t cap);
void build_cache_close(BuildCache *cache);
// Leave the in-progress marker on cancellation/crash, but clear it for a reported build failure.
void build_cache_failed(BuildCache *cache);
bool build_cache_resumable(const char *game_dir);
// Anything Clear would remove (a .gamebuild beyond its lock/owner, or an old .zigcache). Never creates the folder.
bool build_cache_present(const char *game_dir);
// Explicit user cleanup; takes the same lock and keeps the installed library and game data.
bool build_cache_clear(const char *game_dir, char *why, size_t cap);
bool build_cache_generation(BuildCache *cache, const char *module, int *chunks);
bool build_cache_save_generation(BuildCache *cache, const char *module, int chunks, char *why, size_t cap);
// -1: source unavailable, 0: compile needed, 1: validated completed object. Always fills source_hash on success.
int build_cache_object(BuildCache *cache, const char *stem, const char *source, char source_hash[65]);
bool build_cache_save_object(BuildCache *cache, const char *stem, const char *source, const char *source_hash,
                             char *why, size_t cap);
// Publishes <name>.tmp with its .id, preserving the old pair on failure. Success keeps only objects and receipts.
bool build_cache_publish(BuildCache *cache, const char *name, const char *runtime_id, char *why,
                         size_t cap);
#ifdef __cplusplus
}
#endif
