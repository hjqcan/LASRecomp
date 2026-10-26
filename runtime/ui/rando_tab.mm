// Randomizer tab: choose / generate LAS-Randomizer seeds (rando/<seed>/). Switching seeds reboots the game in place
// (game data is read at boot: runtime/main.c), the window stays; each seed keeps its own save slots.
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <algorithm>

#include "imgui.h"
#include "imgui_internal.h"
#include "osk.h"
#include "option_rows.h"
#include "plastic.h"
#include "seed_hash.h"

extern "C" {
#include "../hle.h"
#include "../paths.h"
#include "../rando.h"
#include "../settings.h"
extern const char *g_romfs_dir;
extern int g_start_screen;  // hle/warp.c
void launcher_play_seed(const char *seed);  // ui/launcher.mm
extern char g_window_shot_request[256];  // runtime/hle/hookshot.c (test shots)
}

// Play a seed ("" = vanilla): in game the game reboots on it; on the start screen (Settings) the game starts with it
static void play_seed(const char *name) {
    if (g_start_screen) { launcher_play_seed(name); return; }
    rando_select(name);
    rt_request_reboot(RT_REBOOT_GAME);
}

struct Seed { std::string name, spoiler, id; long mtime; bool race, race_done; };
static bool id_is_race(const std::string &id);  // (a seed made in race mode: its ID says so)
static std::vector<Seed> g_seeds;
static bool g_scanned;
static int g_selected = -1;

static std::string read_file(const std::string &p) {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return "";
    std::string s;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

// Deletes rando/<name> (the seed's game files, spoiler and its save slots).
static bool delete_seed(const std::string &name) {
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos) return false;
    std::string dir = "rando/" + name;
    bool ok = plat_remove_tree(dir.c_str());
    LOG("[rando] deleted seed %s: %s\n", name.c_str(), ok ? "ok" : strerror(errno));
    return ok;
}

static void scan(void) {
    g_seeds.clear();
    DIR *d = opendir("rando");
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string dir = std::string("rando/") + e->d_name;
        struct stat sb;
        if (stat((dir + "/exefs/1.0.0.pchtxt").c_str(), &sb)) continue;
        Seed s;
        s.name = e->d_name;
        s.mtime = sb.st_mtime;
        s.spoiler = dir + "/spoiler.txt";
        if (stat(s.spoiler.c_str(), &sb)) s.spoiler = dir + "/spoiler_" + s.name + ".txt";  // (older seeds)
        s.id = read_file(dir + "/seed_id.txt");
        while (!s.id.empty() && (s.id.back() == '\n' || s.id.back() == ' ')) s.id.pop_back();
        s.race = id_is_race(s.id);
        s.race_done = !stat((dir + "/race_done").c_str(), &sb);  // (the credits were seen: runtime/hle/run_timer.c)
        g_seeds.push_back(s);
    }
    closedir(d);
    std::sort(g_seeds.begin(), g_seeds.end(), [](const Seed &a, const Seed &b) { return a.mtime > b.mtime; });
    g_scanned = true;
}

// ---- generation (background process)
static PlatProc g_gen_pid;
static std::string g_gen_seed, g_gen_status;
static float g_gen_frac;
static double g_gen_start;
static bool g_gen_failed;
static bool g_gen_done;  // finished (or failed to start): the overlay shows the result until Play Now / Close


struct GenOpts {
    char seed[64] = "";       // the seed's NAME (its folder, rando/<name>/): any label; the seed ID is separate
    uint64_t rng = 0;         // 36-bit random part of the seed ID
    int logic = 0;            // always 0 (basic): the setting is gone, the seed ID keeps its 3-bit field
    int dungeon_items = 0;    // none stone-beak mc mcb (default: shuffled)
    int keysanity = 0;        // off / small keys anywhere / + Nightmare Keys / all dungeon items
    bool bomb_bag = false, powder_bag = false;  // shuffle-bombs / shuffle-powder
    bool environments = false, pets_in_dungeons = false;  // randomize-environments / bad-pets
    bool skip_story = true;   // skip-story-events (kids after the Cello, remaining Owl talk): always on
    bool enemy_minibosses = false;  // enemy-minibosses (needs Randomize enemies)
    int tunic = 0;            // retired tunic-color (always 0 = default): the tunic is a live cosmetic now
    bool inc_fishing = false, inc_rapids = false, inc_dampe = false, inc_trade = false, inc_blue = false;
    int traps = 1;            // none few several many trapsanity
    int seashells = 2;        // max reward included: none 5 15 30 40 50
    bool shuffle_instruments = false, shuffle_dungeons = false, randomize_enemies = false, randomize_music = false;
    bool open_mabe = false, fast_songs = true, ohko = false, boss_cutscenes = false, spoiler = true;
    int start_rupees = 0;
    // planned (LAS Recomp additions, not implemented yet)
    int entrances = 0;        // off / dungeons / + houses, caves & connector caves (3 = old "+ connectors": now 2)
    bool decoupled = false, pots = false, grass = false;
    bool no_buffs = false;    // no-buff-drops: enemies never drop Guardian Acorns / Pieces of Power
    bool bushes = false;      // bushsanity: every overworld bush hides an item (tools/rando/bushes.py)
    bool digs = false;        // digsanity: every diggable overworld tile is a check (tools/rando/digs.py)
    bool shellsanity = false; // every Secret Seashell spot is a check (tools/rando/seashell_spots.py); off: vanilla
    int room_shuffle = 0;     // dungeon-room-shuffle: 0 off / 2 coupled / 3 decoupled (1: retired Coherent; dungeon_gen.py)
    bool pet_rooster = false, pet_ghost = false;  // pets in the pool (tools/rando/pets.py)
    bool pet_marin = false;   // Marin in the pool, the desert closed until she wakes the Walrus (open-desert off)
    int shopsanity = 0;       // off / vanilla prices / random prices (tools/rando/shopsanity.py)
    int miniboss_freq = 0;    // miniboss-frequency: overworld mini-boss chance 1x / 2x / 3x / every enemy that fits (enemies.py)
    int enemy_mode = 0;       // enemy-mode: seeded (fixed in the seed) / random (re-rolled at every load: enemy_random.c)
    bool trendsanity = false; // every Trendy Game prize is a check (tools/rando/trendsanity.py)
    bool boss_souls = false;  // boss-souls: a boss only appears once its soul is found (tools/rando/souls.py)
    bool hookshot_any = false;  // hookshot-anything: latches onto any surface (runtime/hle/hookshot.c, hookshot_logic.py)
    bool miniboss_souls = false;  // miniboss-souls: a dungeon mini-boss only appears once its soul is found (souls.py)
    // starting-items (the game's own start): stored as "not" so seed IDs made before these options (zeros) keep them
    bool no_start_sword = false, no_start_shield = false, no_start_ocarina = false;  // (ocarina: with Manbo's Mambo)
    bool random_cosmetics = false;  // random-cosmetics: the seed's own cosmetic set (runtime/cosmetics_seed.c)
    bool tunic_upgrades = false;  // tunic-upgrades: the red / blue tunics add up instead of swapping (runtime: qol.c)
    // stealing (LAS 'stealing': never / normal / always), stored so that seed IDs made before these options (zeros)
    // keep their old rule, 'always' (stealing allowed, no sword needed); new seeds default to the vanilla sword rule
    bool no_stealing = false;       // with Shopsanity: the shopkeeper never lets you steal ('never')
    bool steal_needs_sword = true;  // off = "Steal without sword" ('always'); on = the vanilla rule ('normal')
    // Triforce hunt (tools/rando/triforce.py): pieces needed (0 = off, 1-50), extra pieces 0 none / 1 +25% / 2 +50%
    int tri_needed = 0, tri_extra = 0;
    bool race = false;        // race mode: the spoiler log stays shut until the credits, a seed hash on the title screen
    bool apples = false;      // apples: Applesanity, 3 checks per apple tree (tools/rando/apple_trees.py)
};
static GenOpts g_opts;

// Triforce hunt rows: "Pieces needed" 3 / 12 / 24 / Custom; with Custom, a row under it picks the count
static const int kTriPresets[3] = {3, 12, 24};
static int g_tri_custom = 24;      // the Custom count (kept while a preset is chosen)
static int g_tri_mode = 2;         // 0..2 a preset, 3 Custom (follows a loaded seed ID)
static int g_back_frame = -10;     // the frame B went back a step in the right column (the menu mustn't close on it)
extern "C" bool rando_tab_took_back(void) { return g_back_frame == ImGui::GetFrameCount(); }
static void tri_mode_from_opts(void) {
    g_tri_mode = 3;
    for (int i = 0; i < 3; i++) if (g_opts.tri_needed == kTriPresets[i]) g_tri_mode = i;
    if (g_tri_mode == 3 && g_opts.tri_needed > 0) g_tri_custom = g_opts.tri_needed;
    if (!g_opts.tri_needed) g_tri_mode = 2;
}


