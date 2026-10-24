// Boss souls / mini-boss souls (randomizer options "Boss souls" / "Mini-boss souls", tools/rando/souls.py): a dungeon
// boss or mini-boss only appears once its soul has been found. Souls are items that set an event flag;
// rando/<seed>/souls.tsv (no file: both options off) lists
//   <dungeon> <type> <flag> <name> <reward key>   a boss (0 = Color Dungeon) and its reward actor (instrument / fairy)
//   mini <type> <flag> <name>                     a mini-boss kind (every encounter of it)
//   room <level> <soul flag> <defeat flag> <cells>  a room whose shutters stay open without its enemy
//   door <key> <c,r> <N|S|W|E> <flag>             one of that room's shutters (the last room line's)
// The actor factory asks rt_actor_create_fn for a type's create function (tools/patches.py BOSS_SPAWN_SITE); for a
// held-back boss / mini-boss / reward we hand back none - the factory's own path for a type without a class. Rooms
// create their actors as they stream in, so a soul found inside the dungeon counts once the room is loaded again.
//
// An empty room's shutters would stay open (they follow the enemy), so the way on would be free: souls_tick keeps Link
// in such a room unless he leaves through the doorway he came in by, until the room's defeat flag is set (vanilla: the
// shutters open on that same flag) or the soul is found. Entered by falling / stairs / a warp (not at a doorway): no
// wall (he can't go back the way he came).
// The shutters themselves are the game's fight-room doors at seed time (souls.py hold_shutters, as Tail Cave 07C's:
// open while Link is outside, shut behind him as he comes in - an AreaEventBox sets a room flag -, open once their
// flag is set), each on a door flag of its own driven here (door_flags): the room's defeat flag when the enemy is
// there (its soul was found before the level loaded: the vanilla fight), and while it's held back also open for the
// doorway he came in by.
#include "../game_addresses.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../rando.h"

// GA(PLAYER_STATUS_PTR): PlayerStatus*: current level name at +0

bool flags_ext_get(int idx);
void flags_ext_set(int idx);
void flags_ext_clear(int idx);
uint64_t doorjump_player(uint64_t frame);  // PlayerCollision component: position at +0x80 (and +0xb0)

typedef struct { int kind, dungeon; uint32_t type; int flag; char name[32]; unsigned long long reward; } Soul;  // kind 0 boss, 1 mini
typedef struct { char level[32]; int soul_flag, defeat_flag, ncells; int8_t cells[8][2]; bool present; } Wall;
typedef struct { int wall, flag; int8_t c, r, side; } Door;  // side: 0 N 1 S 2 W 3 E
static Soul g_souls[32];
static int g_nsouls = -1;
static Wall g_walls[48];
static int g_nwalls;
static Door g_doors[160];
static int g_ndoors;
typedef struct { char level[32]; float x, y, z; int kind; } Spot;  // a soul's item model standing in the world
static Spot g_spots[32];
static int g_nspots;

static void load(void) {
    g_nsouls = g_nwalls = g_nspots = g_ndoors = 0;
    const char *dir = rando_seed_dir();
    if (!dir) return;
    char p[1024], line[256];
    snprintf(p, sizeof p, "%s/souls.tsv", dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        Soul *s = &g_souls[g_nsouls];
        Wall *w = &g_walls[g_nwalls];
        char cells[160];
        Spot *sp = &g_spots[g_nspots];
        if (!strncmp(line, "zone\t", 5)) {  // a level whose every fairy is a soul's model (shop shelf, crane floor)
            if (g_nspots < 32 && sscanf(line + 5, "%31[^\t]\t%d", sp->level, &sp->kind) == 2) {
                sp->x = NAN;
                g_nspots++;
            }
        } else if (!strncmp(line, "spot\t", 5)) {
            if (g_nspots < 32 && sscanf(line + 5, "%31[^\t]\t%f\t%f\t%f\t%d", sp->level, &sp->x, &sp->y, &sp->z, &sp->kind) == 5)
                g_nspots++;
        } else if (!strncmp(line, "door\t", 5)) {
            Door *d = &g_doors[g_ndoors];
            unsigned long long key;
            int c, r, flag;
            char side;
            if (g_ndoors < 160 && g_nwalls > 0 &&
                sscanf(line + 5, "%llx\t%d,%d\t%c\t%d", &key, &c, &r, &side, &flag) == 5 && strchr("NSWE", side)) {
                d->wall = g_nwalls - 1, d->flag = flag, d->c = (int8_t)c, d->r = (int8_t)r;
                d->side = (int8_t)(strchr("NSWE", side) - "NSWE");
                g_ndoors++;
            }
        } else if (!strncmp(line, "room\t", 5)) {
            if (g_nwalls < 48 && sscanf(line + 5, "%31[^\t]\t%d\t%d\t%159s", w->level, &w->soul_flag, &w->defeat_flag, cells) == 4) {
                w->ncells = 0;
                for (char *c = strtok(cells, ";"); c && w->ncells < 8; c = strtok(NULL, ";")) {
                    int x, y;
                    if (sscanf(c, "%d,%d", &x, &y) == 2) w->cells[w->ncells][0] = (int8_t)x, w->cells[w->ncells++][1] = (int8_t)y;
                }
                g_nwalls++;
            }
        } else if (g_nsouls >= 32) {
            continue;
        } else if (!strncmp(line, "mini\t", 5)) {
            if (sscanf(line + 5, "%i\t%d\t%31[^\t\n]", (int *)&s->type, &s->flag, s->name) == 3) {
                s->kind = 1, s->dungeon = -1, s->reward = 0;
                g_nsouls++;
            }
        } else if (sscanf(line, "%d\t%i\t%d\t%31[^\t]\t%llx", &s->dungeon, (int *)&s->type, &s->flag, s->name, &s->reward) == 5) {
            s->kind = 0;
            g_nsouls++;
        }
    }
    fclose(f);
    LOG("[souls] %d souls, %d walled rooms (%d shutters) in this seed\n", g_nsouls, g_nwalls, g_ndoors);
}

