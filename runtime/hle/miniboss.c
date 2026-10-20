// Mini-bosses on the overworld (enemy randomizer, "Include mini-bosses"; kinds in LAS enemies.py FIELD_MINIBOSSES).
// Armos Knight, Giant Buzz Blob and Dodongo Snakes fight on their own outside their arenas. Hinox sits in its idle
// state until its dungeon room starts it: entering the room's camera zone (zone manager -> f_7000029520 -> actor
// vfunc +0x100) hands a callback to the room's AreaEventBox, Link inside the box fires it, the actor's next state
// requests the HinoxStart event (BGM, camera, shutters) and that event's end moves it into its fight state. The Field
// has no zones or boxes, so here the runtime moves it into the fight state itself (no start event: no boss music or
// shutter steps) once Link comes near. Actors are seen through a per-frame type check (tools/patches.py
// MB_SEEN_SITE) and picked out by class vtable.
#include "../game_addresses.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

void guest_call(Cpu *c, uint64_t addr);
uint64_t doorjump_player(uint64_t frame);  // PlayerCollision component: position at +0x80

// GA_PLAYER_STATUS_PTR: PlayerStatus*: current level name at +0

// per class: vtable, its state machine's change(sm, state) (+0xd current state, +0xe previous; states 0x30 apart
// from +0x40: ctx, enter, exec, exit member functions), the state machine's offset in the actor, idle state, fight
// state, position offset (x y z floats: the spawn point), rest state (0xff: none) - one it stays in after a move
// that ends its fight in its own room (Blaino's knock-out): woken again like idle, then (0: none) - a state it goes
// on to a few frames after waking (Master Stalfos: 13 shows him, 1 is his fight), at_once: woken as soon as he's
// loaded, wherever Link is (an idle Master Stalfos is invisible), start (main offset, 0: none) - its own start action,
// called with the actor instead of the state change (Lanmola: what his screen's event runs - up out of the sand), own
// (NULL: the Field only) - also a plain enemy in dungeons, caves and houses (enemies.yml, Include mini-bosses): copies
// there are woken too; the original (its level name starts with `own`; a copy keeps the replaced enemy's) is left to
// its arena
typedef struct {
    uint64_t vt, chg; uint32_t sm, pos; uint8_t idle, fight; const char *name; uint8_t rest, then, at_once; uint32_t start;
    const char *own;
} Kind;
static const Kind g_kinds[] = {
    {GA_HINOX_VTABLE, GA_HINOX_CHANGE_STATE, 0xff0, 0x260, 0, 1, "Hinox", 0xff},                          // 0x53: 10 = request HinoxStart
    // (found 2026-10-12 with tools placing each one on Field_13D: LARECOMP_MB_SCAN_NAME / _FORCE)
    {GA_SMASHER_VTABLE, GA_SMASHER_CHANGE_STATE, 0x1110, 0x260, 0, 1, "Smasher", 0xff},                   // 0x5c (+ iron ball)
    {GA_AVALAUNCH_VTABLE, GA_AVALAUNCH_CHANGE_STATE, 0xff0, 0x260, 0, 1, "Avalaunch", 0xff},              // 0x60
    {GA_ROLLING_BONES_VTABLE, GA_ROLLING_BONES_CHANGE_STATE, 0xff0, 0x260, 0, 1, "Rolling Bones", 0xff},  // 0x51 (+ spike bar)
    {GA_GOHMA_VTABLE, GA_GOHMA_CHANGE_STATE, 0x1200, 0x260, 13, 0, "Gohma", 0xff},                        // 0x5a (heads for Link: GOHMA_*)
    {GA_BLAINO_VTABLE, GA_BLAINO_CHANGE_STATE, 0x11a0, 0x260, 10, 1, "Blaino", 5, 0, 0, 0, "EnemyMightPu"},  // 0x5f (5: after a knock-out)
    {GA_CUE_BALL_VTABLE, GA_CUE_BALL_CHANGE_STATE, 0xff0, 0x260, 0, 3, "Cue Ball", 0xff},                 // 0x58 (+ a rail fitted)
    // 0x57: 0 = under the sand until his screen's event (AreaEventBox - not copied) runs his start; back in 0, again
    {GA_LANMOLA_VTABLE, GA_LANMOLA_CHANGE_STATE, 0xff0, 0x260, 0, 1, "Lanmola", 0, 0, 0, (GA_LANMOLA_START - MAIN_BASE)},
    // 0x59: 12 = hidden until his room's event (AreaEventBox - not copied: intro, music) drops him in; 13 shows him
    {GA_MASTER_STALFOS_VTABLE, GA_MASTER_STALFOS_CHANGE_STATE, 0x1230, 0x260, 12, 13, "Master Stalfos", 0xff, 1, 1},
};
#define NKINDS (int)(sizeof g_kinds / sizeof g_kinds[0])
#define WAKE_DIST 7.0f  // (a Field screen is ~15 x 12 units)

typedef struct { uint64_t obj, first, last, woken_at; int kind, woken; float px, pz; int stuck; uint64_t flip_at; } Seen;
// (frequency All: up to 6 on a screen, and the Field keeps the screens around Link loaded - room for all of them)
#define MAX_SEEN 128
static Seen g_seen[MAX_SEEN];
RT_ON_RESET(miniboss_reset) { memset(g_seen, 0, sizeof g_seen); }
extern uint64_t renderer_present_count(void);