// ---- Seed IDs: a 30-character code (Crockford base32, XXXXX-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX) holding the random seed and
// every generation option, with a checksum in the last 5 bits. Same ID + same LAS Recomp version = the same seed, so
// IDs can be shared. The generator gets the ID itself as its seed string. Bump SEED_ID_VERSION when the bit layout
// changes. v2: 36-bit random part; v3: + dungeon room shuffle; v4: 25 characters, + shopsanity, pets, Trendy Game
// prizes (v2 / v3 IDs had 20: they still load, the options they don't hold are off); v5: 30 characters, + Triforce hunt
// (v4 IDs still load, without the hunt). The logic, skip-story and entrance tier 3 fields stay in the layout but are
// fixed now (basic, on, tier 2 includes the connector caves). Layout: version, random part, fields, the yes/no flags
// (growing up), then fixed places counted back from the end: [v5: hunt pieces needed 6, extra 2] steal-needs-sword,
// no-stealing, enemy mode, mini-boss frequency 2, checksum 5.
enum { SEED_ID_VERSION = 5, SEED_ID_BITS = 150, SEED_ID_BITS_V4 = 125, SEED_ID_BITS_V3 = 100, SEED_ID_FLAGS_V3 = 23 };
static int id_mbfreq(int bits) { return bits - 5 - 2; }              // mini-boss frequency
static int id_enemymode(int bits) { return id_mbfreq(bits) - 1; }    // enemy mode (older IDs: 0 = Seeded)
static int id_nosteal(int bits) { return id_enemymode(bits) - 1; }   // stealing off (older IDs: 0 = stealing allowed)
static int id_stealsword(int bits) { return id_nosteal(bits) - 1; }  // stealing needs the sword (older: 0 = no sword)
static int id_hunt(int bits) { return id_stealsword(bits) - 8; }     // (v5) Triforce hunt: pieces needed 6, extra 2
static const char kB32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

struct Bits {
    uint8_t b[SEED_ID_BITS] = {};
    int n = 0;
    void put(uint64_t v, int w) { for (int i = 0; i < w; i++) b[n++] = (v >> i) & 1; }
    uint64_t get(int w) { uint64_t v = 0; for (int i = 0; i < w; i++) v |= (uint64_t)b[n++] << i; return v; }
};

static bool *opt_flags(GenOpts &o, int i) {  // the yes/no options, in ID order (append only)
    bool *f[] = {&o.bomb_bag, &o.powder_bag, &o.environments, &o.pets_in_dungeons, &o.skip_story, &o.enemy_minibosses,
                 &o.inc_fishing, &o.inc_rapids, &o.inc_dampe, &o.inc_trade, &o.inc_blue, &o.shuffle_instruments,
                 &o.shuffle_dungeons, &o.randomize_enemies, &o.randomize_music, &o.open_mabe, &o.fast_songs,
                 &o.boss_cutscenes, &o.spoiler, &o.decoupled, &o.pots, &o.no_buffs, &o.bushes,
                 &o.pet_rooster, &o.pet_ghost, &o.pet_marin, &o.trendsanity, &o.boss_souls, &o.hookshot_any,
                 &o.digs, &o.miniboss_souls, &o.no_start_sword, &o.no_start_shield, &o.no_start_ocarina,
                 &o.tunic_upgrades, &o.shellsanity, &o.race, &o.random_cosmetics, &o.apples};
    return i < (int)(sizeof f / sizeof *f) ? f[i] : NULL;
}

static uint32_t id_check(const Bits &x, int upto) {
    uint32_t h = 0x811c9dc5u;
    for (int i = 0; i < upto; i++) { h ^= x.b[i]; h *= 0x01000193u; }
    return (h ^ (h >> 5) ^ (h >> 10) ^ (h >> 15)) & 31;
}

static void seed_id_encode(GenOpts &o, char *out, size_t cap) {
    Bits x;
    x.put(SEED_ID_VERSION, 4);
    x.put(o.rng, 36);
    x.put(o.logic, 3);  // (retired logic level: always 0 = basic, kept so seed IDs stay the same)
    x.put(o.dungeon_items, 2); x.put(o.keysanity, 2); x.put(o.traps, 3); x.put(o.seashells, 3);
    x.put(o.tunic, 4); x.put(o.entrances, 2); x.put(o.start_rupees, 10);
    x.put(o.room_shuffle, 2);  // (v3)
    x.put(o.shopsanity, 2);    // (v4)
    for (int i = 0; bool *f = opt_flags(o, i); i++) x.put(*f, 1);
    int data = x.n;  // (the flags may grow up to id_hunt)
    x.put(0, SEED_ID_BITS - 5 - data);
    x.n = id_hunt(SEED_ID_BITS);  // (fixed places before the checksum: IDs made before them hold 0 = the old behaviour)
    x.put(o.tri_needed, 6);
    x.put(o.tri_extra, 2);
    x.put(o.steal_needs_sword, 1);
    x.put(o.no_stealing, 1);
    x.put(o.enemy_mode, 1);
    x.n = id_mbfreq(SEED_ID_BITS);
    x.put(o.miniboss_freq, 2);
    x.put(id_check(x, SEED_ID_BITS - 5), 5);
    std::string s;
    for (int c = 0; c < SEED_ID_BITS / 5; c++) {
        if (c && c % 5 == 0) s += '-';
        int v = 0;
        for (int k = 0; k < 5; k++) v |= x.b[c * 5 + k] << k;
        s += kB32[v];
    }
    snprintf(out, cap, "%s", s.c_str());
}

// "" = ok (options + rng set), else what's wrong with the text
static std::string seed_id_decode(const char *text, GenOpts &o) {
    Bits x;
    int chars = 0;
    for (const char *p = text; *p; p++) {
        char ch = (char)toupper((unsigned char)*p);
        if (ch == '-' || ch == ' ') continue;
        if (ch == 'O') ch = '0';
        if (ch == 'I' || ch == 'L') ch = '1';
        const char *q = strchr(kB32, ch);
        if (!q || !ch) return "not a seed ID (unexpected character)";
        if (chars == SEED_ID_BITS / 5) return "too long for a seed ID";
        x.put((uint64_t)(q - kB32), 5);
        chars++;
    }
    int bits = chars * 5;  // 30 characters (v5), 25 (v4), or 20 (v2 / v3)
    if (bits != SEED_ID_BITS && bits != SEED_ID_BITS_V4 && bits != SEED_ID_BITS_V3) return "a seed ID has 30 characters";
    uint32_t sum = 0;
    for (int i = 0; i < 5; i++) sum |= (uint32_t)x.b[bits - 5 + i] << i;
    if (sum != id_check(x, bits - 5)) return "typo? (checksum doesn't match)";
    x.n = 0;
    int ver = (int)x.get(4);
    if (bits == SEED_ID_BITS ? ver != SEED_ID_VERSION : bits == SEED_ID_BITS_V4 ? ver != 4 : ver != 2 && ver != 3)
        return "made by another LAS Recomp version (ID format " + std::to_string(ver) + ")";
    GenOpts t = o;
    t.rng = x.get(36);
    t.logic = (int)x.get(3); t.dungeon_items = (int)x.get(2); t.keysanity = (int)x.get(2); t.traps = (int)x.get(3);
    t.seashells = (int)x.get(3); t.tunic = (int)x.get(4); t.entrances = (int)x.get(2); t.start_rupees = (int)x.get(10);
    t.room_shuffle = ver >= 3 ? (int)x.get(2) : 0;
    t.shopsanity = ver >= 4 ? (int)x.get(2) : 0;
    for (int i = 0; bool *f = opt_flags(t, i); i++) *f = ver >= 4 || i < SEED_ID_FLAGS_V3 ? x.get(1) : 0;
    bool v4up = bits != SEED_ID_BITS_V3;
    if (v4up) { x.n = id_mbfreq(bits); t.miniboss_freq = (int)x.get(2); } else t.miniboss_freq = 0;
    if (v4up) { x.n = id_enemymode(bits); t.enemy_mode = (int)x.get(1); } else t.enemy_mode = 0;
    if (v4up) { x.n = id_stealsword(bits); t.steal_needs_sword = x.get(1); t.no_stealing = x.get(1); }
    else t.steal_needs_sword = t.no_stealing = false;
    if (bits == SEED_ID_BITS) { x.n = id_hunt(bits); t.tri_needed = (int)x.get(6); t.tri_extra = (int)x.get(2); }
    else t.tri_needed = t.tri_extra = 0;
    if (t.room_shuffle > 3 || t.shopsanity > 2 || t.logic > 4 || t.traps > 4 || t.seashells > 5 || t.tunic >= 14 ||
        t.entrances > 3 || t.start_rupees > 999 || t.tri_needed > 50 || t.tri_extra > 2)
        return "not a valid seed ID";
    t.ohko = false;
    // retired choices (older IDs still load): logic is always basic, story scenes always skipped, tier 3
    // (+ connector caves) is part of tier 2 now, and the Coherent room shuffle (1) is Coupled
    t.logic = 0;
    t.skip_story = true;
    t.tunic = 0;  // (the tunic colour is the Cosmetics tab's, live: new seeds stay green; placement never used it)
    if (t.entrances > 2) t.entrances = 2;
    if (t.room_shuffle == 1) t.room_shuffle = 2;
    o = t;
    return "";
}

