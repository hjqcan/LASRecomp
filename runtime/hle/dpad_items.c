// D-pad items (Controls tab, D-pad = Items: g_settings.dpad_mode DPAD_ITEMS). Each d-pad direction can hold an item,
// used with one press like the X / Y buttons.
//   Assigning: on the pause menu's Items page, a direction puts the highlighted item there (as X / Y do; the same
//   direction again takes it off; an item sits on one direction at most). The d-pad doesn't move the Items page's
//   cursor then: the stick does.
//   Using: the game's own item code. Each Items page item has a "use it now?" check (main+0xd50e60 .. 0xd52fb0: Y
//   newly pressed with the item on Y, PlayerStatus +0xa2, or X with it on X, +0xa3) that runs only while Link may use
//   items. tools/patches.py EQUIP_Y_*_SITES make a newly pressed direction count there too, for its own item: one press
//   uses it, and with Y or X pressed at the same time both items are used (bomb arrows). X / Y keep their items.
//   An item no longer owned (a bottle traded, a seed's take-back) leaves its direction.
// Saved with each save slot: savedataN.bin -> savedataN.bin.dpad, written with the save and matched to it by the
// save's body CRC (as the weather cycle, runtime/nvn/weather_cycle.c); a save without one starts empty.
// The HUD's small cross: runtime/ui/dpad_overlay.mm. Test: LARECOMP_DPAD_ITEMS=<frame>:<up>,<down>,<left>,<right> (item
// slots in hex, - for none) sets the directions of the save in play at that presented frame.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../game_addresses.h"
#include "../hle.h"
#include "../menu_sfx.h"
#include "../settings.h"

extern int game_in_play(void);              // runtime/hle/warp.c
extern int inventory_items_cursor(void);    // runtime/ui/inventory_overlay.mm (-1: not on the Items page)
extern uint64_t inventory_items_obj(void);  // (its page object)
extern uint64_t renderer_present_count(void);

#define BODY_CRC 0x5EC4
#define MAGIC 0x31504444u  // "DDP1"
#define NONE 0xff
enum { BTN_Y = 3, BTN_LEFT = 12 };  // (runtime/hle/input.c; directions LEFT UP RIGHT DOWN from bit 12)
#define DIRS (0xfull << BTN_LEFT)

typedef struct {
    uint32_t magic, crc;  // (the save's body CRC this goes with)
    uint8_t slot[4];      // item slot on Left, Up, Right, Down (bit order); NONE: empty
} Dpad;

static Dpad g_d = {MAGIC, 0, {NONE, NONE, NONE, NONE}};
static uint32_t g_live_crc;
static int g_was_in_play;
static struct { char path[1024]; Dpad d; } g_slots[8];  // (what each slot's file held, as read)
static int g_nslots;

static bool g_y_new;     // the item checks: Y newly pressed
static uint32_t g_dirs;  // the d-pad directions newly pressed (bits Left Up Right Down), as held: not mirrored

RT_ON_RESET(dpad_items_reset) {
    memset(g_d.slot, NONE, sizeof g_d.slot);
    g_live_crc = 0, g_was_in_play = 0, g_nslots = 0, g_y_new = false, g_dirs = 0;
}

static uint64_t player_status(void) { return LD64(GA(PLAYER_STATUS_PTR)); }
static bool owned(uint64_t ps, int slot) { return slot >= 0 && slot < 16 && (LD32(ps + 0xa8) >> slot) & 1; }

// the item on direction d (0 Left, 1 Up, 2 Right, 3 Down), -1: none (the HUD cross)
int dpad_items_slot(int d) {
    if (g_settings.dpad_mode != DPAD_ITEMS || d < 0 || d > 3 || !game_in_play()) return -1;
    return g_d.slot[d] == NONE ? -1 : g_d.slot[d];
}

