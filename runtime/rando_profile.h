#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gameversion.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t offset, word; } RandoCodePatch;
typedef struct { uint32_t offset, size; unsigned char bytes[32]; } RandoDataPatch;
typedef struct {
    int code_count, data_count;
    RandoCodePatch code[64];
    RandoDataPatch data[2];
} RandoPatchSet;
bool rando_seed_name_valid(const char *name);
bool rando_seed_compatible(const char *directory, GameLayout version);
const char *rando_patch_filename(GameLayout version);
// Parse and validate the entire file before any guest memory is changed.
bool rando_patch_read(const char *path, GameLayout version, RandoPatchSet *out, char *why, size_t cap);
#ifdef __cplusplus
}
#endif
