// The Cosmetics tab's colour rows (tunic, hearts, hair, skin, shield, leaves, grass, sand, water) share one palette:
// Default (the game's own), twelve named colours, Random and Custom (any colour, from a colour picker; saved as
// #RRGGBB). Each row recolours its thing so that its typical colour becomes the chosen one, keeping the shading
// (col_mapping).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { COL_DEFAULT = 0, COL_RANDOM = 13, COL_CUSTOM = 14, N_COL = 15 };  // (1..12: the named colours)

typedef struct CosColour {
    int pick;      // COL_DEFAULT, a named colour, COL_RANDOM or COL_CUSTOM
    uint32_t rgb;  // the Custom colour, 0xRRGGBB
} CosColour;

// Which row (Random picks a colour per row, again whenever the row is set to Random)
enum { COL_SLOT_TUNIC, COL_SLOT_HEARTS, COL_SLOT_HAIR, COL_SLOT_SKIN, COL_SLOT_SHIELD, COL_SLOT_LEAVES, COL_SLOT_GRASS,
       COL_SLOT_SAND, COL_SLOT_WATER, COL_SLOTS };  // (new rows at the end: a seed's own colours stay per slot)

extern const char *const g_col_names[N_COL];
extern const uint32_t g_col_rgb[N_COL];  // the named colours' targets (0 for Default / Random / Custom)

// larecomp.ini value -> c: a palette name, #RRGGBB (Custom), or an older version's name (Yellow, Sky, Lime, Blond,
// ...: the nearest palette colour); anything else: Default.
void col_parse(CosColour *c, const char *val);
// c -> its larecomp.ini value (name, or #RRGGBB for Custom)
void col_format(const CosColour *c, char *out, size_t n);
// The randomizer seed's own cosmetics (seed option "Random cosmetics", runtime/cosmetics_seed.c): the colour row `slot`
// takes while it is on Default: a palette colour, or COL_CUSTOM with *rgb (0: none - vanilla, or a seed without the
// option). NULL until set.
extern int (*g_col_seed_pick)(int slot, uint32_t *rgb);
// The seed's own colour for row `slot` while it is on Default (0xRRGGBB); false: none
bool col_seed_rgb(const CosColour *c, int slot, uint32_t *rgb);
// The palette colour row `slot` shows: the player's choice, or the seed's while the row is on Default
int col_pick(const CosColour *c, int slot);
// The colour row `slot` should show now (0xRRGGBB); false: Default (the game's own look).
bool col_target(const CosColour *c, int slot, uint32_t *rgb);
// HSV (hue in turns, saturation, value: 0..1) of 0xRRGGBB
void col_hsv(uint32_t rgb, float *h, float *s, float *v);
// The HSV change that takes a thing whose typical colour is (src_h, src_s, src_v) to `dst` (0xRRGGBB): hue shift
// (turns), saturation and value factors. A grey / white / black target keeps the hue and takes the saturation away.
void col_mapping(float src_h, float src_s, float src_v, uint32_t dst, float *shift, float *sat, float *val);