static void log_new_vtable(uint64_t o, uint64_t vt) {  // LARECOMP_MB_LOG (RE): every actor class seen, once
    static uint64_t vts[512];
    static int n;
    for (int i = 0; i < n; i++)
        if (vts[i] == vt) return;
    if (n < 512) vts[n++] = vt;
    char name[40] = "";  // (the actor's level name, "EnemyRola-<key>", when it keeps one at +0x398 like item actors)
    for (uint64_t off = 0; off < 0x3000 && !name[0]; off += 8) {
        const char *nm = (const char *)(uintptr_t)(o + off);
        if (memcmp(nm, "Enemy", 5) && memcmp(nm, "Obj", 3) && memcmp(nm, "Npc", 3)) continue;
        for (int i = 0; i < 39 && nm[i] >= 0x20 && nm[i] < 0x7f; i++) name[i] = nm[i], name[i + 1] = 0;
        snprintf(name + strlen(name), sizeof name - strlen(name), "@%llx", (unsigned long long)off);
    }
    LOG("[mb] new class vtable main+%#llx (actor %llx, +0x100 main+%#llx) %s\n", (unsigned long long)(vt - MAIN_BASE),
        (unsigned long long)o, (unsigned long long)(LD64(vt + 0x100) - MAIN_BASE), name);
}

// LARECOMP_MB_SCAN=<vtable offsets, hex, comma-separated> (RE): finds the state machines inside actors of those classes
// (ctx pointer at +0x40 = the actor, state 0's enter function at +0x48 / +0x50), lists their states and logs changes
static uint64_t g_scan_vt[16], g_scan_obj[8], g_scan_sm[8][8];
static uint8_t g_scan_last[8][8];
static int g_scan_nv = -1;

static bool code_ptr(uint64_t p) { return p >= MAIN_BASE && p < GA_MAIN_CODE_SCAN_END; }

static void scan_seen(uint64_t o, uint64_t vt) {
    if (g_scan_nv < 0) {
        g_scan_nv = 0;
        for (const char *p = ENV("LARECOMP_MB_SCAN"); p && *p && g_scan_nv < 16;) {
            g_scan_vt[g_scan_nv++] = MAIN_BASE + strtoull(p, (char **)&p, 16);
            while (*p == ',') p++;
        }
    }
    int k = 0;
    while (k < g_scan_nv && g_scan_vt[k] != vt) k++;
    // LARECOMP_MB_SCAN_NAME=<text>: (also) actors whose level name (+0x18, "EnemyOctorok-<key>") contains it - a
    // placed test actor keeps the name of the one it replaced; classes share base vtables
    const char *want = ENV("LARECOMP_MB_SCAN_NAME");
    bool named = want && strstr((const char *)(uintptr_t)(o + 0x18), want);
    if (k == g_scan_nv && !named) return;
    int fr = -1;
    for (int i = 0; i < 8; i++) {
        if (g_scan_obj[i] == o) return;
        if (fr < 0 && !g_scan_obj[i]) fr = i;
    }
    if (fr < 0) return;
    g_scan_obj[fr] = o;
    memset(g_scan_sm[fr], 0, sizeof g_scan_sm[fr]);
    int n = 0;
    for (uint64_t off = 0; off < 0x2400 && n < 8; off += 8)
        if (LD64(o + off + 0x40) == o && (code_ptr(LD64(o + off + 0x48)) || code_ptr(LD64(o + off + 0x50)))) {
            g_scan_sm[fr][n++] = off;
            g_scan_last[fr][n - 1] = *(uint8_t *)(uintptr_t)(o + off + 0xd);
            LOG("[mb-scan] actor %llx (main+%#llx): state machine at +%#llx, state %u\n", (unsigned long long)o,
                (unsigned long long)(vt - MAIN_BASE), (unsigned long long)off, g_scan_last[fr][n - 1]);
            for (int s = 0; s < 24; s++) {  // (entries: ctx, then enter / exec / exit member pointers, 16 bytes each)
                uint64_t e = o + off + 0x40 + s * 0x30, f1 = LD64(e + 8), f2 = LD64(e + 0x18), f3 = LD64(e + 0x28);
                if (!code_ptr(f1) && !code_ptr(f2) && code_ptr(LD64(e + 0x10))) f1 = LD64(e + 0x10), f2 = LD64(e + 0x20), f3 = 0;
                if (code_ptr(f1) || code_ptr(f2) || code_ptr(f3))
                    LOG("[mb-scan]   state %d: enter %llx exec %llx exit %llx\n", s, (unsigned long long)f1,
                        (unsigned long long)f2, (unsigned long long)f3);
            }
        }
}

// LARECOMP_MB_DIFF (RE, with LARECOMP_MB_SCAN): at each state change of a scanned actor, its 32-bit fields (first
// 0x2000 bytes) that went down by 1..64 since the last change - its hit points among them
static void scan_diff(int i, uint64_t o) {
    static uint32_t snap[8][0x800];
    static uint64_t snap_obj[8];
    const uint32_t *now = (const uint32_t *)(uintptr_t)o;
    if (snap_obj[i] == o && !strcmp(ENV("LARECOMP_MB_DIFF"), "8")) {  // (=8: bytes and floats too)
        const uint8_t *nb = (const uint8_t *)now, *sb = (const uint8_t *)snap[i];
        for (int b = 0; b < 0x2000; b++)
            if (nb[b] < sb[b] && sb[b] - nb[b] <= 16 && sb[b] < 64) LOG("[mb-diff]   +%#x (u8): %u -> %u\n", b, sb[b], nb[b]);
        const float *nf = (const float *)now, *sf = (const float *)snap[i];
        for (int w = 0; w < 0x800; w++)
            if (nf[w] < sf[w] && sf[w] - nf[w] <= 16 && sf[w] >= 1 && sf[w] <= 100 && nf[w] == floorf(nf[w]) && sf[w] == floorf(sf[w]))
                LOG("[mb-diff]   +%#x (f32): %g -> %g\n", w * 4, sf[w], nf[w]);
    } else if (snap_obj[i] == o)
        for (int w = 0; w < 0x800; w++)
            if (now[w] < snap[i][w] && snap[i][w] - now[w] <= 64 && snap[i][w] < 0x10000)
                LOG("[mb-diff]   +%#x: %u -> %u\n", w * 4, snap[i][w], now[w]);
    memcpy(snap[i], now, sizeof snap[i]);
    snap_obj[i] = o;
}

