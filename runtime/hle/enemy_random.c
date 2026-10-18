// Enemy randomizer, mode Random: enemies change every time an area loads, instead of staying as the seed's level files
// have them (seeds with <seed>/enemies_random.tsv, written by tools/rando/enemy_random.py; Seeded seeds have none).
// Only the actors the seeded roll randomizes are listed - never an Excluded_Actors entry, never an enemy a kill-all
// trigger watches (TagHolocaust / TagPopChecker: shutters, chests, stairs) or that stands in a room with a shutter no
// switch drives, never a mini-boss (they stay where the seed put them) - so every check and every way on is as in a
// Seeded seed. Each roll follows LAS enemies.py: the terrain class of the vanilla actor, the room's restrictions, 2D
// rooms, one fly-bomb / blocking group per room, Stretchy Ghost / orb colours, the enemy table's parameters.
//  - file loads (runtime/services/fs.c -> enemy_random_filter): a read of a listed room file from its first byte rolls
//    its actor records before the bytes reach the game (type, parameters, rotation, scale, enemy flag);
//  - spawns (rt_enemy_spawn): the game keeps recently loaded files, so a level entered again, an overworld area
//    streamed back in or an enemy that comes back makes its actors without a new read - each actor made again gets
//    a new pick of its own (type and parameters).
// A dungeon, house or cave makes all its actors as Link enters it, the overworld as its areas load: enemies change
// with each visit there and whenever the game makes an enemy again.
// LARECOMP_ENEMY_RANDOM=0 turns it off (the seed's own roll), LARECOMP_ENEMY_RANDOM_SEED=<n> makes the rolls repeatable.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "../game_addresses.h"
#include "../hle.h"
#include "../rando.h"

extern uint64_t renderer_present_count(void);

enum { NCLS = 8, MAXPICK = 64, MAXENEMY = 160, MAXFILE = 1024, MAXSLOT = 4096, REC = 0x88 };
static const char *const kCls[NCLS] = {"land", "air", "air_field", "water", "water2D", "water-shallow", "tree", "hole"};
enum { CLS_HOLE = 7 };

typedef struct { uint8_t kind; uint8_t n; int32_t v[8]; uint8_t empty[8]; } Param;  // kind: 0 empty 1 int 2 float 3 choice
typedef struct { int id; uint8_t e; Param p[8]; } Enemy;
typedef struct {
    uint32_t off, empty;
    uint16_t seed_type, vtype;
    uint8_t cls, ve, vp_ok;
    uint8_t fresh;     // (rolled by a file load, not spawned since: its next spawn takes that roll)
    float vrot, vscale[3];
    Param vp[8];
    uint64_t key;      // (the record's first 8 bytes)
    uint8_t rec[REC];  // the record as the latest roll has it
} Slot;
typedef struct {
    char path[128];
    const char *ovp;      // fs.c's host path of the seed file
    int restr[16], nrestr, first, n;
    uint8_t rolled;
    char *host;           // the seed's file
} RFile;

static int g_pick[NCLS][MAXPICK], g_npick[NCLS];
static int g_no2d[16], g_nno2d, g_fly[8], g_nfly, g_block[16], g_nblock;
static Enemy g_enemy[MAXENEMY];
static int g_nenemy, g_sizes;
static RFile *g_files;
static Slot *g_slots;
static int g_nfiles, g_nslots;
static int g_loaded;  // 0 not yet, 1 loaded (maybe empty: off)
static char g_dir[512];
static uint64_t g_rng;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int *g_keyidx;  // open addressing: slot index + 1, by key (rt_enemy_spawn)
static uint64_t *g_keys;
static int g_keycap;

RT_ON_RESET(enemy_random_reset) {
    for (int i = 0; i < g_nfiles; i++) free(g_files[i].host);
    free(g_files), free(g_slots), free(g_keyidx), free(g_keys);
    g_keyidx = NULL, g_keys = NULL, g_keycap = 0;
    g_files = NULL, g_slots = NULL;
    g_nfiles = g_nslots = g_nenemy = g_sizes = g_loaded = 0;
    g_dir[0] = 0;
}

