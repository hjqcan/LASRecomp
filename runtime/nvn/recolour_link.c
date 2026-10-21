// Cosmetics: Link's hair and shield colours (Cosmetics tab "Hair color" / "Shield color"), live: draws sampling his
// hair's colour map (Playerlink.bfres MI_hair_alb: hair only) or a shield's (MI_shieldA_alb, the first shield: only
// its blue face moves, the red emblem and the metal stay; MI_shieldB_alb, the Mirror Shield: its frame - the
// face is a mirror) get the chosen colour's hue, saturation and value, keeping the shading (recolour.c).
#include <string.h>

#include "../settings.h"
#include "recolour.h"

enum { C_HAIR = 1, C_SHIELD_A, C_SHIELD_B };

static int classify(int source, const char *t) {
    (void)source;
    if (!strcmp(t, "MI_hair_alb")) return C_HAIR;
    if (!strcmp(t, "MI_shieldA_alb")) return C_SHIELD_A;
    if (!strcmp(t, "MI_shieldB_alb")) return C_SHIELD_B;
    return 0;
}

static const RecolourSource k_sources[] = {{"region_common/actor/Playerlink.bfres", classify}};

// Each row's chosen colour (shared palette, runtime/cosmetic_colour.h) is a target for the thing's typical colour as
// drawn (hue in degrees, saturation, value): the hair (30, 0.94, 0.74), the first shield's lit blue face (195, 0.7,
// 0.8; its map's blue is 211), the Mirror Shield's beige frame (40, 0.25, 0.8; its face is a mirror and stays one).
static bool tint(int cls, float t[8]) {
    uint32_t rgb;
    float sh, sf, vf;
    if (cls == C_HAIR) {
        if (!col_target(&g_settings.hair, COL_SLOT_HAIR, &rgb)) return false;
        col_mapping(30 / 360.0f, 0.94f, 0.74f, rgb, &sh, &sf, &vf);
        recolour_pack(t, sh, sf, vf, recolour_deg(30), 0.5f, 0, 0.5f);  // (the whole map is hair)
        return true;
    }
    if (!col_target(&g_settings.shield, COL_SLOT_SHIELD, &rgb)) return false;
    if (cls == C_SHIELD_A) {
        col_mapping(195 / 360.0f, 0.7f, 0.8f, rgb, &sh, &sf, &vf);
        recolour_pack(t, sh, sf, vf, recolour_deg(195), 0.15f, 0.25f, 0.6f);  // (the blue face only)
    } else {
        col_mapping(40 / 360.0f, 0.25f, 0.8f, rgb, &sh, &sf, &vf);
        recolour_pack(t, sh, sf, vf, recolour_deg(40), 0.5f, 0, 0);
    }
    return true;
}

const RecolourOption g_recolour_link = {"hair / shield", k_sources, 1, C_HAIR, C_SHIELD_B, tint};
