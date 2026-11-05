// Keysanity: dungeon items found anywhere. The game's AddItem (main+0x7e18b0, Items.gsheet itemID ids) adds small
// keys / Nightmare Key / map / compass / beak to the CURRENT dungeon only (outside one it does nothing). Seeds with
// keysanity give them with a dungeon index (upstream LAS dev convention: 0-7 = Tail Cave..Turtle Rock, 9 = Color
// Dungeon): this hook
// (tools/patches.py ADD_ITEM_SITE: the inner add main+0x8d08e0 that AddItem, chests and drops all call) applies those straight to that dungeon's save record.
// index -1 (vanilla behaviour, the item's own dungeon) is left to the game.
//
// Save record (notes/save_format.md): live save + 0xE0 + 0x10 * rec, dungeon n -> rec 2(n-1), Color Dungeon -> 18;
// +0 small keys held, +1 bits: compass 0, map 1, beak 2, Nightmare Key 3.
#include "../game_addresses.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../rando.h"


// item ids as AddItem gets them: Items.gsheet `itemID` (not the row number; rows 41+ differ)
enum { IT_COMPASS = 44, IT_MAP = 45, IT_BEAK = 47, IT_SMALL_KEY = 48, IT_NIGHTMARE_KEY = 49 };

// Pot filler refills (tools/rando/pots.py REFILL_INDEX): bombs / magic powder / arrows only once Link has the bomb
// bag / powder / bow (a refill would otherwise unlock them - "bombs must be found first"); before that a blue rupee.
// Bombs / powder only so with the seed's Bomb bag / Powder bag (LAS shuffle-bombs / shuffle-powder): without it they
// are unlocked from the start (unlocked-bombs) or found like any item, and a refill gives the real thing (#537).
// Inventory bits: live save +0x5568 (notes/save_format.md): bit 2 bombs, bit 3 bow, bit 8 powder.
enum { IT_BOMB = 4, IT_POWDER = 11, IT_ARROW = 60, IT_RUPEE_BLUE = 102, REFILL_INDEX = 80 };
enum { GET_MAGIC_POWDER = 295 };  // (GlobalFlags.gsheet index)

bool flags_ext_get(int idx);  // flags_ext.c
void flags_put(int idx, bool on);

// the running seed's Bomb bag / Powder bag settings (read again after an in-place reboot)
static int g_shuffle_bombs = -1, g_shuffle_powder = -1;
RT_ON_RESET(shuffle_bags_reset) { g_shuffle_bombs = g_shuffle_powder = -1; }

static void read_bag_settings(void) {
    if (g_shuffle_powder >= 0) return;
    g_shuffle_bombs = g_shuffle_powder = 0;
    const char *dir = rando_seed_dir();
    static char buf[1 << 16];
    char p[512];
    FILE *f = dir ? (snprintf(p, sizeof p, "%s/settings.json", dir), fopen(p, "rb")) : NULL;
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        g_shuffle_bombs = strstr(buf, "\"shuffle-bombs\": true") != NULL;
        g_shuffle_powder = strstr(buf, "\"shuffle-powder\": true") != NULL;
    }
}

static int refill(Cpu *c, int item, int count) {
    read_bag_settings();
    uint32_t inv = *(const uint32_t *)(uintptr_t)(GA(LIVE_SAVE) + 0x5568);
    int have = item == IT_BOMB ? !g_shuffle_bombs || (inv & (1u << 2))
             : item == IT_POWDER ? !g_shuffle_powder || (inv & (1u << 8)) : (inv & (1u << 3)) != 0;
    if (!have) {
        c->x[0] = IT_RUPEE_BLUE;
        c->x[1] = 1;
    } else if (item == IT_POWDER && !(inv & (1u << 8))) {
        flags_put(GET_MAGIC_POWDER, true);  // (powder's found flag, as its own item-get sets it: powder from now on)
    }
    c->x[2] = (uint64_t)(int64_t)-1;  // the game's own add from here on
    LOG("[pots] refill item %d x%d (inventory %#x): %s\n", item, count, inv,
        have ? "owned or no bag -> refill" : "not owned -> blue rupee");
    return 0;
}

// Found Magic Powder (LAS shuffle-powder): LAS lets powder in only once the event flag GetMagicPowder is set - a
// powder capacity upgrade then adds 40 powder (chests, seashell rewards), the blue Skeletal Guards sell it. But every
// item-get sequence for the key MagicPowder sets that flag too (Items.gsheet gettingFlag), whatever it shows: a powder
// refill's "nowhere to put it" message and the Bomb Bag's (held up as the powder sack) set it without the bag, and
// the next capacity upgrade handed out real powder. In these seeds the flag stays clear while the powder bit
// (live save +0x5568 bit 8, set by the bag's own add) is - also mending saves where it was set that way.
void rt_powder_flag_tick(void) {
    read_bag_settings();
    if (!g_shuffle_powder || !flags_ext_get(GET_MAGIC_POWDER)) return;
    if (*(const uint32_t *)(uintptr_t)(GA(LIVE_SAVE) + 0x5568) & (1u << 8)) return;  // (the Powder Bag is found)
    flags_put(GET_MAGIC_POWDER, false);
    LOG("[pots] GetMagicPowder set without the Powder Bag: cleared\n");
}

// x0 item, x1 count, x2 index: returns 1 when handled (AddItem then returns at once)
int rt_add_item(Cpu *c) {
    extern void rt_silent_add(Cpu *);  // silent_pickups.c: a silent pickup's index back to the real one (+ its fly-in)
    rt_silent_add(c);
    int item = (int)(int32_t)c->x[0], count = (int)(int32_t)c->x[1], index = (int)(int32_t)c->x[2];
    extern int triforce_add_item(int item, int index);  // triforce.c: a Triforce piece (Triforce hunt)
    if (triforce_add_item(item, index)) return 1;
    extern int rt_shop_add_item(Cpu *);  // shopsanity.c: a stolen shopsanity good
    if (rt_shop_add_item(c)) return 1;
    if (index == REFILL_INDEX && (item == IT_BOMB || item == IT_POWDER || item == IT_ARROW)) return refill(c, item, count);
    extern int rt_pet_item(int n);  // pets.c: companions found as items (tools/rando/pets.py PET_INDEX 90)
    if (index >= 90 && index < 93) return rt_pet_item(index - 90);
    if (index < 0 || index > 9 || index == 8) return 0;
    int rec = index == 9 ? 18 : 2 * index;
    uint8_t *r = (uint8_t *)(uintptr_t)(GA(LIVE_SAVE) + 0xE0 + 0x10 * rec);
    switch (item) {
    case IT_SMALL_KEY: r[0] = (uint8_t)(r[0] + (count > 0 ? count : 1)); break;
    case IT_COMPASS: r[1] |= 1; break;
    case IT_MAP: r[1] |= 2; break;
    case IT_BEAK: r[1] |= 4; break;
    case IT_NIGHTMARE_KEY: r[1] |= 8; break;
    default: return 0;
    }
    // (the get-item message names the dungeon: seed text, tools/rando/keysanity_text.py)
    LOG("[keysanity] item %d x%d -> dungeon %d (record %d: keys %d bits %#x)\n", item, count, index, rec, r[0], r[1]);
    return 1;
}