static const char *level_name(void) {
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
    return ps ? (const char *)(uintptr_t)ps : "";
}

uint64_t rt_actor_create_fn(uint64_t fn, uint32_t type, uint64_t info) {
    extern bool rt_digs_skip_plant(uint32_t type, uint64_t info);  // digsanity: no grass / bush on a dug tile
    if (rt_digs_skip_plant(type, info)) return 0;
    extern bool rt_bush_item_skip(uint32_t type, uint64_t info);  // bushsanity: no item until its bush is cut
    if (rt_bush_item_skip(type, info)) return 0;
    extern bool rt_shell_skip(uint32_t type, uint64_t info);  // Secret Seashell spots (shell_spots.c)
    if (rt_shell_skip(type, info)) return 0;
    extern bool rt_no_buff_actor(uint32_t type);  // no-buff-drops (qol.c): no Piece of Power / Guardian Acorn at all
    if (rt_no_buff_actor(type)) return 0;
    if (g_nsouls < 0) load();
    if (ENV("LARECOMP_LOG_SPAWN")) {  // (debug: every actor the factory makes; spawn info words)
        LOG("[spawn] type %#x:", type);
        for (int w = 0; w < 24; w++) LOG(" %llx", (unsigned long long)LD64(info + 8 * w));
        LOG("\n");
    }
    for (int i = 0; i < g_nsouls; i++) {
        Soul *s = &g_souls[i];
        bool reward = s->reward && LD64(info) == s->reward;  // (spawn info +0: the level actor's key)
        if (s->type != type && !reward) continue;
        // only in the dungeons: Dampe's chambers reuse boss rooms; overworld mini-bosses (enemy randomizer) stay
        if (strncmp(level_name(), "Lv", 2) || flags_ext_get(s->flag)) return fn;
        LOG("[souls] %s's soul not found: %s held back\n", s->name, reward ? "reward" : s->kind ? "mini-boss" : "boss");
        return 0;
    }
    return fn;
}

// ---- the wall: Link leaves a room with a held-back enemy only into the cell he came from
static bool in_wall(const Wall *w, int c, int r) {
    for (int i = 0; i < w->ncells; i++)
        if (w->cells[i][0] == c && w->cells[i][1] == r) return true;
    return false;
}

static struct { int wall; int from_c, from_r, side; bool way_back; } g_in = {-1};  // from: the cell he came in at
static float g_last[3];
static int g_last_c = -999, g_last_r = -999;
static char g_last_level[32];

// ---- the soul's look: a fairy (Items.gsheet npcKey ItemFairy) drawn evil red while one is being received. A soul
// flag that turns on starts a window (the hold-up and its message); only the fairy model map's draws are tinted
// (texpatch R_FAIRY_MODEL -> escape_bottle_tint kind 3 -> souls_fairy_tint), so real fairies stay pink outside it.
static int g_got_kind = -1;      // kind of the soul just received (0 boss, 1 mini-boss), -1 none
static uint64_t g_got_frame;     // (presented-frame count when it was received)
static uint8_t g_seen[32];       // soul flags as last seen
static char g_flag_level[32];    // level they were last read in (a new level / a loaded save: read again, no trigger)
extern uint64_t renderer_present_count(void);