// The New seed options are kept across restarts (larecomp.ini rando_options): their seed ID without the random part,
// which re-rolls for every seed. Not the seed name either. Saved once a change is done (no row being dragged / typed in).
static void opts_key(const GenOpts &o, char *out, size_t cap) {
    GenOpts t = o;
    t.rng = 0;
    seed_id_encode(t, out, cap);
}

static void opts_load(void) {
    if (!g_settings.rando_options[0]) return;  // (never changed: the defaults)
    GenOpts t = g_opts;
    std::string err = seed_id_decode(g_settings.rando_options, t);
    if (!err.empty()) { LOG("[rando] saved options not loaded: %s\n", err.c_str()); return; }
    t.rng = 0;  // (a fresh random part when shown)
    strlcpy(t.seed, g_opts.seed, sizeof t.seed);
    g_opts = t;
    tri_mode_from_opts();
}

static void opts_save_if_changed(void) {
    if (ImGui::IsAnyItemActive()) return;
    static char defaults[48];
    if (!defaults[0]) opts_key(GenOpts(), defaults, sizeof defaults);
    char k[48];
    opts_key(g_opts, k, sizeof k);
    const char *saved = g_settings.rando_options[0] ? g_settings.rando_options : defaults;
    if (!strcmp(k, saved)) return;
    strlcpy(g_settings.rando_options, k, sizeof g_settings.rando_options);
    settings_save();
}

static bool id_is_race(const std::string &id) {
    GenOpts o;
    return !id.empty() && seed_id_decode(id.c_str(), o).empty() && o.race;
}

// race_overlay.mm: seed <name> was made in race mode (id: its seed ID)
bool rando_seed_race(const char *seed, char *id, size_t idcap) {
    std::string s = read_file(std::string("rando/") + seed + "/seed_id.txt");
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    snprintf(id, idcap, "%s", s.c_str());
    return id_is_race(s);
}

// the seed hash's icon names, for the seed's description ("Bow, Shovel, ...")
static std::string hash_names(const std::string &id) {
    int h[5];
    seed_hash(id.c_str(), h);
    std::string r;
    for (int i = 0; i < 5; i++) r += std::string(i ? ", " : "") + kSeedHashIcons[h[i]].name;
    return r;
}

static uint64_t new_rng(void) { return ((uint64_t)arc4random() << 4 ^ arc4random()) & 0xFFFFFFFFFull; }

static const char *ADJ[] = {"Sleepy", "Brave", "Lucky", "Rapid", "Quiet", "Golden", "Wild", "Secret", "Dreamy", "Mighty"};
static const char *NOUN[] = {"Marin", "Tarin", "Owl", "Cucco", "BowWow", "Moblin", "Zora", "Shell", "WindFish", "Toadstool"};

static void random_seed_name(char *out, size_t n) {
    snprintf(out, n, "%s%s%d", ADJ[arc4random_uniform(10)], NOUN[arc4random_uniform(10)], (int)arc4random_uniform(1000));
}

static std::string clean_name(const char *in) {  // a seed name as a folder name: letters, digits, space - _ ( ) '
    std::string n;
    for (const char *p = in; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        n += (isalnum(ch) || strchr(" -_()'", ch)) && ch ? (char)ch : '_';
    }
    while (!n.empty() && n.back() == ' ') n.pop_back();
    while (!n.empty() && (n[0] == ' ' || n[0] == '.')) n.erase(0, 1);
    return n;
}

// The seed generator: the standalone helper (tools/rando/rando_gen.spec, `make rando-helper`; no Python needed) next to
// the executable or in the .app's Resources (LARECOMP_RANDO_GEN=<path>: that one), else (source tree) the LAS venv
// running tools/rando/las_headless.py. Either takes <romfs> <seed dir> <request.json> and prints
// `stage <fraction> <label>` lines.
static std::vector<std::string> generator_cmd(void) {
    if (const char *g = getenv("LARECOMP_RANDO_GEN")) return {g};
    char p[1024], s[1024];  // (runtime/paths.h: <exe dir>/rando-gen/, <exe dir>/../Resources/rando-gen/, the source tree)
    if (rt_resource("rando-gen/rando-gen" PLAT_EXE_SUFFIX, NULL, p, sizeof p)) return {p};
    if (rt_resource(NULL, "third_party/LAS-Randomizer/" PLAT_VENV_PYTHON, p, sizeof p) &&
        rt_resource(NULL, "tools/rando/las_headless.py", s, sizeof s)) return {p, s};
    return {"third_party/LAS-Randomizer/" PLAT_VENV_PYTHON, "tools/rando/las_headless.py"};
}