static uint64_t rnd(void) {  // splitmix64
    uint64_t z = (g_rng += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static int rnd_below(int n) { return n > 0 ? (int)(rnd() % (uint64_t)n) : 0; }

static bool same_seed(void) {  // (the seed the table was loaded for is still the running one)
    const char *dir = rando_seed_dir();
    return dir ? !strcmp(dir, g_dir) : !g_dir[0];
}

static int ids(char *s, int *out, int cap) {  // "0x1 0x2 ..." -> count
    int n = 0;
    for (char *t = strtok(s, " \t\n"); t && n < cap; t = strtok(NULL, " \t\n"))
        if (strcmp(t, "-")) out[n++] = (int)strtol(t, NULL, 0);
    return n;
}

static void parse_param(const char *t, Param *p) {
    memset(p, 0, sizeof *p);
    if (!strncmp(t, "i:", 2)) p->kind = 1, p->v[0] = (int32_t)strtol(t + 2, NULL, 0);
    else if (!strncmp(t, "f:", 2)) { float f = strtof(t + 2, NULL); p->kind = 2; memcpy(&p->v[0], &f, 4); }
    else if (!strncmp(t, "c:", 2)) {
        p->kind = 3;
        for (const char *q = t + 2; *q && p->n < 8;) {
            if (*q == '-') p->empty[p->n] = 1, q++;
            else p->v[p->n] = (int32_t)strtol(q, (char **)&q, 0);
            p->n++;
            if (*q == ',') q++;
            else break;
        }
    }
}

static int parse_params(char *s, Param *out) {  // 8 space-separated specs; "S" -> 0
    int n = 0;
    for (char *t = strtok(s, " \t\n"); t && n < 8; t = strtok(NULL, " \t\n")) {
        if (!strcmp(t, "S")) return 0;
        parse_param(t, &out[n++]);
    }
    return n == 8;
}

static void load(void) {
    const char *dir = rando_seed_dir();
    g_loaded = 1;
    if (!dir) return;
    strlcpy(g_dir, dir, sizeof g_dir);
    const char *e = ENV("LARECOMP_ENEMY_RANDOM");
    if (e && !strcmp(e, "0")) { LOG("[enemyrand] off (LARECOMP_ENEMY_RANDOM=0)\n"); return; }
    char p[600];
    snprintf(p, sizeof p, "%s/enemies_random.tsv", dir);
    FILE *f = fopen(p, "r");
    if (!f) return;  // (a Seeded seed)
    g_files = calloc(MAXFILE, sizeof *g_files);
    g_slots = calloc(MAXSLOT, sizeof *g_slots);
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (line[0] == '#' || !tab) continue;
        *tab = 0;
        char *rest = tab + 1;
        if (!strcmp(line, "pick")) {
            char *t2 = strchr(rest, '\t');
            if (!t2) continue;
            *t2 = 0;
            for (int c = 0; c < NCLS; c++)
                if (!strcmp(rest, kCls[c])) g_npick[c] = ids(t2 + 1, g_pick[c], MAXPICK);
        } else if (!strcmp(line, "no2d")) {
            g_nno2d = ids(rest, g_no2d, 16);
        } else if (!strcmp(line, "group")) {
            char *t2 = strchr(rest, '\t');
            if (!t2) continue;
            *t2 = 0;
            if (!strcmp(rest, "fly")) g_nfly = ids(t2 + 1, g_fly, 8);
            else if (!strcmp(rest, "block")) g_nblock = ids(t2 + 1, g_block, 16);
        } else if (!strcmp(line, "sizes")) {
            g_sizes = atoi(rest);
        } else if (!strcmp(line, "enemy") && g_nenemy < MAXENEMY) {
            Enemy *en = &g_enemy[g_nenemy];
            char *t2 = strchr(rest, '\t'), *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
            if (!t3) continue;
            en->id = (int)strtol(rest, NULL, 0);
            en->e = (uint8_t)atoi(t2 + 1);
            if (parse_params(t3 + 1, en->p)) g_nenemy++;
        } else if (!strcmp(line, "file") && g_nfiles < MAXFILE) {
            char *t2 = strchr(rest, '\t');
            if (!t2) continue;
            RFile *rf = &g_files[g_nfiles++];
            *t2 = 0;
            strlcpy(rf->path, rest, sizeof rf->path);
            rf->nrestr = ids(t2 + 1, rf->restr, 16);
            rf->first = g_nslots;
        } else if (!strcmp(line, "slot") && g_nfiles && g_nslots < MAXSLOT) {
            // offset seed-type vanilla-type class empty ve rotY "sx sy sz" params
            char *f_[10] = {0};
            int k = 0;
            f_[k++] = rest;
            for (char *q = rest; *q && k < 9; q++)
                if (*q == '\t') *q = 0, f_[k++] = q + 1;
            if (k < 9) continue;
            Slot *s = &g_slots[g_nslots];
            memset(s, 0, sizeof *s);
            s->off = (uint32_t)strtoul(f_[0], NULL, 0);
            s->seed_type = (uint16_t)strtol(f_[1], NULL, 0);
            s->vtype = (uint16_t)strtol(f_[2], NULL, 0);
            s->cls = 0xff;
            for (int c = 0; c < NCLS; c++)
                if (!strcmp(f_[3], kCls[c])) s->cls = (uint8_t)c;
            if (s->cls == 0xff) continue;
            s->empty = (uint32_t)strtoul(f_[4], NULL, 0);
            s->ve = (uint8_t)atoi(f_[5]);
            s->vrot = strtof(f_[6], NULL);
            if (sscanf(f_[7], "%f %f %f", &s->vscale[0], &s->vscale[1], &s->vscale[2]) != 3) continue;
            s->vp_ok = (uint8_t)parse_params(f_[8], s->vp);
            g_nslots++;
            g_files[g_nfiles - 1].n++;
        }
    }
    fclose(f);
    for (int fi = 0; fi < g_nfiles; fi++) {  // (each record's key, from the seed's file)
        RFile *rf = &g_files[fi];
        char hp[800];
        snprintf(hp, sizeof hp, "%s/romfs/%s", dir, rf->path);
        rf->host = strdup(hp);
        int fd = open(hp, O_RDONLY);
        for (int si = 0; si < rf->n; si++)
            if (fd < 0 || pread(fd, &g_slots[rf->first + si].key, 8, g_slots[rf->first + si].off) != 8)
                g_slots[rf->first + si].key = 0;
        if (fd >= 0) close(fd);
    }
    const char *sd = ENV("LARECOMP_ENEMY_RANDOM_SEED");
    g_rng = sd ? strtoull(sd, NULL, 0) : ((uint64_t)time(NULL) << 20) ^ (uint64_t)(uintptr_t)&g_rng ^ (uint64_t)getpid();
    LOG("[enemyrand] Random mode: %d enemies in %d room files re-rolled at every load (%d enemy kinds)\n", g_nslots,
        g_nfiles, g_nenemy);
}

static const Enemy *enemy(int id) {
    for (int i = 0; i < g_nenemy; i++)
        if (g_enemy[i].id == id) return &g_enemy[i];
    return NULL;
}
static bool in(const int *a, int n, int v) {
    for (int i = 0; i < n; i++)
        if (a[i] == v) return true;
    return false;
}

static void put_params(uint8_t *rec, const Param *p, uint32_t empty) {
    for (int i = 0; i < 8; i++) {
        uint32_t v = 0, kind = 4;  // (4: string -> the empty one)
        Param q = p[i];
        if (q.kind == 3 && q.n) {  // one picked
            int k = rnd_below(q.n);
            q.kind = q.empty[k] ? 0 : 1;
            q.v[0] = q.v[k];
        }
        if (q.kind == 0) v = empty, kind = 4;
        else if (q.kind == 1) v = (uint32_t)q.v[0], kind = 3;
        else if (q.kind == 2) memcpy(&v, &q.v[0], 4), kind = 2;
        memcpy(rec + 0x38 + 8 * i, &v, 4);
        memcpy(rec + 0x3C + 8 * i, &kind, 4);
    }
}

// A pick for actor `s` among its terrain class's enemies that `restr` allows (enemies.py: the vanilla type comes off
// the room's restrictions first, list.remove), then a fly-bomb / blocking pick restricts its whole group.
static int pick_for(const Slot *s, int *restr, int *nr) {
    for (int i = 0; i < *nr; i++)
        if (restr[i] == s->vtype) { memmove(&restr[i], &restr[i + 1], sizeof(int) * (size_t)(*nr - i - 1)); (*nr)--; break; }
    const int *pk = g_pick[s->cls];
    int np = g_npick[s->cls], pick = -1, tries = 0;
    do pick = np ? pk[rnd_below(np)] : -1;
    while (in(restr, *nr, pick) && ++tries < 256);
    if (pick < 0 || in(restr, *nr, pick)) pick = s->seed_type;  // (nothing fits: the seed's own)
    if (in(g_fly, g_nfly, pick)) { for (int i = 0; i < g_nfly && *nr < 96; i++) restr[(*nr)++] = g_fly[i]; }
    else if (in(g_block, g_nblock, pick)) { for (int i = 0; i < g_nblock && *nr < 96; i++) restr[(*nr)++] = g_block[i]; }
    return pick;
}

// Actor `s`'s record = the seed's (`seed`) made into enemy `pick`.
static void apply_pick(Slot *s, const uint8_t *seed, int pick) {
    memcpy(s->rec, seed, REC);
    int type = pick;
    if (pick == 0x4A) type = 0x4A + rnd_below(3);       // Stretchy Ghosts: a colour
    else if (pick == 0x4D) type = 0x4D + rnd_below(3);  // Color Dungeon orbs: a colour
    uint8_t *r = s->rec;
    if (pick == s->vtype) {  // (enemies.py leaves a vanilla pick untouched)
        if (s->seed_type != s->vtype) {  // the seed changed it: back to the vanilla actor
            uint16_t t = (uint16_t)s->vtype;
            memcpy(r + 0xC, &t, 2);
            const Enemy *en = enemy(s->vtype);
            if (s->vp_ok) put_params(r, s->vp, s->empty);
            else if (en) put_params(r, en->p, s->empty);
            memcpy(r + 0x24, &s->vrot, 4);
            memcpy(r + 0x2C, s->vscale, 12);
            r[0x84] = s->ve;
        }
    } else if (type != s->seed_type) {  // (the seed's own pick: its record already is that enemy)
        const Enemy *en = enemy(pick);
        uint16_t t = (uint16_t)type;
        memcpy(r + 0xC, &t, 2);
        static const Param none[8];
        put_params(r, en ? en->p : none, s->empty);
        float zero = 0, one[3] = {1, 1, 1};
        memcpy(r + 0x24, &zero, 4);
        memcpy(r + 0x2C, one, 12);
        r[0x84] = en ? en->e : 1;
    }
    if (s->cls != CLS_HOLE) {  // (spawns may change the type but not these: facing the screen, size 1, as enemies.py's)
        float zero = 0, one[3] = {1, 1, 1};
        memcpy(r + 0x24, &zero, 4);
        memcpy(r + 0x2C, one, 12);
    }
    if (g_sizes) {  // (randomize-enemy-sizes)
        float sc = 0.5f + (float)(rnd() >> 11) * (1.0f / 9007199254740992.0f), v[3] = {sc, sc, sc};
        memcpy(r + 0x2C, v, 12);
    }
}

static int restr_start(const RFile *rf, int *restr) {
    int nr = 0;
    restr[nr++] = -1;  // (enemies.py: -1 starts the pick loop)
    for (int i = 0; i < rf->nrestr && nr < 96; i++) restr[nr++] = rf->restr[i];
    return nr;
}
static const char *base(const RFile *rf) { return strrchr(rf->path, '/') ? strrchr(rf->path, '/') + 1 : rf->path; }
static int rec_type(const Slot *s) { uint16_t t; memcpy(&t, s->rec + 0xC, 2); return t; }

// One load of a room file: roll its actors (enemies.py shuffleEnemyActors, types part) into their records.
static void roll(RFile *rf, int fd, const char *why) {
    int restr[96], nr = restr_start(rf, restr);
    char log[1024];
    int ll = snprintf(log, sizeof log, "[enemyrand] f%llu %s %s:", (unsigned long long)renderer_present_count(), why,
                      base(rf));
    for (int si = 0; si < rf->n; si++) {
        Slot *s = &g_slots[rf->first + si];
        uint8_t seed[REC];
        if (pread(fd, seed, REC, s->off) != REC) return;
        apply_pick(s, seed, pick_for(s, restr, &nr));
        s->fresh = 1;
        if (ll < (int)sizeof log - 32) ll += snprintf(log + ll, sizeof log - (size_t)ll, " %#x>%#x", s->vtype, rec_type(s));
    }
    if (ENV("LARECOMP_ENEMY_RANDOM_QUIET") == NULL) LOG("%s\n", log);
}

// One actor of a room made again: a new pick for it alone, under the room's restrictions as enemies.py has them at
// its place in the room, and the groups its room mates' current picks hold (one fly-bomb, one blocking enemy).
static void roll_one(RFile *rf, int k, int fd) {
    Slot *s = &g_slots[rf->first + k];
    uint8_t seed[REC];
    if (pread(fd, seed, REC, s->off) != REC) return;
    int restr[96], nr = restr_start(rf, restr);
    for (int si = 0; si < k; si++)  // (the earlier actors' vanilla types off the list, as in roll)
        for (int i = 0; i < nr; i++)
            if (restr[i] == g_slots[rf->first + si].vtype) {
                memmove(&restr[i], &restr[i + 1], sizeof(int) * (size_t)(nr - i - 1));
                nr--;
                break;
            }
    for (int si = 0; si < rf->n; si++) {
        int t = rec_type(&g_slots[rf->first + si]);
        if (si == k) continue;
        if (in(g_fly, g_nfly, t)) { for (int i = 0; i < g_nfly && nr < 96; i++) restr[nr++] = g_fly[i]; }
        else if (in(g_block, g_nblock, t)) { for (int i = 0; i < g_nblock && nr < 96; i++) restr[nr++] = g_block[i]; }
    }
    int was = rec_type(s);
    apply_pick(s, seed, pick_for(s, restr, &nr));
    if (ENV("LARECOMP_ENEMY_RANDOM_QUIET") == NULL)
        LOG("[enemyrand] f%llu spawn %s #%d: %#x>%#x (was %#x)\n", (unsigned long long)renderer_present_count(), base(rf),
            k, s->vtype, rec_type(s), was);
}

// fs.c: `n` bytes of the seed's replacement file `host` (a host path) starting at file offset `off` were just read
// into `dst` - give this load's roll instead.
void enemy_random_filter(const char *host, uint64_t off, uint8_t *dst, uint64_t n) {
    if (!host || !n) return;
    pthread_mutex_lock(&g_lock);
    if (g_loaded && !same_seed()) enemy_random_reset();
    if (!g_loaded) load();
    if (!g_nfiles || !strstr(host, ".leb")) { pthread_mutex_unlock(&g_lock); return; }
    RFile *rf = NULL;
    for (int i = 0; i < g_nfiles && !rf; i++)
        if (g_files[i].ovp == host) rf = &g_files[i];
    if (!rf) {
        const char *rel = strstr(host, "/romfs/");
        if (rel)
            for (int i = 0; i < g_nfiles; i++)
                if (!strcmp(rel + 7, g_files[i].path)) { rf = &g_files[i]; rf->ovp = host; break; }
    }
    if (rf) {
        if (off == 0 || !rf->rolled) {
            int fd = open(host, O_RDONLY);
            if (fd >= 0) { roll(rf, fd, "load"); close(fd); rf->rolled = 1; }
        }
        for (int si = 0; si < rf->n; si++) {
            Slot *s = &g_slots[rf->first + si];
            uint64_t a = s->off > off ? s->off : off, b = s->off + REC < off + n ? s->off + REC : off + n;
            if (a < b) memcpy(dst + (a - off), s->rec + (a - s->off), (size_t)(b - a));
        }
    }
    pthread_mutex_unlock(&g_lock);
}


// Every spawn of a listed actor (tools/patches.py BOSS_SPAWN_SITE, the actor factory; its spawn info: key +0, type
// +8, the record's eight parameters +0x10, as in the room file). The game keeps loaded room files: a level entered
// again, an overworld area streamed back in or a single enemy made again (they come back after a while) make actors
// from the record it already has. So each actor made again gets a new pick of its own (roll_one: its room's
// restrictions, one fly-bomb / blocking enemy with its room mates' current picks); the first spawn after a file roll
// takes that roll. Rotation, scale and the enemy flag stay the file load's (facing the screen, size 1); hole enemies
// (a spawn box vs a Pincer: different enemy flags) only change with a file load.
static int slot_of(uint64_t key) {
    if (!g_keycap) {
        g_keycap = 1;
        while (g_keycap < g_nslots * 2 + 2) g_keycap <<= 1;
        g_keyidx = calloc((size_t)g_keycap, sizeof *g_keyidx);
        g_keys = calloc((size_t)g_keycap, sizeof *g_keys);
        for (int fi = 0; fi < g_nfiles; fi++)
            for (int si = 0; si < g_files[fi].n; si++) {
                int i = g_files[fi].first + si;
                uint64_t k = g_slots[i].key;
                if (!k) continue;
                uint32_t h = (uint32_t)(k ^ (k >> 29) ^ (k >> 47)) & (uint32_t)(g_keycap - 1);
                while (g_keyidx[h]) h = (h + 1) & (uint32_t)(g_keycap - 1);
                g_keyidx[h] = i + 1, g_keys[h] = k;
            }
    }
    uint32_t h = (uint32_t)(key ^ (key >> 29) ^ (key >> 47)) & (uint32_t)(g_keycap - 1);
    while (g_keyidx[h]) {
        if (g_keys[h] == key) return g_keyidx[h] - 1;
        h = (h + 1) & (uint32_t)(g_keycap - 1);
    }
    return -1;
}

void rt_enemy_spawn(uint64_t info) {
    if (!info || (g_loaded && !g_nslots && same_seed()) || !LD64(info)) return;
    pthread_mutex_lock(&g_lock);
    if (g_loaded && !same_seed()) enemy_random_reset();
    if (!g_loaded) load();
    int i = g_nslots ? slot_of(LD64(info)) : -1;
    if (i < 0) { pthread_mutex_unlock(&g_lock); return; }
    Slot *s = &g_slots[i];
    RFile *rf = NULL;
    for (int fi = 0; fi < g_nfiles && !rf; fi++)
        if (i >= g_files[fi].first && i < g_files[fi].first + g_files[fi].n) rf = &g_files[fi];
    if (rf && s->cls != CLS_HOLE) {
        int fd = s->fresh && rf->rolled ? -1 : open(rf->host, O_RDONLY);
        if (fd >= 0) {
            if (!rf->rolled) roll(rf, fd, "spawn"), rf->rolled = 1;  // (no file roll yet: the whole room)
            else roll_one(rf, i - rf->first, fd);
            close(fd);
        }
        s->fresh = 0;
        ST16(info + 8, LD16((uint64_t)(uintptr_t)(s->rec + 0xC)));
        memcpy((void *)(uintptr_t)(info + 0x10), s->rec + 0x38, 0x40);
    }
    pthread_mutex_unlock(&g_lock);
}
