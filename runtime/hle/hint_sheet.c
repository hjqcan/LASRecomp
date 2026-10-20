// Hint Sheet: every randomizer seed made with it (settings.json "hint-sheet": tools/rando/las_headless.py drops one
// more Bottle from the item pool) starts with the game's FIRST bottle (inventory bit 13, item slot 0xd) as the Hint
// Sheet: Tarin hands it over right after the Escape Bottle (runtime/hle/escape.c), in the game's own get-item message
// (tools/rando/hint_sheet.py; the second bottle is LAS's fishing-pond bottle, see there). Equipped on X / Y and used,
// the game's message box names where the player's next progression item lies.
//
// The use is caught like the Escape Bottle's, in the game's "may Link use this item now?" predicate for the slot
// (tools/patches.py BOTTLE1_USE_SITE: only while Link is free to act), and never swings or catches anything.
// The hints: rando/<seed>/hints.tsv lists the seed's progression steps in hint order (S <Common.bfevfl entry>
// <location> <item> <copy n> <Seashells / Golden Leaves it needs>) and the closing entry (D <entry>). The first step not yet collected is shown: a step is
// collected once its check's event flag is set (rando/<seed>/tracker.tsv), or once the save shows the item owned (at
// least n copies; items the save shows directly). A step whose check has no known flag and whose item the save can't
// show is passed over (never stuck on a hint the player already followed). All collected: the closing message.
// Showing it: the entry is requested from event/Common.bfevfl the way the game requests its own GameOver event
// (main+0xdaaf6c): the resource manager loads the flowchart (GA_EVENT_RES_LOAD, GA_EVENT_RES_WAIT, GA_EVENT_FLOW_HANDLE),
// an event request for the entry name (GA_EVENT_REQUEST_NEW, GA_EVENT_REQUEST_PREPARE) goes to the event manager
// (GA_EVENT_MGR, GA_EVENT_SUBMIT). The event holds Link while the box is up, like a signpost.
//
// Look: the Items page slot, the X / Y button and Tarin's hand-over show the dungeon map's icon / model tinted sky blue
// (hint_sheet_icon_draw / hint_sheet_tint below; runtime/nvn/texpatch.c texpatch_hint_icon). Nothing in the item
// logic knows about it.
#include "../game_addresses.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../rando.h"

extern uint64_t renderer_present_count(void);
extern int game_in_play(void);
extern int warp_busy(void);
extern bool flags_ext_get(int idx);
extern const char *souls_at(int i, int *kind, bool *owned);

#define EVENT_FLAGS (GA_LIVE_SAVE + 0x5DC0)
#define OWNED_BIT 0x20  // PlayerStatus +0xa9 bit 5: the first bottle (inventory bit 13)
#define SLOT 0xd        // its item slot (X / Y: PlayerStatus +0xa3 / +0xa2)
#define HINT_SLOT_X 228.0f  // its Items page slot (pane translation, 1280x720 layout, y up: second of the bottom row)
#define HINT_SLOT_Y -67.0f
static const char *const kTarinHouse = "MarinTarinHouse";

// LARECOMP_HINT_SHEET=1 / 0 forces it on / off (tests)
static int on = -1;
bool hint_sheet_on(void) {
    if (on >= 0) return on;
    const char *e = getenv("LARECOMP_HINT_SHEET");
    if (e) return on = atoi(e) != 0;
    on = 0;
    const char *dir = rando_seed_dir();
    if (!dir) return false;
    char path[512], buf[16384];
    snprintf(path, sizeof path, "%s/settings.json", dir);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    on = strstr(buf, "\"hint-sheet\": true") != NULL;  // (older seeds: no Hint Sheet)
    LOG("[hint] Hint Sheet %s\n", on ? "on" : "off");
    return on;
}

// ---- the seed's steps
typedef struct { char entry[24], loc[64], item[40], needs[48]; int n, flag; } Step;
static Step *g_steps;
static int g_nsteps, g_loaded;
static char g_done[24];
typedef struct { char name[48]; int idx; } NamedFlag;
static NamedFlag *g_flags;  // flags.tsv (trade items, pets)
static int g_nflags;

static int named_flag(const char *name) {
    for (int i = 0; i < g_nflags; i++)
        if (!strcmp(g_flags[i].name, name)) return g_flags[i].idx;
    return -1;
}

