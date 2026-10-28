// Cosmetics: the Fence, Wall and House colours (Cosmetics tab, shared palette), live. They recolour the overworld's
// wooden fences, its cliff walls and the village houses, keeping the shading, texture detail and highlights
// (recolour.c). On Default nothing is tinted: the game's own look.
//   Fences: the wooden posts and rails (WoodFence / WoodFenceSingle: Mabe Village, Animal Village, the prairie). Not
//   the graveyard's stone fence (its own material).
//   Walls: the brick cliff walls and ledges everywhere on the overworld (f_wall_a, the bombable cracked ones the same),
//   Tal Tal Heights' (Tarutaru_wall*), the desert-side ledges (f_wall_y) and the yellow brick ones (f_wall_kr). Not
//   the rocks and the mountain (f_taru_iwa, f_enkei_iwa), Kanalet Castle, or the dungeons' walls (each dungeon has its
//   own textures and colours).
//   Houses: each house is one model with one colour map (its roof, walls, door and trim in one atlas), so each house
//   has its own hue range: the roof's. Painted roofs (Marin and Tarin's blue, Madam MeowMeow's pink, the
//   Quadruplets' teal, Ulrira's purple, Sale's blue planks, ...) move alone: stone walls (grey) and wood keep theirs.
//   Thatch roofs (the Town Tool Shop, the Trendy Game, Animal Village, Mr. Write's) share their hue with the house's
//   wood, so those houses take the colour as a whole (the stone and the greys stay). Not the Library and the Dream
//   Shrine (stone), the phone booth trees (Leaves), the houses' leaf decals (Leaves) or Dampe's shack (many colours).
#include <string.h>

#include "../settings.h"
#include "recolour.h"

enum { B_FENCE = 36, B_WALL, B_HOUSE = 40, B_HOUSE_LAST = 63 };

static bool ends_alb(const char *t) { size_t n = strlen(t); return n > 4 && !strcmp(t + n - 4, "_alb"); }
static bool pre(const char *t, const char *p) { return !strncmp(t, p, strlen(p)); }

// The houses (colour map prefix) and their roofs' colour as drawn: typical hue (degrees), saturation, value, and the
// hue range that moves (degrees either side of the typical hue; >= 180: any) above a saturation
typedef struct {
    const char *map;
    float h, s, v, width, min_sat;
} House;

static const House k_houses[] = {
    {"MI_BdMarinTarin_01_1_", 205, 0.8f, 0.6f, 35, 0.25f},  // Marin and Tarin's: blue tiles
    {"MI_BdMadam_01_", 350, 0.6f, 0.62f, 30, 0.25f},         // Madam MeowMeow's: pink tiles
    {"MI_BdQuaduplet_01_", 160, 0.65f, 0.65f, 30, 0.25f},     // the Quadruplets': teal tiles
    {"MI_Bd_Ulrira_01_", 274, 0.35f, 0.5f, 35, 0.15f},       // Ulrira's: purple tiles
    {"MI_BdMagic_01_", 250, 0.3f, 0.5f, 35, 0.15f},           // the Witch's hut
    {"MI_BdTracy_01_", 300, 0.35f, 0.5f, 50, 0.15f},          // Crazy Tracy's
    {"MI_BdGhost_01_", 260, 0.35f, 0.5f, 35, 0.15f},           // the seaside house (ghost)
    {"MI_BdGhost_02_", 260, 0.35f, 0.5f, 35, 0.15f},
    {"MI_BdBanana_01_1_", 200, 0.7f, 0.65f, 30, 0.25f},       // Sale's house O' bananas: blue planks
    {"MI_BdSchule_01_", 350, 0.6f, 0.6f, 30, 0.2f},          // Schule Donavitch's: red stripes
    {"MI_BdRichard_01_", 20, 0.6f, 0.4f, 25, 0.25f},          // Richard's villa: brick
    {"MI_BdSeashell_01_", 15, 0.4f, 0.6f, 25, 0.2f},          // the Seashell Mansion
    // thatch and wood: the whole house moves (the greys stay)
    {"MI_BdShop_01_", 45, 0.6f, 0.6f, 40, 0.2f},            // the Town Tool Shop: thatch, striped awning
    {"MI_BdCrane_01_", 45, 0.6f, 0.6f, 40, 0.2f},           // the Trendy Game
    {"MI_BdDrWright_01_", 45, 0.6f, 0.6f, 40, 0.2f},        // Mr. Write's
    {"MI_BdBear_01_", 35, 0.5f, 0.58f, 40, 0.2f},             // Animal Village: Chef Bear's
    {"MI_BdBear_03_", 35, 0.5f, 0.58f, 40, 0.2f},
    {"MI_BdChristine_01_", 35, 0.5f, 0.58f, 40, 0.2f},        // Christine's
    {"MI_BdRabbit_01_", 35, 0.5f, 0.58f, 40, 0.2f},           // the rabbits'
    {"MI_BdZora_01_", 35, 0.5f, 0.58f, 40, 0.2f},             // Hidden Zora's
    {"MI_BdFishing_01_", 35, 0.5f, 0.58f, 40, 0.2f},          // the fishing pond's
    {"MI_BdRapidsRide_01_", 35, 0.5f, 0.58f, 40, 0.2f},       // the Rapids Ride hut
};
enum { N_HOUSES = (int)(sizeof k_houses / sizeof *k_houses) };
_Static_assert(B_HOUSE + N_HOUSES - 1 <= B_HOUSE_LAST, "too many houses for their classes");