static void scan_tick(uint64_t frame) {
    for (int i = 0; i < 8; i++) {
        uint64_t o = g_scan_obj[i];
        if (!o) continue;
        int alive = ENV("LARECOMP_MB_SCAN_NAME") && strstr((const char *)(uintptr_t)(o + 0x18), ENV("LARECOMP_MB_SCAN_NAME"));
        for (int k = 0; k < g_scan_nv; k++) alive |= LD64(o) == g_scan_vt[k];
        if (!alive) { g_scan_obj[i] = 0; continue; }
        for (int j = 0; j < 8 && g_scan_sm[i][j]; j++) {
            uint8_t st = *(uint8_t *)(uintptr_t)(o + g_scan_sm[i][j] + 0xd);
            if (st != g_scan_last[i][j]) {
                LOG("[mb-scan] f%llu actor %llx sm +%#llx: %u -> %u\n", (unsigned long long)frame, (unsigned long long)o,
                    (unsigned long long)g_scan_sm[i][j], g_scan_last[i][j], st);
                if (ENV("LARECOMP_MB_DIFF") && j == 0) scan_diff(i, o);
            }
            g_scan_last[i][j] = st;
        }
    }
}

static void shadow_link_seen(uint64_t o);
void rt_mb_seen(uint64_t o) {
    uint64_t vt = LD64(o);
    if (vt == GA_SHADOW_LINK_VTABLE) shadow_link_seen(o);
    if (ENV("LARECOMP_MB_LOG")) log_new_vtable(o, vt);
    if (ENV("LARECOMP_MB_SCAN") || ENV("LARECOMP_MB_SCAN_NAME")) scan_seen(o, vt);
    int k = 0;
    while (k < NKINDS && g_kinds[k].vt != vt) k++;
    if (k == NKINDS) return;
    uint64_t f = renderer_present_count();
    int free_i = -1;
    for (int i = 0; i < MAX_SEEN; i++) {
        if (g_seen[i].obj == o) { g_seen[i].last = f; return; }
        if (free_i < 0 && (!g_seen[i].obj || f - g_seen[i].last > 120)) free_i = i;
    }
    if (free_i >= 0) g_seen[free_i] = (Seen){o, f, f, 0, k, 0, 0, 0, 0, 0};
}

// LARECOMP_MB_FORCE=<frame>:<sm offset hex>:<state>[:<change fn main offset hex>] (RE, with LARECOMP_MB_SCAN_NAME):
// at that frame the scanned actor's state machine changes to that state (default change: Hinox's, main+0x3bfc60)
static uint64_t scan_latest(void) {  // (the newest scanned actor: a reloaded room leaves stale copies in the list)
    for (int i = 7; i >= 0; i--)
        if (g_scan_obj[i]) return g_scan_obj[i];
    return 0;
}

static void force_tick(Cpu *c, uint64_t frame) {
    static long at = -2;
    static uint64_t sm, chg;
    static int state;
    if (at == -2) {
        at = -1;
        const char *e = ENV("LARECOMP_MB_FORCE");
        unsigned long long smo = 0, ch = GA_HINOX_CHANGE_STATE - MAIN_BASE;
        if (e && sscanf(e, "%ld:%llx:%d:%llx", &at, &smo, &state, &ch) >= 3) sm = smo, chg = MAIN_BASE + ch;
        else at = -1;
    }
    static long zat = -2;  // LARECOMP_MB_ZONE=<frame>: the dungeon zone manager's room-entry call on the scanned actor
    if (zat == -2) zat = ENV("LARECOMP_MB_ZONE") ? atol(ENV("LARECOMP_MB_ZONE")) : -1;
    if (zat >= 0 && (long)frame == zat && scan_latest()) {
        uint64_t saved[31], sp = c->sp;
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = scan_latest();
        guest_call(c, GA_AREA_EVENT_TRIGGER);
        memcpy(c->x, saved, sizeof saved);
        c->sp = sp;
        LOG("[mb-zone] f%llu actor %llx: zone entry call\n", (unsigned long long)frame, (unsigned long long)scan_latest());
    }
    if (at < 0 || (long)frame != at || !scan_latest()) return;
    uint64_t saved[31], sp = c->sp;
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = scan_latest() + sm;
    c->x[1] = (uint64_t)state;
    guest_call(c, chg);
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp;
    LOG("[mb-force] f%llu actor %llx sm +%#llx -> state %d\n", (unsigned long long)frame, (unsigned long long)scan_latest(),
        (unsigned long long)sm, state);
}