static void start_generation(void) {
    if (!g_opts.seed[0]) random_seed_name(g_opts.seed, sizeof g_opts.seed);
    std::string name = clean_name(g_opts.seed);
    if (name.empty()) name = "Seed";
    struct stat sb;
    for (int k = 2; !stat(("rando/" + name).c_str(), &sb); k++) {  // never overwrite a seed: "Name (2)"
        std::string base = clean_name(g_opts.seed);
        name = (base.empty() ? "Seed" : base) + " (" + std::to_string(k) + ")";
    }
    strlcpy(g_opts.seed, name.c_str(), sizeof g_opts.seed);
    if (!g_opts.rng) g_opts.rng = new_rng();
    char id[48];
    seed_id_encode(g_opts, id, sizeof id);
    std::string dir = std::string("rando/") + g_opts.seed;
    mkdir("rando", 0755);
    mkdir(dir.c_str(), 0755);
    if (FILE *idf = fopen((dir + "/seed_id.txt").c_str(), "w")) { fprintf(idf, "%s\n", id); fclose(idf); }
    static const char *ditems[] = {"none", "stone-beak", "mc", "mcb"};
    static const char *traps[] = {"none", "few", "several", "many", "trapsanity"};
    static const char *shell_excl[] = {
        "\"5-seashell-reward\", \"15-seashell-reward\", \"30-seashell-reward\", \"40-seashell-reward\", \"50-seashell-reward\"",
        "\"15-seashell-reward\", \"30-seashell-reward\", \"40-seashell-reward\", \"50-seashell-reward\"",
        "\"30-seashell-reward\", \"40-seashell-reward\", \"50-seashell-reward\"", "\"40-seashell-reward\", \"50-seashell-reward\"",
        "\"50-seashell-reward\"", ""};
    std::string req = dir + "/request.json";
    FILE *f = fopen(req.c_str(), "w");
    if (!f) { g_gen_status = "Cannot write " + req; g_gen_failed = g_gen_done = true; return; }
    fprintf(f, "{\"seed\": \"%s\", \"logic\": \"basic\", \"dungeon-items\": \"%s\", \"traps\": \"%s\",\n", id,
            ditems[g_opts.dungeon_items], traps[g_opts.traps]);
    fprintf(f, " \"shuffle-instruments\": %s, \"shuffle-dungeons\": %s, \"randomize-enemies\": %s, \"randomize-music\": %s,\n",
            g_opts.shuffle_instruments ? "true" : "false", g_opts.shuffle_dungeons ? "true" : "false",
            g_opts.randomize_enemies ? "true" : "false", g_opts.randomize_music ? "true" : "false");
    fprintf(f, " \"open-mabe\": %s, \"fast-songs\": %s, \"OHKO\": %s, \"boss-cutscenes\": %s, \"create-spoiler\": %s,\n",
            g_opts.open_mabe ? "true" : "false", g_opts.fast_songs ? "true" : "false", g_opts.ohko ? "true" : "false",
            g_opts.boss_cutscenes ? "true" : "false", g_opts.spoiler ? "true" : "false");
    fprintf(f, " \"starting-rupees\": %d, \"blur-removal\": false,\n", g_opts.start_rupees);
    {  // what Link starts with (the rest of the pool is placed with logic)
        std::string st;
        if (!g_opts.no_start_sword) st += "\"sword\", ";
        if (!g_opts.no_start_shield) st += "\"shield\", ";
        if (!g_opts.no_start_ocarina) st += "\"ocarina\", \"song-mambo\", ";
        if (!st.empty()) st.resize(st.size() - 2);
        fprintf(f, " \"starting-items\": [%s],\n", st.c_str());
    }
    fprintf(f, " \"pots\": %s, \"no-buff-drops\": %s, \"bushes\": %s, \"digs\": %s,\n", g_opts.pots ? "true" : "false",
            g_opts.no_buffs ? "true" : "false", g_opts.bushes ? "true" : "false", g_opts.digs ? "true" : "false");
    fprintf(f, " \"tunic-upgrades\": %s, \"shellsanity\": %s, \"random-cosmetics\": %s,\n",
            g_opts.tunic_upgrades ? "true" : "false", g_opts.shellsanity ? "true" : "false",
            g_opts.random_cosmetics ? "true" : "false");
    fprintf(f, " \"include-fishing\": %s, \"include-rapids\": %s, \"include-dampe\": %s, \"include-trade-gifts\": %s,"
               " \"include-blue-rupees\": %s,\n", g_opts.inc_fishing ? "true" : "false", g_opts.inc_rapids ? "true" : "false",
            g_opts.inc_dampe ? "true" : "false", g_opts.inc_trade ? "true" : "false", g_opts.inc_blue ? "true" : "false");
    fprintf(f, " \"shuffle-bombs\": %s, \"unlocked-bombs\": %s, \"shuffle-powder\": %s,\n", g_opts.bomb_bag ? "true" : "false",
            g_opts.bomb_bag ? "false" : "true", g_opts.powder_bag ? "true" : "false");
    fprintf(f, " \"randomize-environments\": %s, \"bad-pets\": %s,\n", g_opts.environments ? "true" : "false",
            g_opts.pets_in_dungeons ? "true" : "false");
    fprintf(f, " \"skip-story-events\": true, \"tunic-color\": \"default\", \"enemy-minibosses\": %s,\n",
            g_opts.randomize_enemies && g_opts.enemy_minibosses ? "true" : "false");
    fprintf(f, " \"miniboss-frequency\": %d,\n", g_opts.randomize_enemies && g_opts.enemy_minibosses ? g_opts.miniboss_freq : 0);
    fprintf(f, " \"enemy-mode\": \"%s\",\n", g_opts.randomize_enemies && g_opts.enemy_mode ? "random" : "seeded");
    static const char *ks[] = {"off", "keys", "keys+nk", "all"};
    fprintf(f, " \"keysanity\": \"%s\",\n", ks[g_opts.keysanity]);
    if (g_opts.apples) fprintf(f, " \"apples\": true,\n");  // (off: not written, seeds as before)
    static const char *rs[] = {"off", "coupled", "coupled", "decoupled"};  // (1: retired Coherent, never written)
    fprintf(f, " \"dungeon-room-shuffle\": \"%s\",\n", rs[g_opts.room_shuffle]);
    static const char *shop[] = {"off", "vanilla", "random"};
    fprintf(f, " \"shopsanity\": \"%s\", \"trendsanity\": %s,\n", shop[g_opts.shopsanity], g_opts.trendsanity ? "true" : "false");
    fprintf(f, " \"stealing\": \"%s\",\n", g_opts.shopsanity && g_opts.no_stealing ? "never"
                                          : g_opts.steal_needs_sword ? "normal" : "always");
    fprintf(f, " \"boss-souls\": %s, \"miniboss-souls\": %s,\n", g_opts.boss_souls ? "true" : "false",
            g_opts.miniboss_souls ? "true" : "false");
    fprintf(f, " \"hookshot-anything\": %s,\n", g_opts.hookshot_any ? "true" : "false");
    fprintf(f, " \"race-mode\": %s,\n", g_opts.race ? "true" : "false");  // (runtime only: spoiler lock, seed hash)
    fprintf(f, " \"triforce-hunt\": %d, \"triforce-extra\": %d,\n", g_opts.tri_needed, g_opts.tri_extra * 25);
    fprintf(f, " \"entrance-tier\": %d, \"entrance-decoupled\": %s,\n", g_opts.entrances, g_opts.entrances >= 2 && g_opts.decoupled ? "true" : "false");
    fprintf(f, " \"pet-rooster\": %s, \"pet-ghost\": %s, \"open-desert\": %s,\n", g_opts.pet_rooster ? "true" : "false",
            g_opts.pet_ghost ? "true" : "false", g_opts.pet_marin ? "false" : "true");
    fprintf(f, " \"include\": [\"5-seashell-reward\", \"15-seashell-reward\", \"30-seashell-reward\", \"40-seashell-reward\", \"50-seashell-reward\"],\n");
    fprintf(f, " \"exclude\": [%s]}\n", shell_excl[g_opts.seashells]);
    fclose(f);
    std::string log = dir + "/generate.log";
    std::string romfs = g_romfs_dir ? g_romfs_dir : "extracted/romfs";
    auto abs = [](std::string &p) { if (p[0] != '/' && !(p.size() > 1 && p[1] == ':')) p = std::string(rt_data_root()) + "/" + p; };
    abs(romfs), abs(dir), abs(req);  // (the helper / script change directory: absolute, in the data root)
    std::vector<std::string> cmd = generator_cmd();
    std::vector<const char *> argv;
    for (auto &a : cmd) argv.push_back(a.c_str());
    for (const char *a : {romfs.c_str(), dir.c_str(), req.c_str(), (const char *)NULL}) argv.push_back(a);
    setenv("PYTHONHASHSEED", "0", 1);  // (set iteration order must not differ between runs: same ID, same seed)
    g_gen_pid = plat_spawn(argv.data(), log.c_str(), false);
    int rc = !g_gen_pid;
    g_gen_seed = g_opts.seed;
    g_gen_failed = g_gen_done = rc != 0;
    g_gen_status = rc ? "Could not start the seed generator (rando-gen missing?)" : "Starting...";
    g_gen_frac = 0;
    g_gen_start = ImGui::GetTime();
    if (rc) g_gen_pid = 0;
}

static void poll_generation(void) {
    if (!g_gen_pid) return;
    std::string log = read_file("rando/" + g_gen_seed + "/generate.log");
    size_t p = log.rfind("\nstage ");  // `stage <fraction> <label>` lines from tools/rando/las_headless.py
    if (p != std::string::npos) {
        size_t e = log.find('\n', p + 1);
        std::string line = log.substr(p + 7, e == std::string::npos ? std::string::npos : e - p - 7);
        g_gen_frac = (float)atof(line.c_str());
        size_t sp = line.find(' ');
        if (sp != std::string::npos) g_gen_status = line.substr(sp + 1) + "...";
    }
    int st = plat_proc_wait(g_gen_pid, false);
    if (st >= 0) {
        g_gen_pid = 0;
        g_gen_failed = st != 0;
        g_gen_done = true;
        g_gen_status = g_gen_failed ? "Generation failed: see rando/" + g_gen_seed + "/generate.log" : "Seed \"" + g_gen_seed + "\" ready.";
        size_t e = g_gen_failed ? log.rfind("\nerror ") : std::string::npos;  // (a reason the player can act on)
        if (e != std::string::npos) g_gen_status = log.substr(e + 7, log.find('\n', e + 1) - e - 7);
        g_scanned = false;
        g_opts.seed[0] = 0;
        g_opts.rng = new_rng();  // next seed: a fresh ID
    }
}

// ---- UI helpers (same look as the other tabs)
static void gen_overlay(float scale) {
    if (!g_gen_pid && !g_gen_done) return;
    if (!ImGui::IsPopupOpen("##gen")) ImGui::OpenPopup("##gen");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760 * scale, 0));
    if (!ImGui::BeginPopupModal("##gen", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                                   ImGuiWindowFlags_NoSavedSettings))
        return;
    // test hook: LARECOMP_RANDO_GEN_SHOTS=<prefix> saves <prefix>_gen.png (past 30%) and <prefix>_done.png, then
    // presses Play Now (with LARECOMP_RANDO_GEN_TEST)
    static const char *shots = getenv("LARECOMP_RANDO_GEN_SHOTS");
    static int shot_state, done_frames;
    bool test_play = false;
    if (shots) {
        if (shot_state == 0 && g_gen_pid && g_gen_frac >= 0.3f) {
            snprintf(g_window_shot_request, sizeof g_window_shot_request, "%s_gen.png", shots);
            shot_state = 1;
        }
        if (g_gen_done && ++done_frames == 30) snprintf(g_window_shot_request, sizeof g_window_shot_request, "%s_done.png", shots);
        test_play = done_frames == 60;
    }
    ImGui::TextColored(plastic_col(plastic_theme().title), "%s", g_gen_seed.c_str());
    ImGui::Separator();
    ImGui::Spacing();
    if (g_gen_pid) {  // progress bar with the current step and elapsed time
        char over[32];
        int secs = (int)(ImGui::GetTime() - g_gen_start);
        snprintf(over, sizeof over, "%d:%02d", secs / 60, secs % 60);
        ImGui::TextUnformatted(g_gen_status.c_str());
        ImGui::ProgressBar(g_gen_frac > 0.005f ? g_gen_frac : -0.3f * (float)ImGui::GetTime(), ImVec2(-1, 0), over);
    } else {
        float bw = 230 * scale;
        bool close = false;
        if (g_gen_failed) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(plastic_col(plastic_theme().warn), "%s", g_gen_status.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button("Close", ImVec2(bw, 0))) close = true;
            ImGui::SetItemDefaultFocus();
        } else {
            ImGui::TextUnformatted("Seed ready.");
            ImGui::ProgressBar(1, ImVec2(-1, 0), "Done");
            ImGui::Spacing();
            if (ImGui::Button("Play Now", ImVec2(bw, 0)) || test_play) {  // (switches in place: the game reboots on the new seed)
                g_gen_done = false;
                g_gen_status.clear();
                ImGui::CloseCurrentPopup();
                play_seed(g_gen_seed.c_str());
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(bw, 0))) close = true;
        }
        if (close || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            g_gen_done = false;
            g_gen_status.clear();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndPopup();
}

