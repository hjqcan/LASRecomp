// Triforce hunt (randomizer option "Triforce hunt", tools/rando/triforce.py): the seed's goal is N Triforce pieces.
//
// rando/<seed>/triforce.tsv: needed / placed / item <Items.gsheet itemID> / bits <first flag> / remain <first flag> /
// done <flag>, then 'spot' / 'zone' lines (where a piece's model stands in the world).
//   count     each placed piece has its own index; the AddItem hook (keysanity.c rt_add_item -> triforce_add_item) sets
//             GlobalFlags bit `bits + index` - the game's own save keeps it, a piece found twice counts once.
//   messages  the flags the piece's get message reads (Item.bfevfl TriforcePiece): TriforceRemain0..5 = pieces still
//             needed (binary), TriforceDone = the Triforce is complete (its message, the white fade and the ending).
//             The item-get chains give the item before the hold-up, so they're up to date when the message is picked;
//             triforce_tick keeps them in step every frame too (a loaded save, the save editor).
//   look      the Piece of Power's model (texpatch: its colour map) is drawn gold while a piece is received, while Link
//             is in the room / screen where a piece stands (and in the shop / Trendy Game when one is a good there);
//             other Pieces of Power keep their own colours.
#include "../game_addresses.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../rando.h"

bool flags_ext_get(int idx);
void flags_put(int idx, bool on);
extern uint64_t renderer_present_count(void);

typedef struct { char level[32]; float x, y, z; } Spot;  // x NaN: the whole level (shop shelf, crane floor)
static int g_needed = -1, g_placed, g_item = -1, g_bits = -1, g_remain = -1, g_done = -1;
static Spot g_spots[96];
static int g_nspots;
static uint64_t g_got_frame;  // presented-frame count when a piece was last received (0 = none yet)
static volatile int g_link_c = -999, g_link_r = -999;  // Link's room / screen cell (15 x 12 units), game thread

static void load(void) {
    g_needed = g_placed = g_nspots = 0;
    g_item = g_bits = g_remain = g_done = -1;
    const char *dir = rando_seed_dir();
    if (!dir) return;
    char p[1024], line[256];
    snprintf(p, sizeof p, "%s/triforce.tsv", dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char k[16];
        int v;
        Spot *sp = &g_spots[g_nspots];
        if (!strncmp(line, "spot\t", 5)) {
            if (g_nspots < 96 && sscanf(line + 5, "%31[^\t]\t%f\t%f\t%f", sp->level, &sp->x, &sp->y, &sp->z) == 4) g_nspots++;
        } else if (!strncmp(line, "zone\t", 5)) {
            if (g_nspots < 96 && sscanf(line + 5, "%31[^\t\n]", sp->level) == 1) { sp->x = NAN; g_nspots++; }
        } else if (sscanf(line, "%15s %d", k, &v) == 2) {
            if (!strcmp(k, "needed")) g_needed = v;
            else if (!strcmp(k, "placed")) g_placed = v;
            else if (!strcmp(k, "item")) g_item = v;
            else if (!strcmp(k, "bits")) g_bits = v;
            else if (!strcmp(k, "remain")) g_remain = v;
            else if (!strcmp(k, "done")) g_done = v;
        }
    }
    fclose(f);
    if (g_item < 0 || g_bits < 0 || g_remain < 0 || g_done < 0) g_needed = 0;
    LOG("[triforce] %d pieces needed of %d placed (item %d, flags %d+)\n", g_needed, g_placed, g_item, g_bits);
}

RT_ON_RESET(triforce_reset) { g_needed = -1; g_got_frame = 0; }  // (seed switch: reload rando/<seed>/triforce.tsv)

static bool on(void) {
    if (g_needed < 0) load();
    return g_needed > 0;
}

static int count(void) {
    int n = 0;
    for (int i = 0; i < g_placed; i++) n += flags_ext_get(g_bits + i);
    return n;
}

