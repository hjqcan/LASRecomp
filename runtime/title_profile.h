#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define TITLE_MAX_MODULES 8

typedef struct {
    const char *name, *build_id;
    uint64_t base;
    bool hle;
} TitleModule;
typedef struct {
    const char *key, *name, *version, *generator_profile, *status;
    uint64_t program_id;
    unsigned adapter_revision, module_count;
    bool installable;
    TitleModule modules[TITLE_MAX_MODULES];
    unsigned init_order[TITLE_MAX_MODULES];
    const char *code_identity;
    const char *romfs_roots[8];
    unsigned romfs_root_count, minimum_files;
    bool image_satisfies_count;
    unsigned nvn_minor, heap_scale;
    bool las_extensions;
} TitleProfile;

#ifdef __cplusplus
extern "C" {
#endif
const TitleProfile *title_profile_current(void);
unsigned title_profile_count(void);
const TitleProfile *title_profile_at(unsigned index);
bool title_is_las(void);
bool title_build_id(const char *path, char out[65]);
bool title_module_matches(const TitleModule *module, const unsigned char id[32]);
const TitleProfile *title_profile_find(const char *main_path);
// Validation has no effect on the active title; selection is explicit.
const TitleProfile *title_profile_validate(const char *game_dir, char *why, size_t why_size);
bool title_profile_select(const char *game_dir, char *why, size_t why_size);
void title_profile_preview(const char *key, const char *version); // launcher only; boot still validates all modules
// Validate adapter-declared data rules; count < 0 counts files only when needed.
bool title_content_validate(const TitleProfile *profile, const char *romfs, bool has_image,
                            int *count, char *why, size_t why_size);
bool title_content_root(const TitleProfile *profile, const char *romfs);
bool title_profile_write_entry(const TitleProfile *profile, const char *path, char *why, size_t why_size);
#ifdef __cplusplus
}
#endif