static void load(void) {
    g_loaded = 1;
    const char *dir = rando_seed_dir();
    if (!dir) return;
    char path[512], line[1024];
    snprintf(path, sizeof path, "%s/hints.tsv", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    int cap = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *c[6] = {0};
        int k = 0;
        for (char *s = line, *t; k < 6 && (t = strsep(&s, "\t"));) c[k++] = t;
        if (k >= 5 && c[0][0] == 'S') {
            if (g_nsteps == cap) g_steps = realloc(g_steps, (cap = cap ? cap * 2 : 64) * sizeof *g_steps);
            Step *st = &g_steps[g_nsteps++];
            memset(st, 0, sizeof *st);
            strlcpy(st->entry, c[1], sizeof st->entry);
            strlcpy(st->loc, c[2], sizeof st->loc);
            strlcpy(st->item, c[3], sizeof st->item);
            st->n = atoi(c[4]);
            if (k >= 6 && strcmp(c[5], "-")) strlcpy(st->needs, c[5], sizeof st->needs);
            st->flag = -1;
        } else if (k >= 2 && c[0][0] == 'D') strlcpy(g_done, c[1], sizeof g_done);
    }
    fclose(f);
    snprintf(path, sizeof path, "%s/tracker.tsv", dir);  // C <name> <x> <y> <group> <flag>
    if ((f = fopen(path, "r"))) {
        static char big[16384];
        while (fgets(big, sizeof big, f)) {
            if (big[0] != 'C' || big[1] != '\t') continue;
            big[strcspn(big, "\r\n")] = 0;
            char *c[6] = {0};
            int k = 0;
            for (char *s = big, *t; k < 6 && (t = strsep(&s, "\t"));) c[k++] = t;
            if (k < 6) continue;
            for (int i = 0; i < g_nsteps; i++)
                if (!strcmp(g_steps[i].loc, c[1])) g_steps[i].flag = atoi(c[5]);
        }
        fclose(f);
    }
    snprintf(path, sizeof path, "%s/flags.tsv", dir);
    if ((f = fopen(path, "r"))) {
        char name[128];
        int idx, fcap = 0;
        while (fscanf(f, "%127s %d", name, &idx) == 2) {
            if (g_nflags == fcap) g_flags = realloc(g_flags, (fcap = fcap ? fcap * 2 : 256) * sizeof *g_flags);
            strlcpy(g_flags[g_nflags].name, name, sizeof g_flags[0].name);
            g_flags[g_nflags++].idx = idx;
        }
        fclose(f);
    }
    LOG("[hint] %d steps (closing entry %s)\n", g_nsteps, g_done);
}

static bool flag_set(int i) {
    if (i >= 2048) return flags_ext_get(i);
    return i >= 0 && (LD8(EVENT_FLAGS + (i >> 3)) >> (i & 7)) & 1;
}

