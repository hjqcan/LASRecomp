// Quick pickups: a randomizer item picked up from the ground (a SinkingSword item actor: pots, bushes, digs, Secret
// Seashell spots) runs its entry in SinkingSword.bfevfl - and any event the game starts holds Link still while it
// runs: running over a bush's rupee, he stopped dead for 2-3 frames every time (user, 2026-10-21; vanilla rupees and
// hearts are no event). The entries that only set their flag, play a system sound and add an item (rupees, recovery
// hearts, refills; with silent pickups every minor item) are listed in the seed's quick_pickups.tsv
// (tools/rando/quick_pickups.py); for those the runtime does the same steps itself and removes the item actor, and
// the event is never requested. Everything else (an item-get cutscene, a message, a trap) keeps its event.
//
// ACTOR_EVENT_SITE (tools/patches.py; main+0x94a568 in main+0x94a510, an actor's AI starting one of its events, right
// before it builds the event request): x20 = the AI, [AI + 0x58] = its actor, the entry name ($2 of a SinkingSword
// item actor: its location) resolved at [x29 - 0x40] (pointer, length). The skip target is the function's no-event
// return.
// LARECOMP_QUICK_PICKUPS=0 (test): every pickup an event again, as before.
#include "../game_addresses.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../rando.h"

typedef struct { char entry[48]; int flag, item, count, index; char se[40]; } Quick;
static Quick *g_q;
static int g_nq;
static char g_seed[512];
static struct { uint64_t actor, frame; } g_done[8];  // actors given here lately (a repeat request: nothing again)
static unsigned g_done_at;

RT_ON_RESET(quick_pickup_reset) {
    memset(g_done, 0, sizeof g_done);
}

static void load(const char *dir) {
    snprintf(g_seed, sizeof g_seed, "%s", dir);
    free(g_q);
    g_q = NULL, g_nq = 0;
    char p[600];
    snprintf(p, sizeof p, "%s/quick_pickups.tsv", dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    int cap = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        Quick q;
        if (sscanf(line, "%47[^\t]\t%d\t%d\t%d\t%d\t%39s", q.entry, &q.flag, &q.item, &q.count, &q.index, q.se) != 6) continue;
        if (q.se[0] == '-') q.se[0] = 0;
        if (g_nq == cap) g_q = realloc(g_q, sizeof *g_q * (size_t)(cap = cap ? cap * 2 : 512));
        g_q[g_nq++] = q;
    }
    fclose(f);
    LOG("[quick] %d quick pickups in %s\n", g_nq, p);
}

static const Quick *find(const char *name, size_t len) {
    const char *dir = rando_seed_dir();
    if (!dir) return NULL;
    if (strcmp(g_seed, dir)) load(dir);
    for (int i = 0; i < g_nq; i++)
        if (strlen(g_q[i].entry) == len && !memcmp(g_q[i].entry, name, len)) return &g_q[i];
    return NULL;
}

// ACTOR_EVENT_SITE (hooks.c): 0 = request the event as the game does, 2 = no event (given here)
int rt_actor_event(Cpu *c) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_QUICK_PICKUPS") && !atoi(getenv("LARECOMP_QUICK_PICKUPS"));
    if (off) return 0;
    uint64_t actor = LD64(c->x[20] + 0x58);
    if (!actor || LD64(actor) != GA(SINKING_SWORD_VTABLE)) return 0;
    uint64_t name = LD64(c->x[29] - 0x40), len = LD64(c->x[29] - 0x38);
    if (!name || !len || len > 47) return 0;
    const Quick *q = find((const char *)(uintptr_t)name, (size_t)len);
    if (!q) return 0;
    extern uint64_t renderer_present_count(void);
    uint64_t frame = renderer_present_count();
    for (unsigned i = 0; i < sizeof g_done / sizeof g_done[0]; i++)
        if (g_done[i].actor == actor && frame - g_done[i].frame < 120) return 2;  // (given; it goes with this frame)
    unsigned slot = g_done_at++ % (sizeof g_done / sizeof g_done[0]);
    g_done[slot].actor = actor, g_done[slot].frame = frame;

    Cpu saved = *c;  // (the game's registers as they were: the skip returns from the function)
    if (q->flag >= 0) {  // EventFlags.SetFlag
        extern void flags_put(int idx, bool on);
        flags_put(q->flag, true);
    }
    if (q->se[0]) {  // Audio.PlayOneshotSystemSE (volume 1, pitch 1): main+0xa57e0(&{label, length}, 0, s0, s1)
        c->sp -= 0x60;
        uint64_t ref = c->sp, s = c->sp + 0x10;
        size_t n = strlen(q->se);
        memcpy((void *)(uintptr_t)s, q->se, n + 1);
        ST64(ref, s);
        ST64(ref + 8, n);
        float one = 1.0f;
        c->v[0] = c->v[1] = 0;
        memcpy(&c->v[0], &one, 4);
        memcpy(&c->v[1], &one, 4);
        c->x[0] = ref, c->x[1] = 0;
        guest_call(c, GA(PLAY_SYSTEM_SE));
        c->sp = saved.sp;
    }
    uint32_t rupees = LD32(GA(LIVE_SAVE) + 0x5540);
    int16_t hp = (int16_t)LD16(GA(LIVE_SAVE) + 0x5544);
    c->x[0] = (uint64_t)q->item, c->x[1] = (uint64_t)q->count, c->x[2] = (uint64_t)(int64_t)q->index, c->x[3] = 0;
    guest_call(c, GA(ADD_ITEM));  // Inventory.AddItemByKey (through the AddItem hook: refills, silent pickups' cards)
    c->x[0] = actor;
    guest_call(c, GA(ACTOR_KILL));  // SinkingSword.Destroy
    *c = saved;
    LOG("[quick] f%llu %.*s: item %d x%d (index %d), flag %d%s%s - no event (rupees %u -> %u, health %d -> %d)\n",
        (unsigned long long)frame, (int)len, (const char *)(uintptr_t)name, q->item, q->count, q->index, q->flag,
        q->se[0] ? ", sound " : "", q->se, rupees, LD32(GA(LIVE_SAVE) + 0x5540), hp, (int16_t)LD16(GA(LIVE_SAVE) + 0x5544));
    return 2;
}
