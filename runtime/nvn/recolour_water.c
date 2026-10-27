// Cosmetics: the Water colour (Cosmetics tab, shared palette), live. It recolours the water, keeping its shading,
// ripples, highlights and transparency (recolour.c): the sea and its shore waves, rivers, the Rapids, ponds, the fairy
// fountains, the pools in caves and houses, the dungeons' water (one colour everywhere: it is one water material), and
// what the water throws up - waterfalls, splashes, water columns, ripples, rings and bubbles - as a paler version of it.
// A water draw is told by its maps: the water normal / colour maps (MI_f_water_*: the overworld's, the caves' and
// houses' copies, Bottle Grotto's, the Wind Fish's Egg's), the 2D side-view water (fishing pond, boat, Manbo's cave),
// Catfish's Maw entrance pool and the distortion map every water surface samples (MI_f_indirect_a_01_nml: two
// versions, Field.bntx's and the Color Dungeon's, which the houses' and fountains' water shares). The shore waves,
// waterfalls and splashes are effects (Bg.ptcl / Object.ptcl). Only blues move (a hue window around the water's), so
// foam, the falls' white streaks, splash cores and sparkles stay white and the floor seen through the water keeps its
// colour. On Default nothing is tinted: the game's own look.
#include <string.h>

#include "../settings.h"
#include "recolour.h"

enum { W_WATER = 32, W_WAVE, W_SPRAY, W_MIST };

static bool pre(const char *t, const char *p) { return !strncmp(t, p, strlen(p)); }

static int water(int source, const char *t) {
    (void)source;
    if (pre(t, "MI_f_water_") || pre(t, "MI_f_wave_") || pre(t, "MI_2d_water_") || pre(t, "MI_FB_water_") ||
        pre(t, "MI_CatfishsMawEnter_water_01_") || !strcmp(t, "MI_f_indirect_a_01_nml"))
        return W_WATER;
    return 0;
}

static int effect(int source, const char *t) {  // Bg.ptcl / Object.ptcl (between them, every version of these)
    (void)source;
    if (pre(t, "wave_04") || pre(t, "wave_05") || pre(t, "wave_06")) return W_WAVE;  // the shore waves
    // the waterfalls (Tal Tal's, Angler's), splashes (Link's swim / dive / way in and out, the falls', the rapids'),
    // water columns, ripples, rings and bubbles
    if (pre(t, "anglerswaterfall_") || pre(t, "splash_0") || pre(t, "water_splash_") || pre(t, "modelsplash_") ||
        pre(t, "watercylinder_") || pre(t, "ripple_") || pre(t, "wave_00") || !strcmp(t, "bubble_00") ||
        !strcmp(t, "bubble_04"))
        return W_SPRAY;
    // the waterfalls' falling sheet: smoke_01 particles lit by this normal map (smoke_01 alone is any smoke or dust;
    // the lit kind is also some grey smoke: only a clear blue moves, see tint)
    if (!strcmp(t, "smoke_01_nml")) return W_MIST;
    return 0;
}

// (the dungeons' texture files have the water maps' other versions; the 2D water and Catfish's Maw entrance pool are
// in their own models)
static const RecolourSource k_sources[] = {
    {"region_common/map/Field.bntx", water},
    {"region_common/map/Lv04AnglersTunnel.bntx", water},
    {"region_common/map/Lv05CatfishsMaw.bntx", water},
    {"region_common/map/Lv09WindFishsEgg.bntx", water},
    {"region_common/map/Lv10ClothesDungeon.bntx", water},
    {"region_common/map/CatfishsMawEnter_01A.bfres", water},
    {"region_common/map/FishingPond_01A.bfres", water},
    {"region_common/map/FishingBoat_01A.bfres", water},
    {"region_common/effect/Bg.ptcl", effect},
    {"region_common/effect/Object.ptcl", effect},
};

// The water as drawn is a deep cyan-blue: typical (195, 0.8, 0.7) (Martha's Bay, the dungeons' pools); the shallows
// lean cyan (~175), the deep sea blue (~215). Hues within ~65 degrees of 195 move, their variety kept at half (the
// shallows stay lighter than the deep water); the foam (saturation under ~0.15) stays white.
static bool tint(int cls, float t[8]) {
    uint32_t rgb;
    float sh, sf, vf;
    if (!col_target(&g_settings.water, COL_SLOT_WATER, &rgb)) return false;
    col_mapping(195 / 360.0f, 0.8f, 0.7f, rgb, &sh, &sf, &vf);
    if (cls == W_MIST) recolour_pack(t, sh, sf, vf, recolour_deg(195), 0.18f, 0.12f, 0.5f);
    else if (cls == W_SPRAY)  // (pale blue-white, typical (195, 0.3): the water's change, so it stays a paler version of the
                         // water; a wider window, and their white cores - saturation under ~0.04 - stay white)
        recolour_pack(t, sh, sf, vf, recolour_deg(195), 0.25f, 0.04f, 0.5f);
    else recolour_pack(t, sh, sf, vf, recolour_deg(195), 0.18f, cls == W_WAVE ? 0.3f : 0.15f, 0.5f);
    return true;
}

const RecolourOption g_recolour_water = {"water", k_sources, (int)(sizeof k_sources / sizeof *k_sources), W_WATER,
                                         W_MIST, tint};