// How many of `item` the save shows (the check tracker's reading, runtime/ui/tracker_overlay.mm), -1: it can't tell
static int owned(const char *item) {
    uint32_t items = LD32(GA_LIVE_SAVE + 0x5568), keyitems = LD32(GA_LIVE_SAVE + 0x556C);
    uint8_t upg = LD8(GA_LIVE_SAVE + 0x555C), songs = LD8(GA_LIVE_SAVE + 0x555B);
#define B(v, i) ((int)(((v) >> (i)) & 1))
    if (!strcmp(item, "sword")) return B(items, 0) + (B(items, 0) && B(upg, 0));
    if (!strcmp(item, "shield")) return B(items, 1) + (B(items, 1) && B(upg, 1));
    if (!strcmp(item, "bracelet")) return B(items, 10) + (B(items, 10) && B(upg, 2));
    static const struct { const char *n; int bit; } it[] = {{"bomb", 2}, {"bow", 3}, {"hookshot", 4}, {"boomerang", 5},
                                                             {"rod", 6}, {"shovel", 7}, {"powder", 8}, {"feather", 9},
                                                             {"boots", 11}, {"ocarina", 12}};
    for (size_t i = 0; i < sizeof it / sizeof *it; i++)
        if (!strcmp(item, it[i].n)) return B(items, it[i].bit);
    if (!strcmp(item, "bottle")) return B(items, 14);  // (the one real bottle: the first is this, the third the Escape Bottle)
    static const struct { const char *n; int bit; } ki[] = {
        {"flippers", 0}, {"medicine", 1}, {"tail-key", 2}, {"slime-key", 3}, {"angler-key", 4}, {"face-key", 5},
        {"bird-key", 6}, {"full-moon-cello", 7}, {"conch-horn", 8}, {"sea-lilys-bell", 9}, {"surf-harp", 10},
        {"wind-marimba", 11}, {"coral-triangle", 12}, {"evening-calm-organ", 13}, {"thunder-drum", 14}, {"mushroom", 15}};
    for (size_t i = 0; i < sizeof ki / sizeof *ki; i++)
        if (!strcmp(item, ki[i].n)) return B(keyitems, ki[i].bit);
    if (!strcmp(item, "shell-sensor")) return LD8(GA_LIVE_SAVE + 0x556E) & 1;
    if (!strcmp(item, "song-ballad")) return B(songs, 0);
    if (!strcmp(item, "song-mambo")) return B(songs, 1);
    if (!strcmp(item, "song-soul")) return B(songs, 2);
    if (!strcmp(item, "red-tunic")) return B(upg, 6);
    if (!strcmp(item, "blue-tunic")) return B(upg, 7);
    if (!strcmp(item, "powder-capacity")) return B(upg, 3);
    if (!strcmp(item, "bomb-capacity")) return B(upg, 4);
    if (!strcmp(item, "arrow-capacity")) return B(upg, 5);
    if (!strcmp(item, "golden-leaf")) return __builtin_popcount(LD8(GA_LIVE_SAVE + 0x5582) & 0x1F);
    if (!strcmp(item, "seashell")) return __builtin_popcountll(LD64(GA_LIVE_SAVE + 0x5548) & ((1ull << 50) - 1));
#undef B
    static const struct { const char *tok, *flag; int slot; } trade[] = {
        {"yoshi-doll", "TradeYoshiDollGet", 1}, {"ribbon", "TradeRibbonGet", 2}, {"dog-food", "TradeDogFoodGet", 3},
        {"bananas", "TradeBananasGet", 4}, {"stick", "TradeStickGet", 5}, {"honeycomb", "TradeHoneycombGet", 6},
        {"pineapple", "TradePineappleGet", 7}, {"hibiscus", "TradeHibiscusGet", 8}, {"letter", "TradeLetterGet", 9},
        {"broom", "TradeBroomGet", 10}, {"fishing-hook", "TradeFishingHookGet", 11}, {"necklace", "TradeNecklaceGet", 12},
        {"mermaid-scale", "TradeMermaidsScaleGet", 13}, {"magnifying-lens", "", 14}};
    for (size_t i = 0; i < sizeof trade / sizeof *trade; i++)
        if (!strcmp(item, trade[i].tok))
            return LD8(GA_LIVE_SAVE + 0x555A) == trade[i].slot || flag_set(named_flag(trade[i].flag));
    static const struct { const char *tok, *flag; } pets[] = {
        {"rooster", "unused0427"}, {"marin-companion", "unused0428"}, {"ghost", "unused0429"}};
    for (size_t i = 0; i < sizeof pets / sizeof *pets; i++)
        if (!strcmp(item, pets[i].tok)) return flag_set(named_flag(pets[i].flag));
    if (!strncmp(item, "soul-", 5))
        for (int i = 0;; i++) {
            int kind;
            bool own;
            const char *nm = souls_at(i, &kind, &own);
            if (!nm) break;
            char t[48] = "soul-";
            for (size_t k = 5; *nm && k < sizeof t - 1; nm++, k++) t[k] = *nm == ' ' ? '-' : (char)(*nm >= 'A' && *nm <= 'Z' ? *nm + 32 : *nm);
            if (!strcmp(t, item)) return own;
        }
    return -1;
}

// Seashells and Golden Leaves are shown only when the next real step needs them (hint_sheet.py GATED, `needs`)
static bool gated(const Step *s) { return !strcmp(s->item, "seashell") || !strcmp(s->item, "golden-leaf"); }

static bool collected(const Step *s) {
    if (s->flag >= 0 && flag_set(s->flag)) return true;
    int have = owned(s->item);
    if (have >= s->n) return true;  // (owned, wherever it came from)
    return s->flag < 0 && have < 0;  // (nothing tells: passed over)
}