static void test_assign(void) {
    static int done;
    const char *e = ENV("LARECOMP_DPAD_ITEMS");
    if (done || !e || !game_in_play() || renderer_present_count() < strtoull(e, NULL, 10)) return;
    done = 1;
    static const int order[4] = {1, 3, 0, 2};  // (given as up, down, left, right)
    char buf[64], *save = NULL;
    snprintf(buf, sizeof buf, "%s", strchr(e, ':') ? strchr(e, ':') + 1 : e);
    char *tok = strtok_r(buf, ",", &save);
    for (int i = 0; i < 4 && tok; i++, tok = strtok_r(NULL, ",", &save))
        g_d.slot[order[i]] = *tok == '-' ? NONE : (uint8_t)strtol(tok, NULL, 16);
    LOG("[dpad] test: left %02x up %02x right %02x down %02x\n", g_d.slot[0], g_d.slot[1], g_d.slot[2], g_d.slot[3]);
}

// a save came into play: its slot's directions, or none
static void adopt(uint32_t crc) {
    g_live_crc = crc;
    for (int i = 0; i < g_nslots; i++)
        if (g_slots[i].d.crc == crc) {
            g_d = g_slots[i].d;
            LOG("[dpad] items from %s: %02x %02x %02x %02x\n", g_slots[i].path, g_d.slot[0], g_d.slot[1], g_d.slot[2], g_d.slot[3]);
            return;
        }
    memset(g_d.slot, NONE, sizeof g_d.slot);
}

// Game thread, each pad read (runtime/hle/input.c fill_state, the overlay menu closed): the buttons for the game
uint64_t dpad_items_input(uint64_t b) {
    int play = game_in_play();
    uint64_t ps = player_status();
    uint32_t crc = play ? *(const uint32_t *)(uintptr_t)(GA(LIVE_SAVE) + BODY_CRC) : 0;
    if (play && crc != g_live_crc) {  // another save in play: a file loaded, a new game, a Load from the pause menu
        bool slot = false;            // (mid-play, only a slot's: else the game is saving)
        for (int i = 0; i < g_nslots; i++) slot |= g_slots[i].d.crc == crc;
        if (!g_was_in_play || slot) adopt(crc);
        else g_live_crc = crc;
    }
    g_was_in_play = play;
    static uint64_t prev;
    uint64_t press = b & ~prev & DIRS;
    prev = b;
    if (g_settings.dpad_mode != DPAD_ITEMS || !play || !ps) return b;
    test_assign();
    for (int d = 0; d < 4; d++)  // (an item no longer owned leaves its direction)
        if (g_d.slot[d] != NONE && !owned(ps, g_d.slot[d])) {
            LOG("[dpad] item %02x not owned: off direction %d\n", g_d.slot[d], d);
            g_d.slot[d] = NONE;
        }
    int cur = inventory_items_cursor();
    if (cur < 0) return b;  // (in play the game sees the d-pad as ever: its item checks take it)
    // the Items page: a direction puts the highlighted item there
    uint64_t obj = inventory_items_obj();
    int item = obj ? LD8(obj + (uint64_t)cur * 0x280 + 0x598) : NONE;
    for (int d = 0; d < 4; d++) {
        if (!(press >> (BTN_LEFT + d) & 1)) continue;
        if (!owned(ps, item)) { menu_sfx_play(MENU_SFX_BEEP); break; }  // (an empty cell)
        if (g_d.slot[d] == item) {
            g_d.slot[d] = NONE;
            menu_sfx_play(MENU_SFX_CANCEL);
        } else {
            for (int o = 0; o < 4; o++) if (g_d.slot[o] == item) g_d.slot[o] = NONE;
            g_d.slot[d] = (uint8_t)item;
            menu_sfx_play(MENU_SFX_DECIDE);
        }
        LOG("[dpad] items page cell %d (item %02x): direction %d -> %02x\n", cur, item, d, g_d.slot[d]);
        break;
    }
    return b & ~DIRS;  // (the stick moves the cursor)
}