bool spoiler_view_draw(const std::string &seed, float scale);  // spoiler_view.mm
static std::string g_spoiler_seed;  // seed whose spoiler page is open

// ---- the tab, in two columns (as the Controls tab's Configure inputs): Seeds and the seed option categories on the
// left with a new seed's name, ID and Generate seed under them; the chosen category's rows (or the seed list, or a
// seed's actions) on the right. Each category's rows are in their own function below.
enum { CAT_SEEDS, CAT_GENERAL, CAT_GOAL, CAT_ITEMS, CAT_EXTRA, CAT_WORLD, CAT_ENEMIES, CAT_COMPANIONS, CAT_OTHER, N_CAT };
static const char *const k_cat_name[N_CAT] = {"Seeds", "General", "Goal", "Items", "Extra checks", "World",
                                              "Enemies & bosses", "Companions", "Other"};
static int g_cat = CAT_GENERAL;
static bool g_in_right;            // the cursor is in the right column (last frame)
static int g_last_frame = -10;     // (a frame without this tab: the cursor starts over)
static std::string g_act_seed;     // the seed whose actions the right column shows ("": the seed list)
static std::string g_focus_seed;   // (FOCUS_SEED: this seed's row)
static bool g_confirm_delete;
enum { FOCUS_NONE, FOCUS_CAT, FOCUS_RIGHT, FOCUS_SEED, FOCUS_DELETE, FOCUS_CANCEL, FOCUS_TRI };
static int g_focus;                // put the cursor on this row when it's drawn

static void take_focus(int want) {
    if (g_focus != want || opt_collecting()) return;
    g_focus = FOCUS_NONE;
    opt_focus_item();
}

// ---- the categories' rows
static void rows_general(void) {
    {  // starting items (part of the seed)
        bool sw = !g_opts.no_start_sword, sh = !g_opts.no_start_shield, oc = !g_opts.no_start_ocarina;
        if (opt_toggle("Start with Sword", &sw, true, "Link starts with the sword. Off: it's placed in the world, with logic."))
            g_opts.no_start_sword = !sw;
        if (opt_toggle("Start with Shield", &sh, true, "Link starts with the shield. Off: it's placed in the world, with logic."))
            g_opts.no_start_shield = !sh;
        if (opt_toggle("Start with Ocarina and Manbo's Mambo", &oc, true,
                       "Link starts with the Ocarina and Manbo's Mambo, ready to warp. Off: both are placed in the world, "
                       "with logic."))
            g_opts.no_start_ocarina = !oc;
    }
    {  // (part of the seed: LAS 'stealing' normal / always)
        bool any = !g_opts.steal_needs_sword;
        if (opt_toggle("Steal without sword", &any, false, "You can steal from the shop before you have the sword."))
            g_opts.steal_needs_sword = !any;
    }
    opt_toggle("Treat Tunics as Upgrades", &g_opts.tunic_upgrades, false,
               "The red and blue tunics add up instead of replacing each other: the blue one always halves the damage "
               "Link takes, the red one always strengthens his attacks, and with both he has both.");
    opt_toggle("No Guardian Acorns / Pieces of Power", &g_opts.no_buffs, false,
               "Defeating enemies in a row never drops a Guardian Acorn or a Piece of Power.");
    opt_own_next();  // (the player's own, not part of a seed)
    if (opt_toggle("Check tracker on overworld map", &g_settings.tracker, true,
                   "The overworld map marks the checks you have and haven't done yet."))
        settings_save();
}

static void rows_goal(void) {
    bool hunt = g_opts.tri_needed > 0;
    if (opt_toggle("Triforce hunt", &hunt, false,
                   "Collect Triforce pieces: the last one needed rolls the credits. The Wind Fish isn't needed."))
        g_opts.tri_needed = hunt ? (g_tri_mode < 3 ? kTriPresets[g_tri_mode] : g_tri_custom) : 0;
    opt_indent(1);
    static const char *const pn[] = {"3", "12", "24", "Custom"};
    if (!hunt) opt_disable_next();
    if (opt_choice("Pieces needed", &g_tri_mode, pn, 4, 2, "How many pieces finish the game.", false))
        g_opts.tri_needed = g_tri_mode < 3 ? kTriPresets[g_tri_mode] : g_tri_custom;
    if (hunt && g_tri_mode == 3) {  // Custom: the count
        opt_indent(1);
        int v = g_tri_custom;
        if (opt_int("Custom", &v, 1, 50, 1, 24, "%d", "How many pieces finish the game: 1 to 50."))
            g_tri_custom = v, g_opts.tri_needed = v;
        take_focus(FOCUS_TRI);
        opt_indent(-1);
    }
    static const char *const ex[] = {"None", "+25%", "+50%"};
    if (!hunt) opt_disable_next();
    opt_choice("Extra pieces", &g_opts.tri_extra, ex, 3, 0, "More pieces than needed are hidden, so you can skip some.");
    opt_indent(-1);
}

static void rows_items(void) {
    static const char *ditems[] = {"Shuffled", "Start with stone beaks", "Start with maps + compasses", "Start with all three"};
    opt_choice("Dungeon items", &g_opts.dungeon_items, ditems, 4, 0,
               "Maps, compasses and stone beaks: shuffled into the item pool, or given at the start.");
    opt_toggle("Bomb bag", &g_opts.bomb_bag, false, "You must find your bomb bag to collect and carry bombs.");
    opt_toggle("Powder bag", &g_opts.powder_bag, false, "You must find your powder bag to collect and carry Magic Powder.");
    static const char *traps[] = {"None", "Few", "Several", "Many", "Trapsanity"};
    opt_choice("Traps", &g_opts.traps, traps, 5, 1, "How many traps hide among the items.");
    static const char *shells[] = {"None", "Up to 5", "Up to 15", "Up to 30", "Up to 40", "All (50)"};
    opt_choice("Seashell Mansion rewards", &g_opts.seashells, shells, 6, 2,
               "Which of the Seashell Mansion's rewards can hold important items: up to how many seashells.");
    {
        static const char *rup_names[] = {"0", "50", "100", "200", "300", "500", "999"};
        static const int rup[] = {0, 50, 100, 200, 300, 500, 999};
        int ri = 0;
        for (int i = 0; i < 7; i++) if (abs(rup[i] - g_opts.start_rupees) < abs(rup[ri] - g_opts.start_rupees)) ri = i;
        if (opt_choice("Starting rupees", &ri, rup_names, 7, 0, "Rupees in Link's wallet at the start.")) g_opts.start_rupees = rup[ri];
    }
}

static void rows_extra_checks(void) {
    opt_toggle("Bushsanity", &g_opts.bushes, false,
               "Every bush on the island hides an item from the pool (about 310 more checks; bushes over a hole and tall "
               "grass stay as they are). Unchecked bushes are golden: cut one to reveal its item.");
    opt_toggle("Digsanity", &g_opts.digs, false,
               "INSANE: every diggable tile on the overworld is a check - about 5,000 more, and most important items end "
               "up buried. The first dig of a tile brings its item up; grass and bushes have to be cut first. Holes stay "
               "dug, so you can see where you've been.");
    opt_toggle("Shellsanity", &g_opts.shellsanity, false, "Every hidden Secret Seashell spot holds an item from the pool.");
    opt_toggle("Applesanity", &g_opts.apples, false, "Each apple tree holds 3 items from the pool.");
    static const char *ksn[] = {"Off", "Small keys", "Small + Nightmare", "All dungeon items"};
    opt_choice("Keysanity", &g_opts.keysanity, ksn, 4, 0,
               "Dungeon keys (and with the last choice every dungeon item) can be anywhere in the world, with logic. A "
               "key always opens doors in the dungeon it belongs to; with keysanity the Items screen shows each dungeon's keys.");
    opt_toggle("Potsanity", &g_opts.pots, false,
               "Every liftable pot in houses, caves and dungeons hides an item from the pool (about 400 more checks). "
               "Unchecked pots are golden: lift, smash or throw one to reveal its item.");
    static const char *shopn[] = {"Off", "Vanilla prices", "Random prices"};
    opt_choice("Shopsanity", &g_opts.shopsanity, shopn, 3, 0,
               "Every good in the Mabe Village shop is a check: the shield, hearts, shovel, bombs, chamber stone and "
               "piece of heart. Buy it, or steal it (the shopkeeper remembers a thief).");
    {
        opt_indent(1);
        bool steal = !g_opts.no_stealing;
        if (!g_opts.shopsanity) opt_disable_next();
        if (opt_toggle("Stealing", &steal, true, "Off: the shopkeeper never lets you steal.")) g_opts.no_stealing = !steal;
        opt_indent(-1);
    }
    opt_toggle("Trendy Game prizes", &g_opts.trendsanity, false,
               "Each of the 5 crane prize spots holds a check; won spots then hold ordinary prizes.");
    opt_toggle("Fishing prizes", &g_opts.inc_fishing, false, "The fishing pond's prizes hold items from the pool.");
    opt_toggle("Rapids race prizes", &g_opts.inc_rapids, false, "The Rapids Ride race's prizes hold items from the pool.");
    opt_toggle("Dampe's rewards", &g_opts.inc_dampe, false, "Dampe's Chamber Dungeon challenge rewards hold items from the pool.");
    opt_toggle("Trade quest gifts", &g_opts.inc_trade, false, "The trade quest characters' gifts hold items from the pool.");
    opt_toggle("Color Dungeon Blupees", &g_opts.inc_blue, false, "The Color Dungeon's blupees hold items from the pool.");
}