// LARECOMP_MB_BOT=<frame>[:<safe states, '.'-separated>] (test, with LARECOMP_MB_SCAN_NAME): from that frame Link
// walks at the scanned actor and slashes when close - a scripted fight (LARECOMP_CHEATS=hearts keeps him alive); with
// safe states (its first state machine's), he backs off while it is in any other
static void bot_tick(uint64_t frame) {
    static long at = -2;
    static int safe[8], nsafe;
    if (at == -2) {
        const char *e = ENV("LARECOMP_MB_BOT");
        at = e ? strtol(e, (char **)&e, 10) : -1;
        while (e && *e && nsafe < 8) safe[nsafe++] = (int)strtol(e + 1, (char **)&e, 10);
    }
    if (at < 0 || (long)frame < at) return;
    uint64_t o = scan_latest(), comp = o ? LD64(o + 0x80) : 0, pc = doorjump_player(frame);
    static uint64_t had;
    if (had && !o) LOG("[mb-bot] f%llu target %llx gone\n", (unsigned long long)frame, (unsigned long long)had);
    had = o;
    if (!comp || !pc) return;
    const float *p = (const float *)(uintptr_t)(comp + 0x80), *l = (const float *)(uintptr_t)(pc + 0x80);
    float dx = p[0] - l[0], dz = p[2] - l[2], d = sqrtf(dx * dx + dz * dz);
    if (d < 0.01f || d > 40) return;
    extern int32_t g_test_lx, g_test_ly;
    extern uint64_t g_test_stick_until, g_test_buttons, g_test_buttons_until;
    if (frame % 120 == 0) LOG("[mb-bot] f%llu target %.1f away\n", (unsigned long long)frame, d);
    bool ok = !nsafe;
    for (int i = 7; i >= 0 && !ok; i--)
        if (g_scan_obj[i] == o)
            for (int k = 0; k < nsafe; k++) ok |= g_scan_last[i][0] == safe[k];
    if (!ok) {  // (attacking: away from it)
        if (d < 4.5f) g_test_lx = (int32_t)(-dx / d * 32767), g_test_ly = (int32_t)(dz / d * 32767), g_test_stick_until = frame + 2;
        return;
    }
    static uint64_t charge_from;  // (LARECOMP_MB_BOT_SPIN: B held while closing in, released next to it - spin attack)
    if (ENV("LARECOMP_MB_BOT_SPIN")) {
        if (!charge_from || frame > g_test_buttons_until + 20) charge_from = frame;
        bool charged = frame - charge_from > 70;
        if (!(charged && d < 1.8f)) g_test_buttons = 2, g_test_buttons_until = frame + 1;
        else charge_from = 0;
        if (d > 1.0f) g_test_lx = (int32_t)(dx / d * 32767), g_test_ly = (int32_t)(-dz / d * 32767), g_test_stick_until = frame + 1;
        return;
    }
    if (d > 1.6f || frame % 16 == 0) {
        g_test_lx = (int32_t)(dx / d * 32767), g_test_ly = (int32_t)(-dz / d * 32767);
        g_test_stick_until = frame + 2;
    } else if (frame % 16 == 2) {
        g_test_buttons = 2, g_test_buttons_until = frame + 6;
    }
}

// LARECOMP_MB_LIFE=<frame>:<component offset hex>[:<field hex>:<int16>] (test, with LARECOMP_MB_SCAN_NAME): at that
// frame log the scanned actor's component [actor + offset] (its first 0x100 bytes as int16) and optionally set a field
static void life_tick(uint64_t frame) {
    static long at = -2;
    static unsigned long long off, field;
    static int val, set;
    if (at == -2) {
        const char *e = ENV("LARECOMP_MB_LIFE");
        at = -1;
        if (e) set = sscanf(e, "%ld:%llx:%llx:%d", &at, &off, &field, &val) == 4;
    }
    if (at < 0 || (long)frame != at || !scan_latest()) return;
    for (uint64_t o = 0x80; !off && o < 0x3000; o += 8) {  // (offset 0: the first member that is a life component)
        uint64_t q = LD64(scan_latest() + o);
        if (q >= HEAP_BASE && q + 0x200 <= HEAP_BASE + HEAP_MAX && !(q & 7) && LD64(q) == GA_ENEMY_LIFE_VTABLE) off = o;
    }
    LOG("[mb-life] component at +%#llx\n", off);
    uint64_t comp = LD64(scan_latest() + off);
    if (comp < HEAP_BASE || comp + 0x100 > HEAP_BASE + HEAP_MAX) return;
    char line[2048];
    int n = 0;
    for (int i = 0; i < 0x100; i += 2) n += snprintf(line + n, sizeof line - n, "%x:%d ", i, *(int16_t *)(uintptr_t)(comp + i));
    LOG("[mb-life] f%llu %llx: %s\n", (unsigned long long)frame, (unsigned long long)comp, line);
    if (set) *(int16_t *)(uintptr_t)(comp + field) = (int16_t)val;
    if (ENV("LARECOMP_MB_SMDUMP")) {  // (RE: a state machine's raw entries)
        uint64_t sm = scan_latest() + strtoull(ENV("LARECOMP_MB_SMDUMP"), NULL, 16);
        for (int st = 0; st < 20; st++) {
            uint64_t e = sm + 0x40 + st * 0x30;
            LOG("[mb-sm] %d: %llx %llx %llx %llx %llx %llx\n", st, (unsigned long long)LD64(e), (unsigned long long)LD64(e + 8),
                (unsigned long long)LD64(e + 0x10), (unsigned long long)LD64(e + 0x18), (unsigned long long)LD64(e + 0x20),
                (unsigned long long)LD64(e + 0x28));
        }
    }
}