// ---- the item checks (tools/patches.py EQUIP_Y_TEST_SITES / EQUIP_Y_READ_SITES; game thread)
// An item check's Y test: the buttons newly pressed, in the game's own bits (Y 3; the d-pad Left 8, Right 9, Down 10,
// Up 11). Go on to the item on Y?
int rt_equip_y_test(uint32_t pressed) {
    extern bool mirror_input(void);  // (mirror mode: the game got the d-pad's left and right swapped - runtime/hle/input.c)
    g_y_new = pressed >> BTN_Y & 1;
    uint32_t d = 0;
    if (g_settings.dpad_mode == DPAD_ITEMS) {
        bool l = pressed >> 8 & 1, r = pressed >> 9 & 1;
        if (mirror_input()) { bool t = l; l = r, r = t; }
        d = l | (pressed >> 11 & 1) << 1 | r << 2 | (pressed >> 10 & 1) << 3;  // (Left Up Right Down)
    }
    g_dirs = d;
    return g_y_new || d;
}

// Its read of the item on Y, for the check of item `slot`: that item when a direction just pressed holds it, else the
// item on Y if Y was pressed, else none
uint32_t rt_equip_y_read(uint64_t ps, int slot) {
    for (int d = 0; d < 4; d++)
        if (g_dirs >> d & 1 && g_d.slot[d] == slot) {
            LOG("[dpad] direction %d: item %02x\n", d, slot);
            return (uint32_t)slot;
        }
    return g_y_new ? LD8(ps + 0xa2) : NONE;
}

// ---- the save slots (runtime/services/fs.c)
static bool slot_path(const char *path, char *out, size_t n) {  // (the save's own path; a working copy's ".pending" off)
    size_t len = strlen(path);
    bool pending = len > 8 && !strcmp(path + len - 8, ".pending");
    snprintf(out, n, "%.*s.dpad", (int)(len - (pending ? 8 : 0)), path);
    return pending;
}

static int slot_index(const char *p) {
    int i = 0;
    while (i < g_nslots && strcmp(g_slots[i].path, p)) i++;
    if (i == g_nslots && g_nslots < 8) snprintf(g_slots[g_nslots++].path, sizeof g_slots[0].path, "%s", p);
    return i < g_nslots ? i : -1;
}

// a save file was read into guest memory: its directions, kept for when that save comes into play
void dpad_items_save_read(const char *path, uint64_t addr, int64_t off, int64_t size) {
    if (off != 0 || size < BODY_CRC + 4) return;
    char p[1100];
    slot_path(path, p, sizeof p);
    Dpad d;
    FILE *f = fopen(p, "rb");
    bool ok = f && fread(&d, sizeof d, 1, f) == 1 && d.magic == MAGIC &&
              d.crc == *(const uint32_t *)(uintptr_t)(addr + BODY_CRC);  // (else: from an older save of that slot)
    if (f) fclose(f);
    int i = slot_index(p);
    if (i < 0) return;
    if (ok) g_slots[i].d = d;
    else memset(&g_slots[i].d, 0, sizeof g_slots[i].d);  // (crc 0 matches no save)
}

// a save file was written from guest memory: the directions now go with it (none set and no file yet: nothing)
void dpad_items_save_written(const char *path, uint64_t addr, int64_t off, int64_t size) {
    if (off != 0 || size < BODY_CRC + 4) return;
    Dpad d = g_d;
    d.magic = MAGIC;
    d.crc = *(const uint32_t *)(uintptr_t)(addr + BODY_CRC);
    g_live_crc = *(const uint32_t *)(uintptr_t)(GA(LIVE_SAVE) + BODY_CRC);  // (the game writes from a copy)
    char p[1100], tmp[1200];
    slot_path(path, p, sizeof p);
    int i = slot_index(p);
    if (i >= 0) g_slots[i].d = d;
    bool any = false;
    for (int k = 0; k < 4; k++) any |= d.slot[k] != NONE;
    if (!any && access(p, F_OK)) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(&d, sizeof d, 1, f) == 1;
    ok = !fclose(f) && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING);  // (rename() won't replace a file there)
#else
    ok = ok && !rename(tmp, p);
#endif
    if (!ok) { unlink(tmp); LOG("[dpad] could not write %s\n", p); }
}