static void watch_flags(void) {
    const char *lv = level_name();
    bool fresh = strncmp(g_flag_level, lv, sizeof g_flag_level - 1) != 0;
    if (fresh) strlcpy(g_flag_level, lv, sizeof g_flag_level);
    for (int i = 0; i < g_nsouls && i < 32; i++) {
        uint8_t now = flags_ext_get(g_souls[i].flag);
        if (!fresh && *lv && !g_seen[i] && now) {
            g_got_kind = g_souls[i].kind;
            g_got_frame = renderer_present_count();
            LOG("[souls] got %s's soul\n", g_souls[i].name);
        }
        g_seen[i] = now;
    }
}

// m: the draw's bone matrices (vertex cbuf 3: the first row-major 3x4, translation in m[3], m[7], m[11]) or NULL
void souls_fairy_tint(const float *m, uint32_t m_size, float tint[8]) {
    if (g_nsouls < 0) load();
    int kind = -1;
    if (ENV("LARECOMP_SOUL_TINT")) kind = atoi(ENV("LARECOMP_SOUL_TINT"));  // (test: 0 / 1 always on, -1 off)
    else if (g_got_kind >= 0 && renderer_present_count() - g_got_frame <= 600) kind = g_got_kind;  // being received
    else if (g_nspots) {  // standing in the world at a soul's spot (it bobs and turns), or in a soul zone
        const char *lv = level_name();
        for (int i = 0; i < g_nspots && kind < 0; i++)
            if (isnan(g_spots[i].x) && !strcmp(g_spots[i].level, lv)) kind = g_spots[i].kind;
    }
    if (kind < 0 && !ENV("LARECOMP_SOUL_TINT") && m && m_size >= 48 && g_nspots) {
        const char *lv = level_name();
        for (int i = 0; i < g_nspots && kind < 0; i++) {
            const Spot *sp = &g_spots[i];
            if (fabsf(sp->x - m[3]) < 1.0f && fabsf(sp->z - m[11]) < 1.0f && fabsf(sp->y - m[7]) < 2.5f &&
                !strcmp(sp->level, lv))
                kind = sp->kind;
        }
    }
    if (kind < 0) return;
    // ltint: colour.rgb + mix, glow.rgb + luminance power (translate.c): boss blood red, mini-boss red-orange ember
    static const float boss[8] = {1.55f, 0.04f, 0.05f, 1.0f, 0.35f, 0.0f, 0.02f, 1.25f};
    static const float mini[8] = {1.60f, 0.30f, 0.02f, 1.0f, 0.30f, 0.06f, 0.0f, 1.25f};
    memcpy(tint, kind ? mini : boss, sizeof boss);
}

static bool wall_open(const Wall *w) { return flags_ext_get(w->defeat_flag) || flags_ext_get(w->soul_flag); }

// the door flags. A fight-room door opened by its flag stays open for the rest of the level visit (cleared again it
// shows shut but lets Link through), so flags are only set while he is in the level and cleared once he has left it
// (`here`: the level he is in, NULL = none). Set: the room's defeat flag (with the enemy there: the vanilla fight);
// with the enemy held back also the doorway Link came in by (all of them: entered by a fall / stairs, soul found)
static void door_flags(const char *here) {
    for (int i = 0; i < g_ndoors; i++) {
        const Door *d = &g_doors[i];
        const Wall *w = &g_walls[d->wall];
        bool open = flags_ext_get(w->defeat_flag), set = flags_ext_get(d->flag);
        if (!here || strcmp(w->level, here)) {  // (not loaded: shut again for the next visit)
            if (set && !open) {
                flags_ext_clear(d->flag);
                if (ENV("LARECOMP_LOG_SOULS")) LOG("[souls] door flag %d reset\n", d->flag);
            }
            continue;
        }
        if (!open && !w->present && g_in.wall == d->wall)
            open = !g_in.way_back || flags_ext_get(w->soul_flag) ||
                   (d->c == g_in.from_c && d->r == g_in.from_r && d->side == g_in.side);
        if (open && !set) {
            flags_ext_set(d->flag);
            if (ENV("LARECOMP_LOG_SOULS"))
                LOG("[souls] door flag %d (%d,%d %c) open\n", d->flag, d->c, d->r, "NSWE"[(int)d->side]);
        }
    }
}