// LARECOMP_MB_KILL=<frame>:<sm offset hex>:<change fn main offset hex> (test, with LARECOMP_MB_SCAN_NAME): the scanned
// actor dies as from a last hit - hit points 0, then its state machine to its End event's state (the state whose
// entry is an event state named <Name>End)
static void kill_tick(Cpu *c, uint64_t frame) {
    static long at = -2;
    static unsigned long long smo, chg;
    if (at == -2) {
        const char *e = ENV("LARECOMP_MB_KILL");
        at = -1;
        if (!e || sscanf(e, "%ld:%llx:%llx", &at, &smo, &chg) != 3) at = -1;
    }
    uint64_t a = scan_latest();
    if (at < 0 || (long)frame != at || !a) return;
    for (uint64_t o = 0x80; o < 0x3000; o += 8) {
        uint64_t q = LD64(a + o);
        if (q >= HEAP_BASE && q + 0x200 <= HEAP_BASE + HEAP_MAX && !(q & 7) && LD64(q) == GA_ENEMY_LIFE_VTABLE) { ST32(q + 0x168, 0); break; }
    }
    int dead = -1;
    for (int st = 0; st < 24 && dead < 0; st++) {
        uint64_t obj = LD64(a + smo + 0x40 + st * 0x30 + 8);
        if (obj < HEAP_BASE || obj + 0x40 > HEAP_BASE + HEAP_MAX || LD64(a + smo + 0x40 + st * 0x30 + 0x18)) continue;
        uint64_t len = LD64(obj + 0x30);
        if (len >= 3 && len <= 0x18 && !memcmp((const char *)(uintptr_t)(obj + 0x10 + len - 3), "End", 3)) dead = st;
    }
    LOG("[mb-kill] f%llu actor %llx: End state %d\n", (unsigned long long)frame, (unsigned long long)a, dead);
    if (dead < 0) return;
    uint64_t saved[31], sp = c->sp;
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = a + smo, c->x[1] = (uint64_t)dead;
    guest_call(c, MAIN_BASE + chg);
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp;
}

// A copy of a mini-boss whose flow gives a check (Lanmola, Armos Knight) skips the item when its switch 2 - the seed's
// OverworldMinibossMarker flag (settings.json "miniboss-marker-flag", tools/rando/miniboss_guard.py) - is set: set
// every frame in a seed (an Armos Knight copy fights on its own, nothing wakes it). Copies also take a short death
// path on it (no camera, HUD, music or doors: Link keeps control) and a Giant Buzz Blob copy skips its sword hint. The originals have no switch 2
// set: they give it as before. Written through the flag object (main+0x1CC1388, its byte capacity +0x1CC1390): the
// flag may be past the game's 2048 (runtime/hle/flags_ext.c).
static int g_marker = -2;
RT_ON_RESET(miniboss_marker_reset) { g_marker = -2; }
static void miniboss_marker_set(void) {
    if (g_marker == -2) {
        g_marker = -1;
        extern const char *rando_seed_dir(void);
        const char *dir = rando_seed_dir();
        char p[512];
        FILE *f = dir ? (snprintf(p, sizeof p, "%s/settings.json", dir), fopen(p, "rb")) : NULL;
        if (f) {
            static char buf[1 << 16];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
            const char *k = strstr(buf, "\"miniboss-marker-flag\":");
            if (k) g_marker = atoi(k + strlen("\"miniboss-marker-flag\":"));
        }
    }
    if (g_marker < 0) return;
    uint8_t *fl = (uint8_t *)(uintptr_t)LD64(GA_FLAG_PTR);
    if (!fl || (uint32_t)(g_marker >> 3) >= LD32(GA_FLAG_CAP)) return;
    fl[g_marker >> 3] |= (uint8_t)(1 << (g_marker & 7));
}

// Gohma on the Field (tools/patches.py GOHMA_*): its moves head for Link (angle units: 2^31 = pi, 0 = +z) - or, with
// a wall that way, the nearest open direction - and have no track end. Its arena's walls are the track ends; here the
// screen's walls (rando/<seed>/roam.tsv, tools/rando/roam.py) end a move the way its own timer does (gohma_walls).
// g_link: Link's spot (miniboss_tick).
static float g_link[3];
static bool g_have_link;

typedef struct { float x0, z0, ts; int w, h; char cells[96]; } WallScreen;
static WallScreen *g_walls;
static int g_nwalls = -1;
RT_ON_RESET(walls_reset) { free(g_walls), g_walls = NULL, g_nwalls = -1; }

