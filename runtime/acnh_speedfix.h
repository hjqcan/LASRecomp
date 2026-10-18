#pragma once
#include "title_profile.h"

#ifdef __cplusplus
extern "C" {
#endif
// A prepared variant retains its release Build IDs but has a different native
// library identity. The original installation and its compilation cache stay intact.
const TitleProfile *acnh60_profile(const TitleProfile *base, const char *root, char *why, size_t cap);
const char *acnh60_generator_identity(const TitleProfile *profile);
bool acnh60_validate(const char *root, char *why, size_t cap);
bool acnh60_boot(const char *root, char *why, size_t cap);
bool acnh60_active(void);
bool acnh60_romfs_override(const char *relative, char *out, size_t cap);
void acnh60_clear(void);
bool acnh60_base_path(const char *variant, char *out, size_t cap);
#ifdef __cplusplus
}
#endif
