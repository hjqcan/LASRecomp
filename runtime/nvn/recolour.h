// Live cosmetic recolours by draw (Cosmetics tab): textures of the game's models are recognised by content (hashes of
// the user's own game files, read once), and a draw sampling one gets an HSV change of its colour in the fragment
// shader (the per-draw tint, mode "recolour": runtime/nvn/maxwell/translate.c). The shading stays (only hue /
// saturation / value move, inside a hue range). Nothing is re-uploaded: a setting change shows on the next frame.
// Shared by the Metal and Vulkan backends; each recolouring option is one RecolourOption (runtime/nvn/recolour_*.c).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "nvn.h"

// texpatch_identify ids from here up are recolour textures (runtime/nvn/texpatch.c; never edited on upload)
#define TEXPATCH_RECOLOUR_BASE 4096

typedef struct {
    const char *file;               // romfs path (e.g. "region_common/actor/Playerlink.bfres"); a BNTX inside
    int (*classify)(int source, const char *texture);  // class (> 0) a texture of it gets, 0: none
} RecolourSource;

typedef struct {
    const char *name;
    const RecolourSource *sources;
    int nsources;
    int cls_lo, cls_hi;             // the classes it hands out (inclusive)
    // The tint for a draw of class `cls`: false = left as is. Called per draw (keep it cheap).
    bool (*tint)(int cls, float tint[8]);
} RecolourOption;

// Which recolour texture `t` is (its index), or -1. total = the texture's size in memory.
int recolour_identify(const NvnTexture *t, uint64_t total);
// The class of texpatch id `id` (0: not a recolour texture)
int recolour_class_of(int id);
// Of two classes a draw's textures have (0: none), the one to tint with: one whose option is on, and of two on the
// higher class (e.g. a ground draw blending grass and sand maps: sand's, while the Sand row is set)
int recolour_pick(int a, int b);
// The per-draw tint of class `cls` (left untouched when its option is off)
void recolour_tint(int cls, float tint[8]);
// Packs a recolour tint: hue moved by `shift` (turns), saturation / value scaled, for colours whose hue lies within
// `width` (turns; >= 0.5: any) of `centre` and whose saturation is at least `min_sat` (all in the shader's HSV of
// the gamma-encoded colour). spread: their hues' distance from `centre` is scaled by it (1: a plain shift; 0: all
// become centre + shift).
void recolour_pack(float tint[8], float shift, float sat, float val, float centre, float width, float min_sat,
                   float spread);
// Hue (turns) of an HSV hue in degrees
static inline float recolour_deg(float d) { return d / 360.0f; }
