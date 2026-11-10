// Seed hash (race mode): five of the game's item icons picked from a seed ID, shown on the title / file select screen
// (race_overlay.mm) and with the seed in the Randomizer tab, so racers can check at a glance that they have the same
// seed. The icons are the game's own UI textures (runtime/nvn/texpatch.c texpatch_ui_rgba); the names stand in when
// one can't be read.
#pragma once
#include <ctype.h>
#include <stdint.h>

#include "../i18n_mark.h"

static const struct { const char *tex, *name; } kSeedHashIcons[32] = {
    {"ItemBomb_00^w", LN("Bomb")},           {"ItemBoomerang_00^w", LN("Boomerang")},  {"ItemBottle_00^w", LN("Bottle")},
    {"ItemBow_00^w", LN("Bow")},             {"ItemFookShot_00^w", LN("Hookshot")},     {"ItemMagicPowder_00^w", LN("Powder")},
    {"ItemMagicRod_00^w", LN("Magic Rod")},  {"ItemOcarina_03^w", LN("Ocarina")},       {"ItemRocsFeather_00^w", LN("Feather")},
    {"ItemShovel_00^w", LN("Shovel")},       {"CollectInst_00^w", LN("Cello")},         {"CollectInst_01^w", LN("Conch")},
    {"CollectInst_02^w", LN("Bell")},        {"CollectInst_03^w", LN("Harp")},          {"CollectInst_04^w", LN("Marimba")},
    {"CollectInst_05^w", LN("Triangle")},    {"CollectInst_06^w", LN("Organ")},         {"CollectInst_07^w", LN("Drum")},
    {"CollectSecretSeashell_00^w", LN("Seashell")}, {"EqPegasusBoots_00^q", LN("Boots")}, {"EqPowerBracelet_00^q", LN("Bracelet")},
    {"EqShield_00^q", LN("Shield")},         {"EqSword_01^q", LN("Sword")},             {"Eqflippers_00^q", LN("Flippers")},
    {"TrdYoshiDoll_00^q", LN("Doll")},       {"TrdRibbon_00^q", LN("Ribbon")},          {"TrdBananas_00^q", LN("Bananas")},
    {"TrdPineapple_00^q", LN("Pineapple")},  {"TrdHibiscus_00^q", LN("Hibiscus")},      {"TrdBroom_00^q", LN("Broom")},
    {"TrdNecklace_00^q", LN("Necklace")},    {"TrdLens_00^q", LN("Lens")},
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