// The entry to show now: the first real step not collected - or, when it can't be reached without Seashells / Golden
// Leaves the player still lacks, the first of those missing - else the closing one
static const char *next_entry(int *index) {
    if (!g_loaded) load();
    for (int i = 0; i < g_nsteps; i++) {
        Step *s = &g_steps[i];
        if (gated(s) || collected(s)) continue;
        *index = i;
        for (int j = 0; j < g_nsteps; j++) {  // (needs: "<item>:<count>,...")
            Step *g = &g_steps[j];
            const char *nd = gated(g) ? strstr(s->needs, g->item) : NULL;
            if (!nd || nd[strlen(g->item)] != ':' || owned(g->item) >= atoi(nd + strlen(g->item) + 1) || collected(g)) continue;
            *index = j;
            break;
        }
        return g_steps[*index].entry;
    }
    *index = -1;
    return g_done[0] ? g_done : NULL;
}

// ---- use
static volatile int g_pending;
int rt_bottle1_use(void) {
    if (!hint_sheet_on()) return 0;
    static uint64_t last;
    uint64_t frame = renderer_present_count();
    if (frame - last < 30 || warp_busy() || g_pending) return 1;  // (the predicate can run more than once per press)
    last = frame;
    g_pending = 1;
    return 1;
}

// Requests Common.bfevfl entry `name` (game thread). false: the event couldn't be made.
static bool request_common_event(Cpu *c, const char *name) {
    static uint64_t scratch;
    static char names[64];
    if (!scratch) scratch = (uint64_t)(uintptr_t)hle_alloc(0x100);
    uint64_t S = scratch;
    memset((void *)(uintptr_t)S, 0, 0x100);
    strlcpy(names, name, sizeof names);
    uint64_t saved[31];
    memcpy(saved, c->x, sizeof saved);
    bool ok = false;
    uint64_t res_mgr = LD64(GA_EVENT_RES_MGR), mgr = LD64(GA_EVENT_MGR);
    if (res_mgr && mgr) {
        ST64(S + 0x00, GA_STRING_COMMON_FLOW);  // {"event/Common.bfevfl", 19}
        ST64(S + 0x08, 19);
        c->x[0] = S + 0x60; c->x[1] = res_mgr; c->x[2] = S + 0x00;
        guest_call(c, GA_EVENT_RES_LOAD);  // -> {pending load (shared), resource (shared)} at S+0x60
        if (LD64(S + 0x60)) {  // (still loading: wait for it)
            c->x[0] = LD64(S + 0x60);
            guest_call(c, GA_EVENT_RES_WAIT);
        }
        c->x[0] = S + 0x30; c->x[1] = S + 0x60;
        guest_call(c, GA_EVENT_FLOW_HANDLE);  // -> flowchart handle (shared) at S+0x30
        if (LD64(S + 0x30)) {
            ST64(S + 0x10, (uint64_t)(uintptr_t)names);
            ST64(S + 0x18, strlen(names));
            c->x[0] = S + 0x20; c->x[1] = S + 0x30; c->x[2] = S + 0x10;
            guest_call(c, GA_EVENT_REQUEST_NEW);  // -> request (shared) at S+0x20
            if (LD64(S + 0x20)) {
                c->x[0] = LD64(S + 0x20);
                guest_call(c, GA_EVENT_REQUEST_PREPARE);
                c->x[0] = S + 0x50; c->x[1] = mgr; c->x[2] = S + 0x20;
                guest_call(c, GA_EVENT_SUBMIT);  // -> (shared) at S+0x50
                ok = true;
            }
        }
        LOG("[hint] event %s: resource %llx/%llx flow %llx request %llx\n", name, (unsigned long long)LD64(S + 0x60),
            (unsigned long long)LD64(S + 0x70), (unsigned long long)LD64(S + 0x30), (unsigned long long)LD64(S + 0x20));
        static const int ctrl[] = {0x58, 0x28, 0x38, 0x68, 0x78};  // (our references, released)
        for (int i = 0; i < 5; i++)
            if (LD64(S + ctrl[i])) { c->x[0] = LD64(S + ctrl[i]); guest_call(c, GA_SP_RELEASE); }
    }
    memcpy(c->x, saved, sizeof saved);
    return ok;
}

