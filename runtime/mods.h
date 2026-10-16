#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define MODS_MAX 64
typedef struct {
    char id[64], name[128], author[128], version[64], description[512], problem[512];
    char conflict_detail[512];
    uint64_t bytes;
    int files, conflicts;
    bool enabled, compatible;
} ModInfo;
typedef struct { char name[64], game_version[16]; int mods; } ModPreset;
int mods_preset_list(const char *data_root, ModPreset *out, int cap, char *why, size_t why_cap);
bool mods_preset_save(const char *data_root, const char *game_root, int profile, const char *version,
                      const char *name, char *why, size_t cap);
bool mods_preset_apply(const char *data_root, const char *game_root, int profile, const char *version,
                       const char *name, char *why, size_t cap);
bool mods_preset_import(const char *data_root, const char *source, char *why, size_t cap);
int mods_list(const char *data_root, const char *game_root, int profile, const char *version,
              ModInfo *out, int cap, char *why, size_t why_cap);
bool mods_import(const char *data_root, const char *game_root, const char *version, const char *source,
                 char *why, size_t cap);
bool mods_enable(const char *data_root, const char *game_root, int profile, const char *version,
                 const char *id, bool enabled, char *why, size_t cap);
bool mods_load_active(const char *data_root, const char *game_root, int profile, const char *version,
                      char *why, size_t cap);
bool mods_romfs_override(const char *relative, char *out, size_t cap);
void mods_clear_active(void);
void mods_set_game_root(const char *root);
void mods_draw(float scale, bool in_game);
#ifdef __cplusplus
}
#endif