static void rows_world(float scale) {
    {  // entrances, as levers: dungeon entrances = shuffle-dungeons (LAS); houses & caves = entrance-tier 2 (single
       // interiors + connector caves, tools/rando/las_headless.py), decoupled with them. (Tier 1, the old "dungeons only"
       // level, reads as the dungeon lever.)
        if (g_opts.entrances == 1) g_opts.shuffle_dungeons = true, g_opts.entrances = 0;
        bool dungeons = g_opts.shuffle_dungeons, houses = g_opts.entrances >= 2, on = dungeons || houses;
        if (opt_toggle("Entrance randomizer", &on, false, "Shuffles where entrances lead, with logic."))
            g_opts.shuffle_dungeons = on, g_opts.entrances = 0;  // (on: dungeon entrances, the usual choice)
        bool last = false;  // (an attempt to turn off the last kind)
        opt_indent(1);
        if (!on) opt_disable_next();
        if (opt_toggle("Dungeon entrances", &dungeons, false, "Dungeon entrances lead into other dungeons.")) {
            if (!dungeons && !houses) last = true;
            else g_opts.shuffle_dungeons = dungeons;
        }
        if (!on) opt_disable_next();
        if (opt_toggle("Houses & caves", &houses, false,
                       "Single-entrance interiors and connector caves (two or three doors) are shuffled. Marin & Tarin's "
                       "house and water entrances stay vanilla.")) {
            if (!houses && !dungeons) last = true;
            else g_opts.entrances = houses ? 2 : 0;
        }
        if (!houses) opt_disable_next();
        opt_toggle("Decoupled entrances", &g_opts.decoupled, false,
                   "With Houses & caves: leaving a house puts you outside a different door than the one you came in by.");
        opt_indent(-1);
        if (last) ImGui::OpenPopup("##entlast");
        if (!opt_collecting() && ImGui::BeginPopupModal("##entlast", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::TextUnformatted("With the entrance randomizer on, at least one kind of entrance must be shuffled.");
            ImGui::Spacing();
            ImGui::SetItemDefaultFocus();
            if (ImGui::Button("OK", ImVec2(220 * scale, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    {  // dungeon room shuffle (dungeon-room-shuffle; seed ID 0 off / 2 coupled / 3 decoupled, 1 = the retired
       // Coherent, read as Coupled): Off by default
        static const char *names[] = {"Off", "Coupled", "Decoupled"};
        static const int ids[] = {0, 2, 3};
        int i = g_opts.room_shuffle == 3 ? 2 : g_opts.room_shuffle ? 1 : 0;
        if (opt_choice("Dungeon room shuffle", &i, names, 3, 0,
                       "Experimental. Rebuilds the dungeons (all but Eagle's Tower) from shuffled rooms; boss, mini-boss, "
                       "stairs and other key rooms stay put. Any doorway can lead to any doorway of its dungeon. Coupled: "
                       "going back through a door takes you back. Decoupled: it takes you somewhere else."))
            g_opts.room_shuffle = ids[i];
    }
    {  // its doorway letters on the dungeon map (runtime/ui/chaos_overlay.mm; a player setting, not part of the seed):
       // on = as you explore, every pair once you have the map (CHAOS_LETTERS_MAP); off = none
        bool on = g_settings.chaos_letters != CHAOS_LETTERS_OFF;
        opt_indent(1);
        if (!g_opts.room_shuffle) opt_disable_next();
        opt_own_next();
        if (opt_toggle("Map tracks door connections", &on, true,
                       "Matching letters (A-A, B-B...) on the dungeon map show which doorways connect: a pair appears once "
                       "you've been through either end, and every pair of the dungeon once you have its map.")) {
            g_settings.chaos_letters = on ? CHAOS_LETTERS_MAP : CHAOS_LETTERS_OFF;
            settings_save();
        }
        opt_indent(-1);
    }
    opt_toggle("Shuffle instruments", &g_opts.shuffle_instruments, false,
               "The Siren Instruments are items in the pool instead of each dungeon's reward.");
    opt_toggle("Open Mabe Village", &g_opts.open_mabe, false, "Mabe Village's paths are open from the start.");
    opt_toggle("Randomize environments", &g_opts.environments, false,
               "Each area gets another area's lighting and atmosphere (cosmetic).");
}

static void rows_enemies(void) {
    opt_toggle("Randomize enemies", &g_opts.randomize_enemies, false, "Enemies are shuffled around the island.");
    opt_indent(1);
    {
        if (!g_opts.randomize_enemies) opt_disable_next();
        static const char *const modes[] = {"Seeded", "Random"};
        opt_choice("Mode", &g_opts.enemy_mode, modes, 2, 0,
                   "Seeded: enemies are fixed by the seed. Random: they change every time an area loads.");
    }
    if (!g_opts.randomize_enemies) opt_disable_next();
    opt_toggle("Include mini-bosses", &g_opts.enemy_minibosses, false,
               "Some overworld enemies become mini-bosses: Hinox, Armos Knight, Dodongo Snake, Giant Buzz Blob, Smasher, "
               "Avalaunch, Rolling Bones, Cue Ball, Blaino, Master Stalfos, Moblin Chief, Lanmola, the Ball-and-Chain "
               "Soldier or Shadow Link. Shadow Link, Blaino and the Soldier also appear in dungeons and caves.");
    {
        opt_indent(1);
        if (!g_opts.randomize_enemies || !g_opts.enemy_minibosses) opt_disable_next();
        static const char *const freq[] = {"Normal", "2x", "3x", "All"};
        opt_choice("Mini-boss frequency", &g_opts.miniboss_freq, freq, 4, 0,
                   "How often overworld enemies become mini-bosses. All: every one with room for one.");
        opt_indent(-1);
    }
    opt_indent(-1);
    opt_toggle("Boss souls", &g_opts.boss_souls, false,
               "Each dungeon boss (and the Color Dungeon's) has a soul hidden among the items. Until you find it the boss "
               "isn't in its lair: the way on to the instrument stays shut until the boss is beaten. Found souls show "
               "on the Items screen.");
    opt_toggle("Mini-boss souls", &g_opts.miniboss_souls, false,
               "Each kind of dungeon mini-boss has a soul hidden among the items; one soul counts for every fight with "
               "that kind. Until you find it the mini-boss doesn't show up and its room's way on stays shut.");
}

static void rows_companions(void) {
    opt_toggle("Flying Rooster", &g_opts.pet_rooster, false,
               "The Flying Rooster is an item: found, he follows you. The logic counts him wherever he can stand in for "
               "the feather or hookshot.");
    opt_toggle("Ghost", &g_opts.pet_ghost, false,
               "The Ghost is an item: found, walk him to his grave for its check. He no longer appears by himself after "
               "the Surf Harp.");
    opt_toggle("Marin", &g_opts.pet_marin, false,
               "Marin is an item in the pool, and the desert stays closed until you find her.");
    opt_toggle("Companions allowed in dungeons", &g_opts.pets_in_dungeons, false,
               "BowWow, the Rooster and the others follow Link into dungeons; the logic counts what they can do there.");
}

static void rows_other(void) {
    opt_toggle("Hookshot anything", &g_opts.hookshot_any, false,
               "The Hookshot latches onto any wall or object it hits, not only hookshot targets. The logic counts the "
               "gaps this crosses. Seeds made with it always have Hookshot Anything on.");
    opt_toggle("Fast songs", &g_opts.fast_songs, true, "Learning a song skips its cutscene.");
    opt_toggle("Boss cutscenes", &g_opts.boss_cutscenes, false, "Bosses' intro cutscenes play.");
    opt_toggle("Randomize music", &g_opts.randomize_music, false, "The game's music tracks are shuffled.");
    opt_toggle("Random cosmetics", &g_opts.random_cosmetics, false,
               "The seed gets its own random colors. Your choices in the Cosmetics tab still win.");
    opt_toggle("Spoiler log", &g_opts.spoiler, true, "Writes where every item is, to read from the seed's Spoiler log.");
    opt_toggle("Race mode", &g_opts.race, false,
               "The spoiler log stays shut until the credits, and the title screen shows a seed hash to compare.");
}

static void cat_rows(int c, float scale) {
    switch (c) {
    case CAT_GENERAL: rows_general(); break;
    case CAT_GOAL: rows_goal(); break;
    case CAT_ITEMS: rows_items(); break;
    case CAT_EXTRA: rows_extra_checks(); break;
    case CAT_WORLD: rows_world(scale); break;
    case CAT_ENEMIES: rows_enemies(); break;
    case CAT_COMPANIONS: rows_companions(); break;
    case CAT_OTHER: rows_other(); break;
    }
}

// ---- the options picked, read-only (Generate seed under the cursor), for a last look: the new seed's name and ID, then
// every seed option with its value, by category in the left column's order (options shut by their parent - greyed out
// on their page - are left out; the player's own settings too). Two columns when the pane is wide enough; an entry too
// long for its column goes on under itself, a step in. The pane scrolls (wheel, right stick: option_rows.mm).
static void seed_summary(float scale) {
    const PlasticTheme &pt = plastic_theme();
    struct Item { bool head; std::string a, b; int indent; };
    std::vector<Item> items;
    for (int c = CAT_GENERAL; c < N_CAT; c++) {
        std::vector<OptLine> v;
        opt_collect_begin(&v);
        cat_rows(c, scale);
        opt_collect_end();
        if (v.empty()) continue;
        items.push_back({true, k_cat_name[c], "", 0});
        for (const OptLine &l : v) items.push_back({false, l.label, l.value, l.indent});
    }
    char id[48];
    seed_id_encode(g_opts, id, sizeof id);
    opt_section("Your options");
    ImGui::SetWindowFontScale(0.8f);
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), lh = fs * 1.12f, gap = fs * 0.4f, step = fs * 0.9f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    // "label: value" in lines of at most w (broken between words; the lines after the first a step in): each line's
    // start and end in the text, and where the label ends (the value is drawn in its own colour)
    struct Line { size_t s, e; };
    auto wrap = [&](const std::string &t, float w, std::vector<Line> *out) {
        out->clear();
        const char *b = t.c_str(), *p = b, *end = b + t.size();
        while (p < end) {
            float lw = out->empty() ? w : w - step;
            const char *q = font->CalcWordWrapPositionA(fs / font->FontSize, p, end, lw);
            if (q <= p) q = p + 1;  // (a word longer than the line: at least one character)
            out->push_back({(size_t)(p - b), (size_t)(q - b)});
            p = q;
            while (p < end && *p == ' ') p++;
        }
        return (int)out->size();
    };
    auto draw = [&](ImVec2 at, float w, const Item &it) {
        std::string la = it.a + ": ", t = la + it.b;
        std::vector<Line> ls;
        wrap(t, w, &ls);
        for (size_t k = 0; k < ls.size(); k++) {
            float x = at.x + (k ? step : 0), y = at.y + lh * k;
            size_t s = ls[k].s, e = ls[k].e, m = std::min(std::max(s, la.size()), e);  // (label: s..m, value: m..e)
            if (m > s) dl->AddText(font, fs, ImVec2(x, y), pt.text, t.c_str() + s, t.c_str() + m);
            if (e > m) {
                float lx = m > s ? font->CalcTextSizeA(fs, FLT_MAX, 0, t.c_str() + s, t.c_str() + m).x : 0;
                dl->AddText(font, fs, ImVec2(x + lx, y), pt.value_changed, t.c_str() + m, t.c_str() + e);
            }
        }
        return lh * ls.size();
    };
    ImVec2 p = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x;
    p.y += draw(p, W, {false, "Seed", g_opts.seed[0] ? g_opts.seed : "Random", 0});
    p.y += draw(p, W, {false, "Seed ID", id, 0});
    p.y += gap;
    // two columns while a middling entry still fits on one line of one (the longer ones go on under themselves)
    float mid_w = font->CalcTextSizeA(fs, FLT_MAX, 0, "Seashell Mansion rewards: All (50)").x + fs * 0.5f;
    int cols = (W - fs) * 0.5f >= mid_w ? 2 : 1;
    float cw = cols == 2 ? (W - fs) * 0.5f : W;
    std::vector<float> hs(items.size());
    float total = 0;
    std::vector<Line> tmp;
    for (size_t i = 0; i < items.size(); i++) {
        const Item &it = items[i];
        float iw = cw - fs * 0.5f - step * it.indent;
        hs[i] = it.head ? lh + gap : lh * wrap(it.a + ": " + it.b, iw, &tmp);
        total += hs[i];
    }
    size_t split = items.size();  // (the second column starts at a category: the most even split)
    if (cols == 2) {
        float acc = 0, best = total;
        for (size_t i = 0; i < items.size(); i++) {
            if (items[i].head && i && fmaxf(acc, total - acc) < best) best = fmaxf(acc, total - acc), split = i;
            acc += hs[i];
        }
    }
    float y0 = p.y, y = y0, x = p.x, ymax = y0;
    for (size_t i = 0; i < items.size(); i++) {
        if (i == split) x += cw + fs, y = y0;
        const Item &it = items[i];
        if (it.head) {
            if (y > y0) y += gap;
            std::string t = it.a;
            for (char &c : t) c = (char)toupper((unsigned char)c);
            dl->AddText(font, fs, ImVec2(x, y), pt.section, t.c_str());
            y += lh;
        } else {
            float ix = fs * 0.5f + step * it.indent;
            y += draw(ImVec2(x + ix, y), cw - ix, it);
        }
        ymax = fmaxf(ymax, y);
    }
    ImGui::Dummy(ImVec2(W, ymax + gap - ImGui::GetCursorScreenPos().y));
    ImGui::SetWindowFontScale(1.0f);
}

// ---- Seeds: the seed list, or a seed's actions
static void rows_seeds(const char *active) {
    if (!g_act_seed.empty()) {
        const Seed *sp = NULL;
        for (const Seed &x : g_seeds) if (x.name == g_act_seed) sp = &x;
        if (!sp) { g_act_seed.clear(), g_confirm_delete = false; return; }
        const Seed &s = *sp;
        bool is_active = active && s.name == active;
        static char rename_buf[64];
        if (!opt_text_editing("Rename")) strlcpy(rename_buf, s.name.c_str(), sizeof rename_buf);
        if (g_confirm_delete) {
            ImGui::TextColored(plastic_col(plastic_theme().warn), "Delete this seed and its save files?");
            ImGui::Spacing();
            opt_warn_next();
            if (opt_action("Delete seed + saves", "", NULL)) {
                delete_seed(s.name);
                g_act_seed.clear(), g_confirm_delete = false, g_focus = FOCUS_RIGHT;
                scan();
                return;
            }
            if (opt_action("Cancel", "", NULL)) g_confirm_delete = false, g_focus = FOCUS_DELETE;
            take_focus(FOCUS_CANCEL);
            return;
        }
        if (!is_active && opt_action("Play", "", NULL)) play_seed(s.name.c_str());
        if (s.race && !s.race_done) opt_disable_next();  // (race mode: shut until the credits)
        opt_button_next();  // (Locked: still a button, greyed out)
        if (opt_action("Spoiler log", s.race && !s.race_done ? "Locked" : "", NULL)) g_spoiler_seed = s.name;
        if (s.id.empty()) opt_disable_next();
        if (opt_action("Copy seed ID", "", NULL)) ImGui::SetClipboardText(s.id.c_str());
        if (is_active) opt_disable_next();  // (the game is reading its files)
        opt_button_next();  // (a button like the others, the name on its right)
        if (opt_text("Rename", rename_buf, sizeof rename_buf, "", NULL)) {
            std::string to_name = clean_name(rename_buf), to = "rando/" + to_name;
            struct stat sb;
            if (!to_name.empty() && to_name != s.name) {
                if (!stat(to.c_str(), &sb)) g_gen_status = "A seed named \"" + to_name + "\" already exists.";
                else if (rename(("rando/" + s.name).c_str(), to.c_str()) == 0) {
                    // (seeds made before seed IDs name their spoiler after the folder)
                    rename((to + "/spoiler_" + s.name + ".txt").c_str(), (to + "/spoiler_" + to_name + ".txt").c_str());
                    LOG("[rando] renamed seed %s -> %s\n", s.name.c_str(), to_name.c_str());
                    g_act_seed = to_name;
                }
            }
            scan();
            return;
        }
        if (is_active) opt_disable_next();
        opt_warn_next();  // (red: not to be taken for Play)
        if (opt_action("Delete", "", NULL)) g_confirm_delete = true, g_focus = FOCUS_CANCEL;
        take_focus(FOCUS_DELETE);
        return;
    }
    if (active && opt_action("Switch to vanilla", "", "Leaves this seed and plays the game without the randomizer. "
                                                       "Each seed keeps its own save files."))
        play_seed("");
    for (const Seed &s : g_seeds) {
        bool is_active = active && s.name == active;
        std::string d = "Seed ID: " + (s.id.empty() ? std::string("-") : s.id) +
                        (s.race ? "\nRace mode. Seed hash: " + hash_names(s.id) : std::string()) +
                        "\n\nPlay it, read its spoiler log, copy its ID to share, rename or delete it. Each seed has its "
                        "own save files.";
        if (opt_action(s.name.c_str(), is_active ? "Playing" : "", d.c_str(), false))
            g_act_seed = s.name, g_confirm_delete = false, g_focus = FOCUS_RIGHT;
        if (g_focus == FOCUS_SEED && s.name == g_focus_seed) take_focus(FOCUS_SEED);
    }
    if (g_focus == FOCUS_SEED) g_focus = FOCUS_RIGHT;  // (that seed is gone: the first row)
    if (g_seeds.empty()) {
        opt_disable_next();
        opt_action("No seeds yet", "", "Make one with Generate seed.");
    }
}

// the seed info above its actions
static void seed_header(const char *active) {
    for (const Seed &s : g_seeds) {
        if (s.name != g_act_seed) continue;
        ImGui::TextColored(plastic_col(plastic_theme().title), "%s", s.name.c_str());
        ImGui::TextDisabled("Seed ID: %s", s.id.empty() ? "-" : s.id.c_str());
        if (s.race) ImGui::TextDisabled("Race mode. Seed hash: %s", hash_names(s.id).c_str());
        if (active && s.name == active) ImGui::TextDisabled("Playing");
        ImGui::Spacing();
    }
}

extern "C" void rando_tab_draw(float scale) {
    if (!g_scanned) scan();
    static bool spoiler_test;  // LARECOMP_SPOILER_TEST=<seed>: open that seed's spoiler page (test hook)
    if (!spoiler_test && getenv("LARECOMP_SPOILER_TEST")) { spoiler_test = true; g_spoiler_seed = getenv("LARECOMP_SPOILER_TEST"); }
    if (!g_spoiler_seed.empty()) {  // the spoiler page replaces the tab until Back
        if (!spoiler_view_draw(g_spoiler_seed, scale)) g_spoiler_seed.clear();
        return;
    }
    static bool opts_once;  // the options the player last set (larecomp.ini)
    if (!opts_once) { opts_once = true; opts_load(); }
    static bool id_once;  // LARECOMP_RANDO_ID=<seed ID>: the tab starts with that ID's options (test hook)
    if (const char *t = getenv("LARECOMP_RANDO_ID"); !id_once && t) {
        id_once = true;
        std::string err = seed_id_decode(t, g_opts);
        if (!err.empty()) LOG("[rando] test seed ID: %s\n", err.c_str());
        tri_mode_from_opts();
    }
    static bool test_once;  // LARECOMP_RANDO_GEN_TEST=1|<name>[:<seed ID>]: generate a seed (with that ID's options)
    if (const char *t = getenv("LARECOMP_RANDO_GEN_TEST"); !test_once && t) {  // as soon as the tab is shown (test hook)
        test_once = true;
        std::string nm = t;
        if (size_t c = nm.find(':'); c != std::string::npos) {
            std::string err = seed_id_decode(nm.c_str() + c + 1, g_opts);
            if (!err.empty()) LOG("[rando] test seed ID: %s\n", err.c_str());
            nm.resize(c);
        }
        strlcpy(g_opts.seed, nm.size() > 1 ? nm.c_str() : "MenuTest", sizeof g_opts.seed);
        start_generation();
    }
    poll_generation();

    int frame = ImGui::GetFrameCount();
    if (frame - g_last_frame > 1) g_focus = FOCUS_NONE, g_in_right = false, g_confirm_delete = false;  // (back on the tab)
    g_last_frame = frame;
    // (test) LARECOMP_RANDO_CAT=<category name, e.g. extra checks>[:right]: that category shown (right: the cursor on its
    // first row); LARECOMP_RANDO_SEED_OPEN=<seed>: that seed's actions; LARECOMP_TRI_RIGHT=1: the Triforce hunt's Custom
    // count row. For a few frames, past the menu putting the cursor on the tab bar.
    static int hook_frames = -1, hook_focus;
    if (hook_frames < 0) {
        hook_frames = 0;
        if (const char *h = getenv("LARECOMP_RANDO_CAT")) {
            for (int c = 0; c < N_CAT; c++)
                if (!strncasecmp(h, k_cat_name[c], strlen(k_cat_name[c]))) g_cat = c;
            hook_focus = strstr(h, ":right") ? FOCUS_RIGHT : FOCUS_CAT, hook_frames = 10;
        }
        if (const char *h = getenv("LARECOMP_RANDO_SEED_OPEN")) g_cat = CAT_SEEDS, g_act_seed = h, hook_focus = FOCUS_RIGHT, hook_frames = 10;
        if (getenv("LARECOMP_TRI_RIGHT")) g_cat = CAT_GOAL, hook_focus = FOCUS_TRI, hook_frames = 10;
    }
    if (hook_frames > 0) hook_frames--, g_focus = hook_focus;

    bool busy = g_gen_pid || g_gen_done;  // the generation overlay is up: the whole panel is greyed out under it
    // B / Esc in the right column: a step back (delete confirm -> the seed's actions -> the seed list -> the category);
    // Left too, on a row that doesn't step with left / right
    if (g_in_right && !busy && !ui_text_busy() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        (opt_pressed_back() || (opt_pressed_left() && !opt_focused_lr()))) {
        g_back_frame = frame;
        if (g_confirm_delete) g_confirm_delete = false, g_focus = FOCUS_DELETE;
        else if (!g_act_seed.empty()) g_focus_seed = g_act_seed, g_act_seed.clear(), g_focus = FOCUS_SEED;
        else g_focus = FOCUS_CAT;
    }

    ImGui::BeginDisabled(busy);
    // (the start screen: no game loaded, nothing playing - and the seed's patches mustn't be applied yet)
    const char *active = !g_start_screen && rando_seed_dir() ? rando_seed_dir() + 6 : NULL;

    opt_begin("##rando", scale);
    opt_compact();
    for (int c = 0; c < N_CAT; c++) {
        if (g_in_right && c == g_cat) opt_mark_next();
        char n[16];
        snprintf(n, sizeof n, "%d", (int)g_seeds.size());
        bool open = opt_link(k_cat_name[c], NULL, c == CAT_SEEDS ? n : NULL);
        bool clicked = ImGui::IsItemClicked();
        if (c == g_cat) take_focus(FOCUS_CAT);
        bool pick = (opt_item_nav_focused() && g_cat != c) || open;  // (moving down the list shows each)
        if (pick && g_cat != c) g_act_seed.clear(), g_confirm_delete = false;
        if (pick) g_cat = c;
        if (open && !clicked) g_focus = FOCUS_RIGHT;  // (A / Right: the cursor goes to its rows)
        if (open && c == CAT_SEEDS) g_act_seed.clear(), g_confirm_delete = false;
    }

    // ---- a new seed
    opt_section("New seed");
    opt_text("Seed name", g_opts.seed, sizeof g_opts.seed, "Random",
             "The seed's name (its folder). Leave it empty for a random one.");
    {  // Seed ID: the ID of these options + this seed's random part; type / paste one to load a shared seed
        static char idbuf[48];
        static std::string id_msg;
        if (!g_opts.rng) g_opts.rng = new_rng();
        if (!opt_text_editing("Seed ID")) seed_id_encode(g_opts, idbuf, sizeof idbuf);
        std::string d = "A seed ID holds the random seed and every seed option: the same ID on the same LAS Recomp "
                        "version makes the same seed. Enter a shared ID here to load its options.";
        if (!id_msg.empty()) d += "\n\n" + id_msg;
        if (opt_text("Seed ID", idbuf, sizeof idbuf, "", d.c_str())) {
            GenOpts t = g_opts;
            id_msg = seed_id_decode(idbuf, t);
            if (id_msg.empty()) {
                char keep[64];
                strlcpy(keep, g_opts.seed, sizeof keep);
                g_opts = t;
                strlcpy(g_opts.seed, keep, sizeof g_opts.seed);
                tri_mode_from_opts();
                id_msg = "Loaded: the options are set from this ID.";
            }
        }
        opt_pair_next();
        if (opt_action("Copy new seed's ID", "", "Copies the seed ID above, to share the seed.")) ImGui::SetClipboardText(idbuf);
        if (opt_action("New seed ID", "", "A new random seed with the same options.")) { g_opts.rng = new_rng(); id_msg.clear(); }
    }
    if (opt_action("Generate seed", "", "Makes a seed with these options. It shows up under Seeds, ready to play."))
        start_generation();
    bool summary = ImGui::IsItemHovered() || opt_item_nav_focused();  // (this row only: the right column sums up the options)
    if (!g_gen_status.empty() && !busy) {  // (e.g. a rename that failed)
        opt_disable_next();
        opt_action(g_gen_status.c_str(), "", g_gen_status.c_str(), false);
    }

    // ---- the right column: the chosen category's rows, the description under them
    opt_split();
    if (summary) seed_summary(scale);
    else {
        if (g_cat == CAT_SEEDS && !g_act_seed.empty()) seed_header(active);
        else opt_section(k_cat_name[g_cat]);
        opt_list_begin();
        if (g_focus == FOCUS_RIGHT) g_focus = FOCUS_NONE, opt_focus_next_row();
        if (g_cat == CAT_SEEDS) rows_seeds(active);
        else cat_rows(g_cat, scale);
        opt_list_end();
        opt_desc_text();
    }
    g_in_right = opt_in_right();
    opt_split_end();
    ImGui::EndDisabled();
    opts_save_if_changed();
    gen_overlay(scale);
}
