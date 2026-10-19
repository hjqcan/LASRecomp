#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
// Validate modules and compiled code, then create/update this installation's
// desktop shortcut. Existing unrelated shortcuts are preserved.
bool game_shortcut_create(const char *game_dir, char *output, size_t output_size, char *why, size_t why_size);
#ifdef __cplusplus
}
#endif