void hint_sheet_tick(Cpu *c, uint64_t frame) {
    (void)frame;
    // LARECOMP_HINT_SHOW=<frame>: use the Hint Sheet at that frame (test: no input needed)
    static long at = -2;
    if (at == -2) at = getenv("LARECOMP_HINT_SHOW") ? atol(getenv("LARECOMP_HINT_SHOW")) : -1;
    if (at >= 0 && (long)frame >= at && game_in_play()) { at = -1; g_pending = 1; }
    if (!g_pending || !game_in_play()) return;
    g_pending = 0;
    if (!hint_sheet_on()) return;
    int i;
    const char *e = next_entry(&i);
    if (!e) { LOG("[hint] no hints.tsv for this seed\n"); return; }
    bool ok = request_common_event(c, e);
    LOG("[hint] shows %s (step %d%s%s)%s\n", e, i, i >= 0 ? ": " : "", i >= 0 ? g_steps[i].loc : "", ok ? "" : " - request failed");
}

RT_ON_RESET(hint_sheet_reset) {  // (seed switch: the next seed may differ)
    on = -1;
    free(g_steps);
    g_steps = NULL;
    g_nsteps = 0;
    g_loaded = 0;
    g_done[0] = 0;
    free(g_flags);
    g_flags = NULL;
    g_nflags = 0;
    g_pending = 0;
}

// ---- look
// Is this draw of the bottle's item icon (UI pane matrix in vertex UBO 0, as escape.c's escape_bottle_tint reads it)
// the Hint Sheet's? Its Items page slot (second of the bottom row) and the X / Y button holding item slot 0xd.
// LARECOMP_LOG_BOTTLE_PANES=1 logs every bottle icon's position (finding slots).
bool hint_sheet_icon_draw(const float *ui, uint32_t ui_size) {
    if (!ui || ui_size < 24 * 4 || !hint_sheet_on()) return false;
    uint64_t ps = LD64(GA_PLAYER_STATUS_PTR);
    if (!ps || !(LD8(ps + 0xa9) & OWNED_BIT)) return false;
    float x = ui[19], y = ui[23];
    static int logp = -1;
    if (logp < 0) logp = getenv("LARECOMP_LOG_BOTTLE_PANES") != NULL;
    if (logp) {
        static uint64_t lf;
        static int n;
        uint64_t f = renderer_present_count();
        if (f != lf) lf = f, n = 0;
        if (n++ < 6 && f % 60 == 0) LOG("[hint] bottle icon at %.1f, %.1f (scale %.2f)\n", x, y, ui[16]);
    }
    bool hit = fabsf(x - HINT_SLOT_X) < 24 && fabsf(y - HINT_SLOT_Y) < 24;
    if (LD8(ps + 0xa2) == SLOT) hit |= fabsf(x - 476) < 24 && fabsf(y - 296) < 24;
    if (LD8(ps + 0xa3) == SLOT) hit |= fabsf(x - 571) < 24 && fabsf(y - 310) < 24;
    return hit;
}

// The tint (renderer ltint: rgb = mix(rgb, t0.rgb * lum^t1.w + t1.rgb * lum, t0.w)) for a bottle-kind draw
// (texpatch_bottle_kind): 1 = the bottle's icon (here: the Hint Sheet's, the dungeon map's icon drawn in its place),
// 5 = the dungeon map's model while Tarin hands the Hint Sheet over. LARECOMP_HINT_TINT=<8 floats> (tests).
static void sky_blue(float tint[8], bool lit) {
    static float env[8];
    static int have = -1;
    if (have < 0) {
        const char *e = getenv("LARECOMP_HINT_TINT");
        have = e && sscanf(e, "%f,%f,%f,%f,%f,%f,%f,%f", &env[0], &env[1], &env[2], &env[3], &env[4], &env[5], &env[6],
                           &env[7]) == 8;
    }
    // light sky blue (never the map's own orange paper): UI icon unlit, the held-up model lit
    static const float ui[8] = {0.45f, 0.85f, 1.40f, 1.0f, 0.06f, 0.12f, 0.18f, 1.0f};
    static const float model[8] = {0.45f, 0.85f, 1.40f, 1.0f, 0.06f, 0.12f, 0.18f, 1.0f};
    memcpy(tint, have ? env : lit ? model : ui, 8 * sizeof(float));
}

bool hint_sheet_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]) {
    if (!hint_sheet_on()) return false;
    if (kind == 5) {
        uint64_t ps = LD64(GA_PLAYER_STATUS_PTR);
        if (!ps || (LD8(ps + 0xa9) & OWNED_BIT) || strcmp((const char *)(uintptr_t)ps, kTarinHouse)) return false;
        sky_blue(tint, true);
        return true;
    }
    if (kind != 1 || !hint_sheet_icon_draw(ui, ui_size)) return false;
    sky_blue(tint, false);
    return true;
}