// the message flags: pieces still needed (binary), the Triforce complete
static void sync_flags(void) {
    int have = count(), left = g_needed - have;
    if (left < 0) left = 0;
    for (int b = 0; b < 6; b++)
        if (flags_ext_get(g_remain + b) != ((left >> b) & 1)) flags_put(g_remain + b, (left >> b) & 1);
    if (flags_ext_get(g_done) != (left == 0)) flags_put(g_done, left == 0);
    static int logged = -1;
    if (logged != left) {
        logged = left;
        int rb = 0;
        for (int b = 0; b < 6; b++) rb |= flags_ext_get(g_remain + b) << b;
        LOG("[triforce] %d found, %d to go (flags read back %d, done %d; bitfield %llx)\n", have, left, rb,
            flags_ext_get(g_done), (unsigned long long)LD64(GA(FLAG_PTR)));
    }
}

// Overlays (Items page, tracker): false when the seed has no hunt
bool triforce_counts(int *have, int *needed) {
    if (!on()) return false;
    *have = count();
    *needed = g_needed;
    return true;
}

// AddItem (keysanity.c rt_add_item): item id, index -> 1 when it was a piece (handled: nothing else is added)
int triforce_add_item(int item, int index) {
    if (!on() || item != g_item) return 0;
    if (index >= 0 && index < g_placed) flags_put(g_bits + index, true);
    sync_flags();
    g_got_frame = renderer_present_count();
    LOG("[triforce] piece %d: %d of %d\n", index, count(), g_needed);
    return 1;
}

void triforce_tick(void) {
    extern int game_in_play(void);
    if (!on() || !game_in_play()) return;
    // LARECOMP_TRIFORCE_GIVE=<frames in play>:<index>[,<index>...] (test): those pieces found
    static long give_at = -2;
    static const char *give_list;
    static uint64_t since;
    if (give_at == -2) {
        const char *e = getenv("LARECOMP_TRIFORCE_GIVE");
        give_at = e ? strtol(e, (char **)&give_list, 10) : -1;
        if (give_list && *give_list == ':') give_list++;
        else give_at = -1;
    }
    if (!since) since = renderer_present_count();
    if (give_at >= 0 && renderer_present_count() - since >= (uint64_t)give_at) {
        for (const char *p = give_list; *p;) {
            int i = (int)strtol(p, (char **)&p, 10);
            if (i >= 0 && i < g_placed) flags_put(g_bits + i, true);
            if (*p) p++;
        }
        LOG("[triforce] test: pieces %s given\n", give_list);
        give_at = -1;
    }
    if (g_nspots) {
        extern uint64_t doorjump_player(uint64_t frame);  // PlayerCollision component: position at +0x80
        uint64_t pc = doorjump_player(renderer_present_count());
        if (pc) {
            const float *pos = (const float *)(uintptr_t)(pc + 0x80);
            g_link_c = (int)floorf(pos[0] / 15.0f), g_link_r = (int)floorf(pos[2] / 12.0f);
        }
    }
    sync_flags();
}

static const char *level_name(void) {
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
    return ps ? (const char *)(uintptr_t)ps : "";
}

// The Piece of Power model's draws (texpatch: its colour map -> escape_bottle_tint kind 6): gold while a piece is
// being received (the hold-up and its message) and where a piece stands in the world. m: the draw's bone matrices
// (vertex cbuf 3: the first row-major 3x4, translation in m[3], m[7], m[11]) or NULL.
void triforce_model_tint(const float *m, uint32_t m_size, float tint[8]) {
    if (!on() && !getenv("LARECOMP_TRIFORCE_TINT")) return;
    bool gold = getenv("LARECOMP_TRIFORCE_TINT") || (g_got_frame && renderer_present_count() - g_got_frame <= 600);
    const char *lv = level_name();
    for (int i = 0; i < g_nspots && !gold; i++) {  // (a standing piece: while Link is in its room / screen)
        const Spot *sp = &g_spots[i];
        if (strcmp(sp->level, lv)) continue;
        gold = isnan(sp->x) || (g_link_c == (int)floorf(sp->x / 15.0f) && g_link_r == (int)floorf(sp->z / 12.0f));
    }
    (void)m, (void)m_size;
    if (!gold) return;
    // ltint (translate.c): colour.rgb, mix, glow.rgb, luminance power - bright Triforce gold, lit 3D
    static const float kGold[8] = {1.90f, 1.40f, 0.30f, 1.0f, 0.50f, 0.35f, 0.05f, 0.50f};
    memcpy(tint, kGold, sizeof kGold);
}