void souls_tick(uint64_t frame) {
    if (g_nsouls < 0) load();
    if (g_nsouls > 0) watch_flags();
    if (!g_nwalls) return;
    const char *lv = level_name();
    if (strncmp(lv, "Lv", 2)) {  // (back in a dungeon later: a new load, its enemies made again)
        g_last_level[0] = 0;
        if (g_ndoors) door_flags(NULL);
        return;
    }
    uint64_t pc = doorjump_player(frame);
    if (!pc) return;
    float *pos = (float *)(uintptr_t)(pc + 0x80), *pos2 = (float *)(uintptr_t)(pc + 0xb0);
    if (strncmp(g_last_level, lv, sizeof g_last_level - 1)) {  // new level: no history; which enemies it made
        strlcpy(g_last_level, lv, sizeof g_last_level);
        g_in.wall = -1;
        g_last_c = -999;
        for (int i = 0; i < g_nwalls; i++) g_walls[i].present = flags_ext_get(g_walls[i].soul_flag);
    }
    int c = (int)floorf(pos[0] / 15.0f), r = (int)floorf(pos[2] / 12.0f);
    int now = -1;
    for (int i = 0; i < g_nwalls && now < 0; i++)
        if (!strcmp(g_walls[i].level, lv) && in_wall(&g_walls[i], c, r)) now = i;
    if (g_in.wall >= 0 && now != g_in.wall) g_in.wall = -1;  // left (the way back, a fall, a warp)
    if (now >= 0 && g_in.wall != now) {  // entering: the doorway he came in by (the cell edge he stands at)
        g_in.wall = now;
        g_in.from_c = c, g_in.from_r = r;
        float dx = pos[0] - c * 15.0f, dz = pos[2] - r * 12.0f;
        float edge[4] = {dz, 12.0f - dz, dx, 15.0f - dx};  // N S W E
        int sd = 0;
        for (int k = 1; k < 4; k++)
            if (edge[k] < edge[sd]) sd = k;
        g_in.side = sd;
        // (at a doorway: from a neighbour or a door jump; not: a fall, stairs, a warp)
        g_in.way_back = edge[sd] < 3.0f;
        const Wall *w = &g_walls[now];
        if (!wall_open(w))
            LOG("[souls] f%llu entered a walled room (%d,%d) from (%d,%d) by its %c side%s\n", (unsigned long long)frame, c,
                r, g_last_c, g_last_r, "NSWE"[sd], g_in.way_back ? "" : ": not at a doorway, no wall");
    }
    door_flags(lv);
    if (g_in.wall >= 0 && g_in.way_back) {  // inside: keep him off every wall but the doorway he came in by
        const Wall *w = &g_walls[g_in.wall];
        if (!wall_open(w)) {
            // (a margin short of the doorway: once he steps into it the game takes him through to the next room)
            const float M = 1.6f;
            float x = pos[0], z = pos[2];
            for (int i = 0; i < w->ncells; i++) {
                if (w->cells[i][0] != c || w->cells[i][1] != r) continue;
                float x0 = c * 15.0f, z0 = r * 12.0f;
                static const int8_t step[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};  // N S W E
                for (int sd = 0; sd < 4; sd++) {
                    int nc = c + step[sd][0], nr = r + step[sd][1];
                    if (in_wall(w, nc, nr) || (c == g_in.from_c && r == g_in.from_r && sd == g_in.side)) continue;
                    if (sd == 0 && z < z0 + M) z = z0 + M;
                    if (sd == 1 && z > z0 + 12.0f - M) z = z0 + 12.0f - M;
                    if (sd == 2 && x < x0 + M) x = x0 + M;
                    if (sd == 3 && x > x0 + 15.0f - M) x = x0 + 15.0f - M;
                }
            }
            if (x != pos[0] || z != pos[2]) {
                pos[0] = pos2[0] = x;
                pos[2] = pos2[2] = z;
                if (ENV("LARECOMP_LOG_SOULS")) LOG("[souls] f%llu wall at (%.2f, %.2f)\n", (unsigned long long)frame, x, z);
            }
        }
    }
    g_last[0] = pos[0], g_last[1] = pos[1], g_last[2] = pos[2];
    g_last_c = c, g_last_r = r;
}

// for the inventory overlay: -1 = the seed has no boss souls, else whether dungeon n's (0 = Color Dungeon) is found
int souls_owned(int dungeon) {
    if (g_nsouls < 0) load();
    for (int i = 0; i < g_nsouls; i++)
        if (!g_souls[i].kind && g_souls[i].dungeon == dungeon) return flags_ext_get(g_souls[i].flag);
    return -1;
}

// every soul of the seed (overlay, tracker): name of soul i (NULL past the end), *kind 0 boss / 1 mini-boss, *owned
const char *souls_at(int i, int *kind, bool *owned) {
    if (g_nsouls < 0) load();
    if (i < 0 || i >= g_nsouls) return NULL;
    *kind = g_souls[i].kind;
    *owned = flags_ext_get(g_souls[i].flag);
    return g_souls[i].name;
}

RT_ON_RESET(souls_reset) {  // (seed switch: reload rando/<seed>/souls.tsv)
    g_got_kind = -1;
    g_flag_level[0] = 0;
    g_nspots = 0;
    g_nsouls = -1;
    g_nwalls = 0;
    g_ndoors = 0;
    g_in.wall = -1;
    g_last_c = g_last_r = -999;
    g_last_level[0] = 0;
}
