// Seed hash (race mode): five of the game's item icons picked from a seed ID, shown on the title / file select screen
// (race_overlay.mm) and with the seed in the Randomizer tab, so racers can check at a glance that they have the same
// seed. The icons are the game's own UI textures (runtime/nvn/texpatch.c texpatch_ui_rgba); the names stand in when
// one can't be read.
#pragma once
#include <ctype.h>
#include <stdint.h>

static const struct { const char *tex, *name; } kSeedHashIcons[32] = {
    {"ItemBomb_00^w", "Bomb"},           {"ItemBoomerang_00^w", "Boomerang"},  {"ItemBottle_00^w", "Bottle"},
    {"ItemBow_00^w", "Bow"},             {"ItemFookShot_00^w", "Hookshot"},     {"ItemMagicPowder_00^w", "Powder"},
    {"ItemMagicRod_00^w", "Fire Rod"},   {"ItemOcarina_03^w", "Ocarina"},       {"ItemRocsFeather_00^w", "Feather"},
    {"ItemShovel_00^w", "Shovel"},       {"CollectInst_00^w", "Cello"},         {"CollectInst_01^w", "Conch"},
    {"CollectInst_02^w", "Bell"},        {"CollectInst_03^w", "Harp"},          {"CollectInst_04^w", "Marimba"},
    {"CollectInst_05^w", "Triangle"},    {"CollectInst_06^w", "Organ"},         {"CollectInst_07^w", "Drum"},
    {"CollectSecretSeashell_00^w", "Seashell"}, {"EqPegasusBoots_00^q", "Boots"}, {"EqPowerBracelet_00^q", "Bracelet"},
    {"EqShield_00^q", "Shield"},         {"EqSword_01^q", "Sword"},             {"Eqflippers_00^q", "Flippers"},
    {"TrdYoshiDoll_00^q", "Doll"},       {"TrdRibbon_00^q", "Ribbon"},          {"TrdBananas_00^q", "Bananas"},
    {"TrdPineapple_00^q", "Pineapple"},  {"TrdHibiscus_00^q", "Hibiscus"},      {"TrdBroom_00^q", "Broom"},
    {"TrdNecklace_00^q", "Necklace"},    {"TrdLens_00^q", "Lens"},
};

// The five icons (indices into kSeedHashIcons) of a seed ID: FNV-1a over its characters (dashes, spaces and case
// don't count), 5 bits each
static inline void seed_hash(const char *id, int out[5]) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char *p = id; *p; p++) {
        if (*p == '-' || *p == ' ' || *p == '\n' || *p == '\r') continue;
        h ^= (uint8_t)toupper((unsigned char)*p);
        h *= 0x100000001b3ull;
    }
    h ^= h >> 29;
    for (int i = 0; i < 5; i++) out[i] = (int)(h >> (5 * i) & 31);
}