static void walls_load(void) {
    extern const char *rando_seed_dir(void);
    g_nwalls = 0;
    const char *dir = rando_seed_dir();
    char path[1024];
    if (!dir) return;
    snprintf(path, sizeof path, "%s/roam.tsv", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    g_walls = calloc(512, sizeof *g_walls);
    char cells[128];
    WallScreen w;
    while (g_nwalls < 512 && fscanf(f, "%f\t%f\t%f\t%d\t%d\t%127s", &w.x0, &w.z0, &w.ts, &w.w, &w.h, cells) == 6)
        if (w.w * w.h <= (int)sizeof w.cells && (int)strlen(cells) == w.w * w.h)
            memcpy(w.cells, cells, (size_t)(w.w * w.h)), g_walls[g_nwalls++] = w;
    fclose(f);
}

#define GOHMA_BODY 1.0f  // (half its width)
static bool wall_at(float x, float z) {  // (Gohma's body around (x, z) touches a wall or the island's edge)
    if (g_nwalls < 0) walls_load();
    if (!g_nwalls) return false;
    for (int k = 0; k < 5; k++) {
        float px = x + (k == 1 ? GOHMA_BODY : k == 2 ? -GOHMA_BODY : 0), pz = z + (k == 3 ? GOHMA_BODY : k == 4 ? -GOHMA_BODY : 0);
        bool open = false;
        for (int i = 0; i < g_nwalls && !open; i++) {
            const WallScreen *r = &g_walls[i];
            int cx = (int)floorf((px - r->x0) / r->ts), cz = (int)floorf((pz - r->z0) / r->ts);
            if (cx >= 0 && cx < r->w && cz >= 0 && cz < r->h) open = r->cells[cz * r->w + cx] == '.';
        }
        if (!open) return true;
    }
    return false;
}
int rt_gohma_free(void) {
    extern const char *rando_seed_dir(void);
    uint64_t ps = LD64(GA_PLAYER_STATUS_PTR);
    return rando_seed_dir() && ps && !strcmp((const char *)(uintptr_t)ps, "Field");
}

uint32_t rt_gohma_heading(uint64_t gohma, uint32_t track) {
    uint64_t comp = LD64(gohma + 0x80);
    if (!rt_gohma_free() || !g_have_link || !comp) return track;
    const float *p = (const float *)(uintptr_t)(comp + 0x80);
    float dx = g_link[0] - p[0], dz = g_link[2] - p[2];
    if (dx * dx + dz * dz < 0.01f) return track;
    float a = atan2f(dx, dz);
    for (int t = 0; t < 8; t++) {  // (toward Link, else the nearest open way: 45 degree steps either side)
        float b = a + (t + 1) / 2 * 0.7853982f * (t % 2 ? 1 : -1);
        if (!wall_at(p[0] + sinf(b) * 1.5f, p[2] + cosf(b) * 1.5f)) { a = b; break; }
    }
    if (ENV("LARECOMP_LOG_ROAM")) LOG("[roam] Gohma at %.1f %.1f heads %.0f deg (Link %.1f %.1f)\n", p[0], p[2], a * 57.29578f, g_link[0], g_link[2]);
    return (uint32_t)(int32_t)(a * (2147483648.0f / 3.14159265f));
}

// Each frame: a wall just ahead on Gohma's heading ends its move (its timer +0x15a4 reaches its limit +0x15a6, so its
// own next state follows), and this frame's step is dropped so it doesn't push in.
static void gohma_walls(uint64_t gohma) {
    uint64_t comp = LD64(gohma + 0x80);
    if (!comp) return;
    const float *p = (const float *)(uintptr_t)(comp + 0x80);
    float speed = *(float *)(uintptr_t)(gohma + 0x15e4);
    if (speed <= 0) return;
    float a = (float)*(int32_t *)(uintptr_t)(gohma + 0x15ec) * (3.14159265f / 2147483648.0f);
    float ahead = speed + 0.1f;
    if (!wall_at(p[0] + sinf(a) * ahead, p[2] + cosf(a) * ahead)) return;
    *(float *)(uintptr_t)(gohma + 0x15e4) = 0;
    *(int16_t *)(uintptr_t)(gohma + 0x15a4) = *(int16_t *)(uintptr_t)(gohma + 0x15a6);
    if (ENV("LARECOMP_LOG_ROAM")) LOG("[roam] Gohma at %.1f %.1f: wall ahead, move ends\n", p[0], p[2]);
}

// Cue Ball on the Field rolls the loop of four corners fitted to its screen (enemies.py fit_rail): state 2 rolls to
// its first corner, state 7 to the next, state 8 turns there - the next corner one step along its direction (+0x1731: 0 forward, else
// back; corners at +0x1410, the one it heads for +0x1760). Here something in its way (a bush) would hold it forever:
// stuck a moment, or now and then at random, it turns back - its own turn with its direction flipped.
static void set_state(Cpu *c, const Seen *s, const Kind *k, int state) {  // (its own state change)
    uint64_t saved[31], sp = c->sp;
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = s->obj + k->sm;
    c->x[1] = (uint64_t)state;
    guest_call(c, k->chg);
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp;
}

// Stuck a moment (moved under 0.02 a frame for 20 frames) or due for a change (every 3 - 9 s): tracked per mini-boss.
static bool stuck_or_due(Seen *s, const float *p, bool moving, uint64_t frame) {
    float dx = p[0] - s->px, dz = p[2] - s->pz;
    s->px = p[0], s->pz = p[2];
    if (!s->flip_at) s->flip_at = frame + 180 + (uint32_t)(s->obj >> 4) % 360;
    if (!moving) { s->stuck = 0; return false; }
    s->stuck = dx * dx + dz * dz < 0.0004f ? s->stuck + 1 : 0;
    if (s->stuck < 20 && frame < s->flip_at) return false;
    s->stuck = 0;
    s->flip_at = frame + 180 + (uint32_t)(frame * 2654435761u >> 8) % 360;
    return true;
}

// Avalaunch rolls out from its spawn point on a random spoke (state 2) and back to it (state 3). Now and then - or
// stuck - it switches: out turns back home early, home heads out on a new spoke.
static void avalaunch_tick(Cpu *c, Seen *s, const Kind *k, uint64_t frame) {
    uint64_t comp = LD64(s->obj + 0x80);
    if (!comp) return;
    const float *p = (const float *)(uintptr_t)(comp + 0x80);
    uint8_t st = *(uint8_t *)(uintptr_t)(s->obj + k->sm + 0xd);
    if (!stuck_or_due(s, p, st == 2 || st == 3, frame)) return;
    set_state(c, s, k, st == 2 ? 3 : 2);
    if (ENV("LARECOMP_LOG_ROAM")) LOG("[roam] Avalaunch at %.1f %.1f switches (%s)\n", p[0], p[2], st == 2 ? "home" : "out");
}

static void cueball_tick(Cpu *c, Seen *s, const Kind *k, uint64_t frame) {
    uint64_t comp = LD64(s->obj + 0x80);
    if (!comp) return;
    const float *p = (const float *)(uintptr_t)(comp + 0x80);
    uint8_t st = *(uint8_t *)(uintptr_t)(s->obj + k->sm + 0xd);
    // (2: rolling to its first corner - only stuck turns it there; 7: to the next corner)
    bool due_ok = st == 7;
    int was_stuck = s->stuck;
    if (!stuck_or_due(s, p, st == 7 || st == 2, frame)) return;
    if (!due_ok && was_stuck < 19) { s->flip_at = 0; return; }  // (state 2: a random turn waits for the loop)
    *(uint8_t *)(uintptr_t)(s->obj + 0x1731) ^= 1;
    set_state(c, s, k, 8);
    if (ENV("LARECOMP_LOG_ROAM")) LOG("[roam] Cue Ball at %.1f %.1f turns back\n", p[0], p[2]);
}

static bool is_copy(uint64_t o, const Kind *k) {  // (a copy keeps the level name of the enemy it replaced)
    return !k->own || strncmp((const char *)(uintptr_t)(o + 0x18), k->own, strlen(k->own));
}

// Blaino's knock-out (tools/patches.py CRASHOUT_SITE): a copy's Link just falls back down - no fade, no restart warp
// (its start only exists in his ring): on the Field, and in dungeons, caves and houses unless the original Blaino (in
// his own ring: Turtle Rock 03H) is about. (The countdown's own reference to the puncher, +0xc8, is already cleared
// here.) Copies are never placed in a room with a mini-boss of its own (enemies.py ARENA_TYPES).
int rt_crashout_skip(uint64_t o) {
    (void)o;
    extern const char *rando_seed_dir(void);
    if (rt_gohma_free()) return 1;
    uint64_t ps = LD64(GA_PLAYER_STATUS_PTR);
    if (!rando_seed_dir() || !ps || !strncmp((const char *)(uintptr_t)ps, "Panel", 5)) return 0;
    uint64_t f = renderer_present_count();
    for (int i = 0; i < MAX_SEEN; i++)
        if (g_seen[i].obj && f - g_seen[i].last <= 2 && LD64(g_seen[i].obj) == GA_BLAINO_VTABLE &&
            !is_copy(g_seen[i].obj, &g_kinds[g_seen[i].kind]))
            return 0;
    LOG("[miniboss] Blaino knock-out: a copy's, no crash-out\n");
    return 1;
}

// Shadow Link on the Field (a chamber mini-boss: "Include mini-bosses"). As a shadow he follows Link; within 3 units
// his own code requests his Appear event (main+0xd157f0: ShadowLink.bfevfl - the camera looks at him, his Sign glow,
// PopStart) and marks it at +0x1703 (requested) / +0x1704 (running); once +0x1704 is cleared again (the event's end)
// he pops up. An event stops Link and takes the camera, so in seeds the request itself is skipped - on the Field (a
// mini-boss) and in dungeons, caves and houses (a plain enemy: enemies.yml, Include mini-bosses), everywhere but his
// own Dampe's chamber levels (Panel*) - (tools/patches.py SHADOW_LINK_APPEAR_SITE): the marks are set as it would, his
// Sign glow plays here (his tailor at [+0x16c0]+0x58, channel "Other", as his own code plays its effects) and, after
// the event's own timing, +0x1704 is cleared - he pops up with Link still in control and the camera on Link.
#define MAX_SL 32  // (several on a screen with frequency All)
static struct { uint64_t obj, last, at; } g_sl[MAX_SL];
RT_ON_RESET(shadow_link_reset) { memset(g_sl, 0, sizeof g_sl); }
#define SL_REQUESTED 0x1703
#define SL_RUNNING 0x1704

static void shadow_link_seen(uint64_t o) {
    for (int i = 0; i < MAX_SL; i++)
        if (g_sl[i].obj == o) g_sl[i].last = renderer_present_count();
}

int rt_shadow_link_appear(uint64_t o) {  // (SHADOW_LINK_APPEAR_SITE: 1 = the request is skipped)
    extern const char *rando_seed_dir(void);
    uint64_t ps = LD64(GA_PLAYER_STATUS_PTR);
    if (!rando_seed_dir() || !ps || !strncmp((const char *)(uintptr_t)ps, "Panel", 5)) return 0;  // (Dampe's chamber: as is)
    uint64_t f = renderer_present_count();
    int slot = -1;
    for (int i = 0; i < MAX_SL && slot < 0; i++)
        if (g_sl[i].obj == o || !g_sl[i].obj || f - g_sl[i].last > 120) slot = i;
    if (slot < 0) slot = 0;
    g_sl[slot].obj = o, g_sl[slot].last = f, g_sl[slot].at = f;
    ST8(o + SL_REQUESTED, 1), ST8(o + SL_RUNNING, 1);
    LOG("[miniboss] f%llu Shadow Link %llx: appears (no event)\n", (unsigned long long)f, (unsigned long long)o);
    return 1;
}

static void shadow_link_sign(Cpu *c, uint64_t o) {
    static uint64_t mem;  // ({"Other", 5}, {"Sign", 4}: the tailor's channel and animation names)
    if (!mem) mem = (uint64_t)(uintptr_t)hle_alloc(0x20);
    uint64_t holder = LD64(o + 0x16c0), t = holder ? LD64(holder + 0x58) : 0;
    if (!t) return;
    ST64(mem, GA_STRING_OTHER), ST64(mem + 8, 5), ST64(mem + 0x10, GA_STRING_SIGN), ST64(mem + 0x18, 4);
    uint64_t saved[31], sp = c->sp;
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = t, c->x[1] = mem, c->x[2] = mem + 0x10;
    guest_call(c, GA_TAILOR_PREPARE);
    c->x[0] = LD64(holder + 0x58), c->x[1] = mem, c->x[2] = mem + 0x10;
    guest_call(c, GA_TAILOR_PLAY);
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp;
}

static void shadow_link_tick(Cpu *c, uint64_t frame) {
    for (int i = 0; i < MAX_SL; i++) {
        uint64_t o = g_sl[i].obj;
        if (!o) continue;
        if (frame - g_sl[i].last > 2 || LD64(o) != GA_SHADOW_LINK_VTABLE || !LD8(o + SL_RUNNING)) {
            if (frame - g_sl[i].last > 120 || (LD64(o) == GA_SHADOW_LINK_VTABLE && !LD8(o + SL_RUNNING)))
                memset(&g_sl[i], 0, sizeof g_sl[i]);  // (up, or gone)
            continue;
        }
        uint64_t t = frame - g_sl[i].at;  // (the Appear event's timing: Sign after 0.5 s, up 1.25 s later)
        if (t == 30) shadow_link_sign(c, o);
        if (t >= 105) ST8(o + SL_RUNNING, 0);
    }
}

void miniboss_tick(Cpu *c, uint64_t frame) {
    if (ENV("LARECOMP_MB_SCAN") || ENV("LARECOMP_MB_SCAN_NAME")) scan_tick(frame);
    force_tick(c, frame);
    bot_tick(frame);
    life_tick(frame);
    kill_tick(c, frame);
    extern const char *rando_seed_dir(void);
    if (!rando_seed_dir()) return;  // (only seeds put mini-bosses on the Field)
    shadow_link_tick(c, frame);  // (any level: Shadow Link is a plain enemy in dungeons too)
    miniboss_marker_set();       // (any level: his copies there have the marker on switch 2 too)
    uint64_t ps = LD64(GA_PLAYER_STATUS_PTR);
    if (!ps || !strncmp((const char *)(uintptr_t)ps, "Panel", 5)) return;  // (Dampe's chamber: as is)
    bool field = !strcmp((const char *)(uintptr_t)ps, "Field");  // (elsewhere: only copies of kinds with `own`)
    uint64_t pc = doorjump_player(frame);
    if (!pc) return;
    const float *lp = (const float *)(uintptr_t)(pc + 0x80);
    memcpy(g_link, lp, sizeof g_link), g_have_link = true;
    for (int i = 0; i < MAX_SEEN; i++) {
        Seen *s = &g_seen[i];
        if (!s->obj || frame - s->last > 2 || frame - s->first < 30 || LD64(s->obj) != g_kinds[s->kind].vt) continue;
        const Kind *k = &g_kinds[s->kind];
        if (!field && (!k->own || !is_copy(s->obj, k))) continue;  // (dungeon arenas start their own)
        if (!strcmp(k->name, "Gohma")) gohma_walls(s->obj);
        if (!strcmp(k->name, "Cue Ball")) cueball_tick(c, s, k, frame);
        if (!strcmp(k->name, "Avalaunch")) avalaunch_tick(c, s, k, frame);
        uint8_t st = *(uint8_t *)(uintptr_t)(s->obj + k->sm + 0xd);
        if (k->then && s->woken && st == k->fight && frame - s->woken_at >= 2) set_state(c, s, k, k->then);
        if (ENV("LARECOMP_LOG_MB_Y") && frame % 5 == 0 && s->woken && LD64(s->obj + 0x80)) {  // (RE: height by state)
            const float *q = (const float *)(uintptr_t)(LD64(s->obj + 0x80) + 0x80);
            LOG("[mb-y] f%llu %s %llx st %u at %.2f %.2f %.2f\n", (unsigned long long)frame, k->name,
                (unsigned long long)s->obj, st, q[0], q[1], q[2]);
        }
        // first wake from idle; again from its rest state (a while after the last wake: it went there itself)
        bool again = s->woken && k->rest != 0xff && st == k->rest && frame - s->woken_at > 180;
        if (s->woken ? !again : st != k->idle) continue;
        const float *p = (const float *)(uintptr_t)(s->obj + k->pos);
        // (Yarna Desert's own Lanmola, Field_13O at 217.5, 150: his screen's event starts him)
        if (!strcmp(k->name, "Lanmola") && fabsf(p[0] - 217.5f) < 1.5f && fabsf(p[2] - 150.0f) < 1.5f) continue;
        float dx = p[0] - lp[0], dz = p[2] - lp[2];
        if (!k->at_once && dx * dx + dz * dz > WAKE_DIST * WAKE_DIST) continue;
        s->woken = 1;
        s->woken_at = frame;
        uint64_t saved[31], sp = c->sp;
        memcpy(saved, c->x, sizeof saved);
        if (k->start) {
            c->x[0] = s->obj;
            guest_call(c, MAIN_BASE + k->start);
        } else {
            c->x[0] = s->obj + k->sm;
            c->x[1] = k->fight;
            guest_call(c, k->chg);
        }
        memcpy(c->x, saved, sizeof saved);
        c->sp = sp;
        LOG("[miniboss] f%llu %s woken (Link %.1f away; it stood at %.1f %.1f)\n", (unsigned long long)frame, k->name,
            sqrtf(dx * dx + dz * dz), p[0], p[2]);
    }
}