static int field(int source, const char *t) {  // Field.bntx (the overworld's textures) and the field actors
    (void)source;
    if (!ends_alb(t)) return 0;
    if (pre(t, "MI_WoodFence_01_") || pre(t, "MI_WoodFenceSingle_01_")) return B_FENCE;
    if (pre(t, "MI_f_wall_a_01") || pre(t, "MI_f_wall_y_0") || pre(t, "MI_f_wall_kr_01") ||
        pre(t, "MI_FCrackedWall_01_") || pre(t, "MI_Tarutaru_wall"))
        return B_WALL;
    for (int i = 0; i < N_HOUSES; i++)
        if (pre(t, k_houses[i].map)) return B_HOUSE + i;
    return 0;
}

static const RecolourSource k_sources[] = {
    {"region_common/map/Field.bntx", field},
    {"region_common/actor/ObjFieldCrackedWall.bfres", field},
    {"region_common/actor/ObjBdGhost.bfres", field},
};

// Fences and walls are all browns (hues 10-45: wood, brick, the pale caps): every hue moves, their variety kept at
// 35%; only the near-greys (saturation under ~0.08) stay.
static bool tint(int cls, float t[8]) {
    uint32_t rgb;
    float sh, sf, vf;
    if (cls == B_FENCE) {  // (typical as drawn, in the sun: 37, 0.55, 0.72)
        if (!col_target(&g_settings.fences, COL_SLOT_FENCES, &rgb)) return false;
        col_mapping(37 / 360.0f, 0.55f, 0.72f, rgb, &sh, &sf, &vf);
        recolour_pack(t, sh, sf, vf, recolour_deg(37), 0.5f, 0.08f, 0.35f);
        return true;
    }
    if (cls == B_WALL) {  // (typical as drawn, in the sun: 24, 0.65, 0.72)
        if (!col_target(&g_settings.walls, COL_SLOT_WALLS, &rgb)) return false;
        col_mapping(24 / 360.0f, 0.65f, 0.72f, rgb, &sh, &sf, &vf);
        recolour_pack(t, sh, sf, vf, recolour_deg(24), 0.5f, 0.08f, 0.35f);
        return true;
    }
    if (cls < B_HOUSE || cls >= B_HOUSE + N_HOUSES) return false;
    if (!col_target(&g_settings.houses, COL_SLOT_HOUSES, &rgb)) return false;
    const House *h = &k_houses[cls - B_HOUSE];
    col_mapping(h->h / 360.0f, h->s, h->v, rgb, &sh, &sf, &vf);
    recolour_pack(t, sh, sf, vf, recolour_deg(h->h), h->width >= 180 ? 0.5f : h->width / 360.0f, h->min_sat, 0.5f);
    return true;
}

const RecolourOption g_recolour_buildings = {"fences / walls / houses", k_sources,
                                             (int)(sizeof k_sources / sizeof *k_sources), B_FENCE, B_HOUSE_LAST, tint};
