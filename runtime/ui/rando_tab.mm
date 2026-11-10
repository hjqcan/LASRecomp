// Randomizer page: choose / generate LAS-Randomizer seeds (rando/<seed>/). Its own panel on the start screen (the
// RANDOMIZER row's New Seed: launcher.mm) and the in-game menu bar's Randomizer (menu.mm), not a Settings tab.
// Switching seeds reboots the game in place (game data is read at boot: runtime/main.c), the window stays; each seed
// keeps its own save slots.
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <string>
#include <vector>
#include <algorithm>

#include "imgui.h"
#include "localization.h"
#include "imgui_internal.h"
#include "i18n.h"
#include "osk.h"
#include "option_rows.h"
#include "plastic.h"
#include "seed_hash.h"

extern "C" {
#include "../hle.h"
#include "../paths.h"
#include "../platform.h"
#include "../rando.h"
#include "../rando_profile.h"
#include "../settings.h"
#include "../gameversion.h"
extern const char *g_romfs_dir;
extern int g_start_screen;  // hle/warp.c
void launcher_play_seed(const char *seed);  // ui/launcher.mm
extern char g_window_shot_request[256];  // runtime/hle/hookshot.c (test shots)
}

static std::string g_source_romfs = "extracted/romfs";
extern "C" void rando_set_game_root(const char *root) { g_source_romfs = std::string(root) + "/romfs"; }

// Play a seed ("" = vanilla): in game the game reboots on it; on the start screen (Settings) the game starts with it
static void play_seed(const char *name) {
    if (g_start_screen) { launcher_play_seed(name); return; }
    rando_select(name);
    rt_request_reboot(RT_REBOOT_GAME);
}

// printf into a std::string (a shown line from a translated format, i18n.h)
static std::string strf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static std::string strf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
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
        if (!rando_seed_compatible(dir.c_str(), game_version_current()) ||
            stat((dir + "/exefs/" + rando_patch_filename(game_version_current())).c_str(), &sb)) continue;
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
#if PLAT_TOUCH
    plat_touch_shortcuts_sync();  // (the launcher shortcuts' seeds: made, renamed or deleted here)
#endif
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
    int dungeon_items = 0;    // given at the start: bit 0 stone beaks, bit 1 maps (LAS none stone-beak mc mcb)
    bool compass_differs = false;  // compasses given unlike the maps (seed IDs before the split: 0, the same)
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
    // planned (Multi additions, not implemented yet)
    int entrances = 0;        // off / dungeons / + houses, caves & connector caves (3 = old "+ connectors": now 2)
    bool decoupled = false, pots = false, grass = false;
    bool no_buffs = false;    // no-buff-drops: enemies never drop Guardian Acorns / Pieces of Power
    bool bushes = false;      // bushsanity: every overworld bush hides an item (tools/rando/bushes.py)
    bool digs = false;        // digsanity: every diggable overworld tile is a check (tools/rando/digs.py)
    bool shellsanity = false; // every Secret Seashell spot is a check (tools/rando/seashell_spots.py); off: vanilla
    int room_shuffle = 0;     // dungeon-room-shuffle: 0 off / 2 coupled / 3 decoupled (1: retired Coherent; dungeon_gen.py)
    bool pet_rooster = false, pet_ghost = false;  // pets in the pool (tools/rando/pets.py)
    bool pet_marin = false;   // Marin in the pool, the desert closed until she wakes the Walrus (open-desert off); she stays after
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
    int start_instruments = 0;  // starting-instruments: Link starts with this many (0-8), picked by the seed (LAS shuffler)
    bool race = false;        // race mode: the spoiler log stays shut until the credits, a seed hash on the title screen
    bool apples = false;      // apples: Applesanity, 3 checks per apple tree (tools/rando/apple_trees.py)
    bool chest_looks = false; // chest-looks: a chest's colour shows what's inside (tools/rando/chest_looks.py)
    bool retired_key_rings = false;  // (the removed key rings: its seed ID bit stays, always off)
    bool retired_labyrinth = false;  // (the removed Labyrinth: its seed ID bit stays, always off)
    bool hints = false;       // hints: owl statues and phone booths tell about the seed (tools/rando/seed_hints.py)
    bool rocks = false;       // rocks: every overworld rock hides an item (tools/rando/rocks.py)
    // fast-egg: the Egg's first drop lands at the boss, no maze (tools/rando/fast_egg.py). On for new seeds; seed IDs
    // made before it hold 0 = the maze, as they were
    bool fast_egg = true;
    bool no_start_mambo = false;  // Manbo's Mambo not given at the start (before the split: with the Ocarina)
    bool weather_cycle = false;  // weather-cycle: the overworld's weather changes on its own (runtime/nvn/weather_cycle.c)
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
// every generation option, with a checksum in the last 5 bits. Same ID + same Multi version = the same seed, so
// IDs can be shared. The generator gets the ID itself as its seed string. Bump SEED_ID_VERSION when the bit layout
// changes. v2: 36-bit random part; v3: + dungeon room shuffle; v4: 25 characters, + shopsanity, pets, Trendy Game
// prizes (v2 / v3 IDs had 20: they still load, the options they don't hold are off); v5: 30 characters, + Triforce hunt
// (v4 IDs still load, without the hunt; v5 IDs made before Starting Instruments hold 0 there). The logic, skip-story and entrance tier 3 fields stay in the layout but are
// fixed now (basic, on, tier 2 includes the connector caves). Layout: version, random part, fields, the yes/no flags
// (growing up), then fixed places counted back from the end: [v5: starting instruments 4, hunt pieces needed 6,
// extra 2] steal-needs-sword,
// no-stealing, enemy mode, mini-boss frequency 2, checksum 5.
enum { SEED_ID_VERSION = 5, SEED_ID_BITS = 150, SEED_ID_BITS_V4 = 125, SEED_ID_BITS_V3 = 100, SEED_ID_FLAGS_V3 = 23 };
static int id_mbfreq(int bits) { return bits - 5 - 2; }              // mini-boss frequency
static int id_enemymode(int bits) { return id_mbfreq(bits) - 1; }    // enemy mode (older IDs: 0 = Seeded)
static int id_nosteal(int bits) { return id_enemymode(bits) - 1; }   // stealing off (older IDs: 0 = stealing allowed)
static int id_stealsword(int bits) { return id_nosteal(bits) - 1; }  // stealing needs the sword (older: 0 = no sword)
static int id_hunt(int bits) { return id_stealsword(bits) - 8; }     // (v5) Triforce hunt: pieces needed 6, extra 2
static int id_startinst(int bits) { return id_hunt(bits) - 4; }      // (v5) starting instruments 0-8
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
                 &o.tunic_upgrades, &o.shellsanity, &o.race, &o.random_cosmetics, &o.apples, &o.chest_looks,
                 &o.retired_key_rings, &o.hints, &o.rocks, &o.fast_egg, &o.compass_differs, &o.no_start_mambo,
                 &o.weather_cycle, &o.retired_labyrinth};
                 // (order after random_cosmetics: apples, chest_looks, key rings (retired), hints, rocks, fast_egg,
                 // compass_differs, no_start_mambo, weather_cycle, labyrinth (retired))
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
    int data = x.n;  // (the flags may grow up to id_startinst)
    x.put(0, SEED_ID_BITS - 5 - data);
    x.n = id_startinst(SEED_ID_BITS);  // (fixed places before the checksum: IDs made before them hold 0 = the old behaviour)
    x.put(o.start_instruments, 4);
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

// "" = ok (options + rng set), else what's wrong with the text (English: shown through L(), logged as it is)
static std::string seed_id_decode(const char *text, GenOpts &o) {
    Bits x;
    int chars = 0;
    for (const char *p = text; *p; p++) {
        char ch = (char)toupper((unsigned char)*p);
        if (ch == '-' || ch == ' ') continue;
        if (ch == 'O') ch = '0';
        if (ch == 'I' || ch == 'L') ch = '1';
        const char *q = strchr(kB32, ch);
        if (!q || !ch) return LN("That's not a seed ID. It has a character seed IDs don't use.");
        if (chars == SEED_ID_BITS / 5) return LN("Too long for a seed ID.");
        x.put((uint64_t)(q - kB32), 5);
        chars++;
    }
    int bits = chars * 5;  // 30 characters (v5), 25 (v4), or 20 (v2 / v3)
    if (bits != SEED_ID_BITS && bits != SEED_ID_BITS_V4 && bits != SEED_ID_BITS_V3)
        return LN("A seed ID has 30 characters.");
    uint32_t sum = 0;
    for (int i = 0; i < 5; i++) sum |= (uint32_t)x.b[bits - 5 + i] << i;
    if (sum != id_check(x, bits - 5)) return LN("That ID has a typo somewhere.");
    x.n = 0;
    int ver = (int)x.get(4);
    if (bits == SEED_ID_BITS ? ver != SEED_ID_VERSION : bits == SEED_ID_BITS_V4 ? ver != 4 : ver != 2 && ver != 3)
        return LN("This ID is from another Multi version.");
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
    if (bits == SEED_ID_BITS) { x.n = id_startinst(bits); t.start_instruments = (int)x.get(4); }
    else t.start_instruments = 0;
    if (t.room_shuffle > 3 || t.shopsanity > 2 || t.logic > 4 || t.traps > 4 || t.seashells > 5 || t.tunic >= 14 ||
        t.entrances > 3 || t.start_rupees > 999 || t.tri_needed > 50 || t.tri_extra > 2 ||
        t.start_instruments > 8)
        return LN("Not a valid seed ID.");
    t.ohko = false;
    // retired choices (older IDs still load): logic is always basic, story scenes always skipped, tier 3
    // (+ connector caves) is part of tier 2 now, and the Coherent room shuffle (1) is Coupled
    t.logic = 0;
    t.skip_story = true;
    t.tunic = 0;  // (the tunic colour is the Cosmetics tab's, live: new seeds stay green; placement never used it)
    if (t.entrances > 2) t.entrances = 2;
    if (t.room_shuffle == 1) t.room_shuffle = 2;
    if (t.miniboss_freq == 2) t.miniboss_freq = 3;  // (the retired 3x reads as All)
    t.retired_key_rings = false;
    t.retired_labyrinth = false;
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
    for (int i = 0; i < 5; i++) r += std::string(i ? ", " : "") + L(kSeedHashIcons[h[i]].name);
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
    if (!f) { g_gen_status = strf(L("Couldn't write %s."), req.c_str()); g_gen_failed = g_gen_done = true; return; }
    fprintf(f, "{\"game-version\": \"%s\",", game_version_name(game_version_current()));
    fprintf(f, "\"seed\": \"%s\", \"logic\": \"basic\", \"dungeon-items\": \"%s\", \"traps\": \"%s\",\n", id,
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
        if (!g_opts.no_start_ocarina) st += "\"ocarina\", ";
        if (!g_opts.no_start_mambo) st += "\"song-mambo\", ";
        if (!st.empty()) st.resize(st.size() - 2);
        fprintf(f, " \"starting-items\": [%s],\n", st.c_str());
    }
    fprintf(f, " \"pots\": %s, \"no-buff-drops\": %s, \"bushes\": %s, \"digs\": %s,\n", g_opts.pots ? "true" : "false",
            g_opts.no_buffs ? "true" : "false", g_opts.bushes ? "true" : "false", g_opts.digs ? "true" : "false");
    if (g_opts.rocks) fprintf(f, " \"rocks\": true,\n");  // (off: not written, seeds as before)
    if (g_opts.weather_cycle) fprintf(f, " \"weather-cycle\": true,\n");  // (likewise)
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
    if (g_opts.chest_looks) fprintf(f, " \"chest-looks\": true,\n");  // (off: not written, seeds as before)
    {  // what's given at the start, each kind on its own (the LAS setting above when maps and compasses match)
        bool beak = g_opts.dungeon_items & 1, map = g_opts.dungeon_items & 2, compass = map != g_opts.compass_differs;
        fprintf(f, " \"dungeon-items-start\": [%s%s%s],\n", map ? "\"map\"" : "",
                compass ? (map ? ", \"compass\"" : "\"compass\"") : "",
                beak ? (map || compass ? ", \"stone-beak\"" : "\"stone-beak\"") : "");
    }
    static const char *rs[] = {"off", "coupled", "coupled", "decoupled"};  // (1: retired Coherent, never written)
    fprintf(f, " \"dungeon-room-shuffle\": \"%s\",\n", rs[g_opts.room_shuffle]);
    static const char *shop[] = {"off", "vanilla", "random"};
    fprintf(f, " \"shopsanity\": \"%s\", \"trendsanity\": %s,\n", shop[g_opts.shopsanity], g_opts.trendsanity ? "true" : "false");
    fprintf(f, " \"stealing\": \"%s\",\n", !g_opts.shopsanity || g_opts.steal_needs_sword && !g_opts.no_stealing ? "normal"
                                          : g_opts.no_stealing ? "never" : "always");  // (Fast Stealing: with Shopsanity)
    fprintf(f, " \"boss-souls\": %s, \"miniboss-souls\": %s,\n", g_opts.boss_souls ? "true" : "false",
            g_opts.miniboss_souls ? "true" : "false");
    fprintf(f, " \"hookshot-anything\": %s,\n", g_opts.hookshot_any ? "true" : "false");
    fprintf(f, " \"race-mode\": %s,\n", g_opts.race ? "true" : "false");  // (runtime only: spoiler lock, seed hash)
    if (g_opts.fast_egg) fprintf(f, " \"fast-egg\": true,\n");  // (off: not written, seeds as before)
    if (g_opts.hints) fprintf(f, " \"hints\": true,\n");  // (only when on: seeds without it stay as they were)
    fprintf(f, " \"triforce-hunt\": %d, \"triforce-extra\": %d,\n", g_opts.tri_needed, g_opts.tri_extra * 25);
    fprintf(f, " \"starting-instruments\": %d,\n", g_opts.start_instruments);
    fprintf(f, " \"entrance-tier\": %d, \"entrance-decoupled\": %s,\n", g_opts.entrances, g_opts.entrances >= 2 && g_opts.decoupled ? "true" : "false");
    fprintf(f, " \"pet-rooster\": %s, \"pet-ghost\": %s, \"open-desert\": %s,\n", g_opts.pet_rooster ? "true" : "false",
            g_opts.pet_ghost ? "true" : "false", g_opts.pet_marin ? "false" : "true");
    fprintf(f, " \"include\": [\"5-seashell-reward\", \"15-seashell-reward\", \"30-seashell-reward\", \"40-seashell-reward\", \"50-seashell-reward\"],\n");
    fprintf(f, " \"exclude\": [%s]}\n", shell_excl[g_opts.seashells]);
    fclose(f);
    std::string log = dir + "/generate.log";
    std::string romfs = g_start_screen ? g_source_romfs : g_romfs_dir ? g_romfs_dir : g_source_romfs;
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
    g_gen_status = rc ? L("Couldn't start the seed generator. rando-gen might be missing.") : L("Starting...");
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
        if (sp != std::string::npos) g_gen_status = L_line(line.substr(sp + 1)) + "...";
    }
    int st = plat_proc_wait(g_gen_pid, false);
    if (st >= 0) {
        g_gen_pid = 0;
        g_gen_failed = st != 0;
        g_gen_done = true;
        g_gen_status = g_gen_failed ? strf(L("Seed generation failed. The details are in rando/%s/generate.log."), g_gen_seed.c_str())
                                    : strf(L("Seed \"%s\" ready."), g_gen_seed.c_str());
        size_t e = g_gen_failed ? log.rfind("\nerror ") : std::string::npos;  // (a reason the player can act on)
        if (e != std::string::npos) g_gen_status = L_line(log.substr(e + 7, log.find('\n', e + 1) - e - 7));
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
        ImGui::TextUnformatted(ui_tr(g_gen_status.c_str()));
        ImGui::ProgressBar(g_gen_frac > 0.005f ? g_gen_frac : -0.3f * (float)ImGui::GetTime(), ImVec2(-1, 0), over);
    } else {
        float bw = 230 * scale;
        bool close = false;
        if (g_gen_failed) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(plastic_col(plastic_theme().warn), "%s", g_gen_status.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button(L("Close"), ImVec2(bw, 0))) close = true;
            ImGui::SetItemDefaultFocus();
        } else {
            ImGui::TextUnformatted(L("Seed ready."));
            ImGui::ProgressBar(1, ImVec2(-1, 0), L("Done"));
            ImGui::Spacing();
            if (ImGui::Button(L("Play now"), ImVec2(bw, 0)) || test_play) {  // (switches in place: the game reboots on the new seed)
                g_gen_done = false;
                g_gen_status.clear();
                ImGui::CloseCurrentPopup();
                play_seed(g_gen_seed.c_str());
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button(L("Close"), ImVec2(bw, 0))) close = true;
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
enum { CAT_SEEDS, CAT_PRESETS, CAT_GENERAL, CAT_GOAL, CAT_ITEMS, CAT_EXTRA, CAT_WORLD, CAT_ENEMIES, CAT_COMPANIONS, CAT_FUN, CAT_OTHER, N_CAT };
static const char *const k_cat_name[N_CAT] = {LN("Seeds"), LN("Presets"), LN("General"), LN("Goal"), LN("Items"),
                                              LN("Extra checks"), LN("World"), LN("Enemies & bosses"), LN("Companions"),
                                              LN("Fun and Wacky"), LN("Other")};
static int g_cat = CAT_GENERAL;
static bool g_in_right;            // the cursor is in the right column (last frame)
static int g_last_frame = -10;     // (a frame without this tab: the cursor starts over)
static std::string g_act_seed;     // the seed whose actions the right column shows ("": the seed list)
static std::string g_focus_seed;   // (FOCUS_SEED: this seed's row)
static bool g_confirm_delete;
enum { FOCUS_NONE, FOCUS_CAT, FOCUS_RIGHT, FOCUS_SEED, FOCUS_DELETE, FOCUS_CANCEL, FOCUS_TRI, FOCUS_PRESET, FOCUS_SAVE_AS };
static int g_focus;                // put the cursor on this row when it's drawn

static void take_focus(int want) {
    if (g_focus != want || opt_collecting()) return;
    g_focus = FOCUS_NONE;
    opt_focus_item();
}

// ---- the player's own rows (not part of a seed): in their categories and on the in-game seed sheet
static void row_tracker(void) {
    opt_own_next();
    if (opt_toggle("Overworld and Dungeon Check Tracker", &g_settings.tracker, true,
                   "The overworld and dungeon maps show which checks you can reach with your items."))
        settings_save();
}

// its doorway letters on the dungeon map (runtime/ui/chaos_overlay.mm): on = as you explore, every pair once you have
// the map (CHAOS_LETTERS_MAP); off = none. (room_shuffle: the seed's, greyed out without it)
static void row_door_letters(bool room_shuffle) {
    bool on = g_settings.chaos_letters != CHAOS_LETTERS_OFF;
    if (!room_shuffle) opt_disable_next();
    opt_own_next();
    if (opt_toggle("Map tracks door connections", &on, true,
                   "Letter pairs on the dungeon map show which doorways connect. A pair shows up once you've used "
                   "either door, and you see them all once you have the map.")) {
        g_settings.chaos_letters = on ? CHAOS_LETTERS_MAP : CHAOS_LETTERS_OFF;
        settings_save();
    }
}

// ---- the categories' rows
static void rows_general(void) {
    {  // starting items (part of the seed)
        bool sw = !g_opts.no_start_sword, sh = !g_opts.no_start_shield, oc = !g_opts.no_start_ocarina,
             mb = !g_opts.no_start_mambo;
        if (opt_toggle("Start with Sword", &sw, true,
                       "Link starts with the sword. Turn it off to find it somewhere in the world."))
            g_opts.no_start_sword = !sw;
        if (opt_toggle("Start with Shield", &sh, true,
                       "Link starts with the shield. Turn it off to find it somewhere in the world."))
            g_opts.no_start_shield = !sh;
        if (opt_toggle("Start with Ocarina", &oc, true, "Link starts with the Ocarina.")) g_opts.no_start_ocarina = !oc;
        if (opt_toggle("Start with Manbo's Mambo", &mb, true, "Link starts with Manbo's Mambo.")) g_opts.no_start_mambo = !mb;
    }
    opt_toggle("Treat Tunics as Upgrades", &g_opts.tunic_upgrades, false,
               "The tunic effects stack, so with both tunics Link has double defense and double attack.");
    opt_toggle("Disable Guardian Acorns / Pieces of Power", &g_opts.no_buffs, false,
               "Disables Guardian Acorns and Pieces of Power from spawning.");
    row_tracker();
    opt_toggle("Fast Egg", &g_opts.fast_egg, true, "First drop in the Wind Fish Egg drops you to the boss.");
}

static void rows_goal(void) {
    opt_int("Starting Instruments", &g_opts.start_instruments, 0, 8, 1, 0, "%d", "How many instruments you start with.");
    bool hunt = g_opts.tri_needed > 0;
    if (opt_toggle("Triforce hunt", &hunt, false,
                   "Collect Triforce pieces to win. The last one you need rolls the credits, no Wind Fish required."))
        g_opts.tri_needed = hunt ? (g_tri_mode < 3 ? kTriPresets[g_tri_mode] : g_tri_custom) : 0;
    static const char *const pn[] = {"3", "12", "24", LN("Custom")};
    if (!hunt) opt_disable_next();
    if (opt_choice("Pieces needed", &g_tri_mode, pn, 4, 2, "How many Triforce pieces you need to win.", false))
        g_opts.tri_needed = g_tri_mode < 3 ? kTriPresets[g_tri_mode] : g_tri_custom;
    if (hunt && g_tri_mode == 3) {  // Custom: the count
        int v = g_tri_custom;
        if (opt_int("Custom", &v, 1, 50, 1, 24, "%d", "Pick any number from 1 to 50."))
            g_tri_custom = v, g_opts.tri_needed = v;
        take_focus(FOCUS_TRI);
    }
    static const char *const ex[] = {LN("None"), "+25%", "+50%"};
    if (!hunt) opt_disable_next();
    opt_choice("Extra pieces", &g_opts.tri_extra, ex, 3, 0,
               "Hides extra pieces on top of the ones you need, so you can skip some.");
}

static void rows_items(void) {
    {  // dungeon items: each kind shuffled (on) or given at the start (dungeon_items bits + compass_differs)
        bool beak = !(g_opts.dungeon_items & 1), map = !(g_opts.dungeon_items & 2), compass = map != g_opts.compass_differs;
        opt_section("Dungeon items shuffled");
        bool ch = opt_toggle("Stone Beaks", &beak, true, "Turn off to start with every dungeon's Stone Beak.");
        ch |= opt_toggle("Maps", &map, true, "Turn off to start with every dungeon's map.");
        ch |= opt_toggle("Compasses", &compass, true, "Turn off to start with every dungeon's compass.");
        if (ch) g_opts.dungeon_items = (beak ? 0 : 1) | (map ? 0 : 2), g_opts.compass_differs = map != compass;
        opt_section("Other items");
    }
    opt_toggle("Hookshot anything", &g_opts.hookshot_any, false,
               "The Hookshot latches onto any wall or object it hits, not only Hookshot targets. The logic counts the "
               "gaps this can help cross that a normal Hookshot couldn't.");
    opt_toggle("Bomb Bag", &g_opts.bomb_bag, false, "You must find your bomb bag to collect and carry bombs.");
    opt_toggle("Powder Bag", &g_opts.powder_bag, false, "You must find your powder bag to collect and carry Magic Powder.");
    static const char *traps[] = {LN("None"), LN("Few"), LN("Several"), LN("Many"), LN("Trapsanity")};
    opt_choice("Traps", &g_opts.traps, traps, 5, 1, "How many traps hide among the items.");
    opt_toggle("Chest looks match contents", &g_opts.chest_looks, false,
               "Chests with progression items look normal, key chests are blue and everything else is brown.");
    static const char *shells[] = {LN("None"), LN("Up to 5"), LN("Up to 15"), LN("Up to 30"), LN("Up to 40"), LN("All (50)")};
    opt_choice("Seashell Mansion rewards", &g_opts.seashells, shells, 6, 2,
               "How many seashells a progression item could be locked behind.");
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
               "Every bush on the island hides an item, about 310 more checks. Bushes you haven't cut yet are golden.");
    opt_toggle("Digsanity", &g_opts.digs, false,
               "Every diggable tile on the overworld is a check, about 5,000 more. Holes stay dug.");
    opt_toggle("Shellsanity", &g_opts.shellsanity, false, "Every hidden Secret Seashell spot holds an item from the pool.");
    opt_toggle("Applesanity", &g_opts.apples, false, "Each apple tree holds 3 items from the pool.");
    opt_toggle("Rocksanity", &g_opts.rocks, false, "Every rock you can lift on the overworld hides an item from the pool.");
    static const char *ksn[] = {LN("Off"), LN("Small Keys"), LN("Small + Nightmare"), LN("All dungeon items")};
    opt_choice("Keysanity", &g_opts.keysanity, ksn, 4, 0,
               "Dungeon keys can be anywhere. The last choice does this for every dungeon item.");
    opt_toggle("Potsanity", &g_opts.pots, false,
               "Every pot you can lift in houses, caves and dungeons hides an item, about 400 more checks. Pots you "
               "haven't checked yet are golden.");
    static const char *shopn[] = {LN("Off"), LN("Vanilla prices"), LN("Random prices")};
    opt_choice("Shopsanity", &g_opts.shopsanity, shopn, 3, 0, "Everything in the Mabe Village shop is a check.");
    {  // stealing (LAS 'stealing' never / normal / always), only with Shopsanity
        bool steal = !g_opts.no_stealing, fast = !g_opts.steal_needs_sword;
        if (!g_opts.shopsanity) opt_disable_next();
        if (opt_toggle("Stealing", &steal, true, "Turns on or off the ability to steal.")) g_opts.no_stealing = !steal;
        if (!g_opts.shopsanity || g_opts.no_stealing) opt_disable_next();
        if (opt_toggle("Fast Stealing", &fast, false, "Fast Stealing doesn't require the Sword.")) g_opts.steal_needs_sword = !fast;
    }
    opt_toggle("Trendy Game prizes", &g_opts.trendsanity, false,
               "Each of the 5 crane prize spots holds a check. Once you win one, it goes back to normal prizes.");
    opt_toggle("Fishing prizes", &g_opts.inc_fishing, false, "The fishing pond's prizes hold items from the pool.");
    opt_toggle("Rapids race prizes", &g_opts.inc_rapids, false, "The Rapids Ride race's prizes hold items from the pool.");
    opt_toggle("Dampe's rewards", &g_opts.inc_dampe, false, "Dampe's Chamber Dungeon challenge rewards hold items from the pool.");
    opt_toggle("Trade Quest Items", &g_opts.inc_trade, false, "Progression can lie behind the Trade Quest.");
    opt_toggle("Color Dungeon Blupees", &g_opts.inc_blue, false, "The Color Dungeon's Blupees hold items from the pool.");
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
        if (!on) opt_disable_next();
        if (opt_toggle("Dungeon entrances", &dungeons, false, "Dungeon entrances lead into other dungeons.")) {
            if (!dungeons && !houses) last = true;
            else g_opts.shuffle_dungeons = dungeons;
        }
        if (!on) opt_disable_next();
        if (opt_toggle("Houses & caves", &houses, false,
                       "Shuffles houses, caves and the caves that link two areas. Marin & Tarin's house and water "
                       "entrances stay vanilla.")) {
            if (!houses && !dungeons) last = true;
            else g_opts.entrances = houses ? 2 : 0;
        }
        if (!houses) opt_disable_next();
        opt_toggle("Decoupled entrances", &g_opts.decoupled, false,
                   "Leaving a house drops you outside a different door than the one you came in.");
        if (last) ImGui::OpenPopup("##entlast");
        if (!opt_collecting() && ImGui::BeginPopupModal("##entlast", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::TextUnformatted(L("Keep at least one kind of entrance shuffled, or turn the entrance randomizer off."));
            ImGui::Spacing();
            ImGui::SetItemDefaultFocus();
            if (ImGui::Button(L("OK"), ImVec2(220 * scale, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
    {  // dungeon room shuffle (dungeon-room-shuffle; seed ID 0 off / 2 coupled / 3 decoupled, 1 = the retired
       // Coherent, read as Coupled): Off by default
        static const char *names[] = {LN("Off"), LN("Coupled"), LN("Decoupled")};
        static const int ids[] = {0, 2, 3};
        int i = g_opts.room_shuffle == 3 ? 2 : g_opts.room_shuffle ? 1 : 0;
        if (opt_choice("Dungeon room shuffle", &i, names, 3, 0,
                       "Experimental. Shuffles dungeon rooms, creating new layouts."))
            g_opts.room_shuffle = ids[i];
    }
    row_door_letters(g_opts.room_shuffle != 0);
    opt_toggle("Shuffle instruments", &g_opts.shuffle_instruments, false,
               "The Siren Instruments are items in the pool instead of each dungeon's reward.");
    opt_toggle("Open Mabe Village", &g_opts.open_mabe, false, "Mabe Village's paths are open from the start.");
}

static void rows_enemies(void) {
    opt_toggle("Randomize enemies", &g_opts.randomize_enemies, false, "Shuffles enemies around the island.");
    {
        if (!g_opts.randomize_enemies) opt_disable_next();
        static const char *const modes[] = {LN("Seeded"), LN("Random")};
        opt_choice("Mode", &g_opts.enemy_mode, modes, 2, 0,
                   "Seeded keeps enemies the same every time. Random rerolls them whenever an area loads.");
    }
    if (!g_opts.randomize_enemies) opt_disable_next();
    opt_toggle("Include mini-bosses", &g_opts.enemy_minibosses, false,
               "Mini-bosses join the normal enemy pool. Only some appear indoors, since the rest don't behave well in "
               "small rooms.");
    {  // (miniboss-frequency 0 normal / 1 double / 3 all: the retired 3x, 2, reads as All)
        if (!g_opts.randomize_enemies || !g_opts.enemy_minibosses) opt_disable_next();
        static const char *const freq[] = {LN("Normal"), LN("Double"), LN("All")};
        static const int ids[] = {0, 1, 3};
        int i = g_opts.miniboss_freq >= 2 ? 2 : g_opts.miniboss_freq;
        if (opt_choice("Mini-boss frequency", &i, freq, 3, 0,
                       "How often overworld enemies turn into mini-bosses. All swaps every enemy that has room."))
            g_opts.miniboss_freq = ids[i];
    }
    opt_toggle("Boss souls", &g_opts.boss_souls, false,
               "Each boss has a soul hidden in the item pool, Color Dungeon included. The boss won't show up until you "
               "find its soul, so that instrument stays locked. Found souls show on the Items screen.");
    opt_toggle("Mini-boss souls", &g_opts.miniboss_souls, false,
               "Each kind of mini-boss has a soul hidden in the item pool. Without it, that mini-boss won't show up "
               "and its room stays shut. One soul covers every fight with that kind.");
}

static void rows_companions(void) {
    opt_toggle("Flying Rooster", &g_opts.pet_rooster, false,
               "The Flying Rooster is an item. Once you find him he follows you, and the logic lets him stand in for "
               "Roc's Feather or the Hookshot.");
    opt_toggle("Ghost", &g_opts.pet_ghost, false,
               "The Ghost is an item. Once you find him, walk him to his grave for a check. He won't show up on his "
               "own after the Surf Harp.");
    opt_toggle("Marin", &g_opts.pet_marin, false,
               "Marin is an item in the pool, and the desert is blocked without her. She decides to stay with you after "
               "waking the Walrus.");
    opt_toggle("Companions allowed in dungeons", &g_opts.pets_in_dungeons, false,
               "Companions follow Link into dungeons, and the logic counts on what they can do there, like the Flying "
               "Rooster standing in for Roc's Feather.");
}

static void rows_fun(void) {
    opt_toggle("Weather Cycle", &g_opts.weather_cycle, false,
               "Adds a weather cycle, including storm and blizzard hazards.");
    opt_toggle("Randomize music", &g_opts.randomize_music, false, "Shuffles the game's music.");
    opt_toggle("Randomize Cosmetics", &g_opts.random_cosmetics, false, "Randomizes cosmetics to the seed.");
    opt_toggle("Randomized Game Environments", &g_opts.environments, false,
               "The game has many built-in room shaders, like the desert, the Egg and the Mysterious Woods, some of them "
               "unused. The seed gives each area a random one.");
}

static void rows_other(void) {
    opt_toggle("Hints", &g_opts.hints, false, "Owl statues and phone booths give hints.");
    opt_toggle("Fast songs", &g_opts.fast_songs, true, "Learning a song skips its cutscene.");
    opt_toggle("Boss cutscenes", &g_opts.boss_cutscenes, false, "Plays each boss's intro cutscene.");
    opt_toggle("Spoiler log", &g_opts.spoiler, true,
               "Saves a list of where every item is. Open it from the seed's Spoiler log.");
    opt_toggle("Race mode", &g_opts.race, false,
               "Locks the spoiler log until the credits and shows a seed hash on the title screen, so racers can check "
               "they're on the same seed.");
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
    case CAT_FUN: rows_fun(); break;
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
        items.push_back({true, L(k_cat_name[c]), "", 0});
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
    p.y += draw(p, W, {false, L("Seed"), g_opts.seed[0] ? g_opts.seed : L("Random"), 0});
    p.y += draw(p, W, {false, L("Seed ID"), id, 0});
    p.y += gap;
    // two columns while a middling entry still fits on one line of one (the longer ones go on under themselves)
    std::string mid = std::string(L("Seashell Mansion rewards")) + ": " + L("All (50)");
    float mid_w = font->CalcTextSizeA(fs, FLT_MAX, 0, mid.c_str()).x + fs * 0.5f;
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
            std::string t = i18n_upper(it.a.c_str());
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

// ---- Presets: four built-in ones (difficulty options only, by option name) and the player's own (every option: the
// options key, as rando_options, plus the two player settings on the tabs' pages). Applying one keeps the seed's name
// and random part, and leaves one step of Undo.
struct OptName { const char *name; bool GenOpts::*b; int GenOpts::*i; bool inv; };
static const OptName kOptNames[] = {  // (names the built-in presets use; inv: the field stores "not")
    {"start_sword", &GenOpts::no_start_sword, nullptr, true}, {"start_shield", &GenOpts::no_start_shield, nullptr, true},
    {"start_ocarina", &GenOpts::no_start_ocarina, nullptr, true},
    {"start_mambo", &GenOpts::no_start_mambo, nullptr, true},
    {"steal_without_sword", &GenOpts::steal_needs_sword, nullptr, true},
    {"dungeon_items", nullptr, &GenOpts::dungeon_items}, {"compass_differs", &GenOpts::compass_differs}, {"keysanity", nullptr, &GenOpts::keysanity},
    {"bomb_bag", &GenOpts::bomb_bag}, {"powder_bag", &GenOpts::powder_bag}, {"traps", nullptr, &GenOpts::traps},
    {"seashells", nullptr, &GenOpts::seashells}, {"start_rupees", nullptr, &GenOpts::start_rupees},
    {"bushes", &GenOpts::bushes}, {"digs", &GenOpts::digs}, {"shellsanity", &GenOpts::shellsanity},
    {"apples", &GenOpts::apples}, {"pots", &GenOpts::pots}, {"shopsanity", nullptr, &GenOpts::shopsanity},
    {"stealing", &GenOpts::no_stealing, nullptr, true}, {"trendsanity", &GenOpts::trendsanity},
    {"fishing", &GenOpts::inc_fishing}, {"rapids", &GenOpts::inc_rapids}, {"dampe", &GenOpts::inc_dampe},
    {"trade", &GenOpts::inc_trade}, {"blupees", &GenOpts::inc_blue},
    {"entrance_dungeons", &GenOpts::shuffle_dungeons}, {"entrance_houses", nullptr, &GenOpts::entrances},
    {"decoupled", &GenOpts::decoupled}, {"room_shuffle", nullptr, &GenOpts::room_shuffle},
    {"shuffle_instruments", &GenOpts::shuffle_instruments}, {"open_mabe", &GenOpts::open_mabe},
    {"randomize_enemies", &GenOpts::randomize_enemies}, {"enemy_mode", nullptr, &GenOpts::enemy_mode},
    {"enemy_minibosses", &GenOpts::enemy_minibosses}, {"miniboss_freq", nullptr, &GenOpts::miniboss_freq},
    {"boss_souls", &GenOpts::boss_souls}, {"miniboss_souls", &GenOpts::miniboss_souls}, {"no_buffs", &GenOpts::no_buffs},
    {"hints", &GenOpts::hints}, {"chest_looks", &GenOpts::chest_looks},
    {"rocks", &GenOpts::rocks},
};
static const OptName *opt_by_name(const char *n) {
    for (const OptName &o : kOptNames) if (!strcmp(o.name, n)) return &o;
    return nullptr;
}

struct PresetVal { const char *opt; int v; };
struct Builtin { const char *name, *desc; std::vector<PresetVal> vals; };
static const std::vector<Builtin> &builtins(void) {
    // every row: Easy, Standard, Hard, Chaos. Taste and comfort options (music, cosmetics, environments, spoiler, race,
    // companions, Hookshot anything, Triforce hunt, Fast songs, Boss cutscenes, Treat Tunics as Upgrades) are left alone.
    static const struct { const char *opt; int v[4]; } t[] = {
        {"start_sword", {1, 1, 1, 0}}, {"start_shield", {1, 1, 0, 0}}, {"start_ocarina", {1, 1, 1, 0}},
        {"start_mambo", {1, 1, 1, 0}},
        {"steal_without_sword", {1, 0, 0, 0}},
        {"dungeon_items", {3, 0, 0, 0}}, {"compass_differs", {0, 0, 0, 0}}, {"keysanity", {0, 0, 2, 3}},
        {"bomb_bag", {0, 0, 1, 1}}, {"powder_bag", {0, 0, 1, 1}}, {"traps", {0, 1, 2, 3}}, {"seashells", {2, 2, 3, 5}},
        {"start_rupees", {200, 0, 0, 0}}, {"hints", {1, 1, 1, 0}}, {"chest_looks", {1, 1, 0, 0}},
        {"bushes", {0, 0, 0, 1}}, {"digs", {0, 0, 0, 1}}, {"rocks", {0, 0, 0, 1}}, {"shellsanity", {0, 0, 0, 1}},
        {"apples", {0, 0, 0, 1}}, {"pots", {0, 0, 0, 1}}, {"shopsanity", {0, 0, 2, 2}}, {"stealing", {1, 1, 1, 0}},
        {"trendsanity", {0, 0, 0, 1}}, {"fishing", {0, 0, 0, 1}}, {"rapids", {0, 0, 0, 1}}, {"dampe", {0, 0, 0, 1}},
        {"trade", {0, 0, 0, 1}}, {"blupees", {0, 0, 0, 1}},
        {"entrance_dungeons", {0, 0, 1, 1}}, {"entrance_houses", {0, 0, 2, 2}}, {"decoupled", {0, 0, 0, 1}},
        {"room_shuffle", {0, 0, 2, 3}}, {"shuffle_instruments", {0, 0, 1, 1}},
        {"open_mabe", {1, 0, 0, 0}}, {"randomize_enemies", {0, 0, 1, 1}}, {"enemy_mode", {0, 0, 0, 1}},
        {"enemy_minibosses", {0, 0, 0, 1}}, {"miniboss_freq", {0, 0, 0, 1}},
        {"boss_souls", {0, 0, 1, 1}}, {"miniboss_souls", {0, 0, 0, 1}}, {"no_buffs", {0, 0, 1, 1}},
    };
    static std::vector<Builtin> b = {
        {LN("Easy"), LN("Dungeon items from the start, no traps, Mabe Village open, 200 rupees."), {}},
        {LN("Standard"), LN("The default options, plus hints and chests that match their contents."), {}},
        {LN("Hard"), LN("Keysanity, shuffled entrances, rooms and instruments, random enemies, boss souls."), {}},
        {LN("Chaos"), LN("Every extra check and every shuffle."), {}},
    };
    if (b[0].vals.empty())
        for (auto &r : t) for (int k = 0; k < 4; k++) b[k].vals.push_back({r.opt, r.v[k]});
    return b;
}

static void builtin_apply(const Builtin &p, GenOpts &o) {
    for (const PresetVal &v : p.vals) {
        const OptName *n = opt_by_name(v.opt);
        if (!n) continue;  // (an option this build doesn't have)
        if (n->b) o.*(n->b) = n->inv ? !v.v : v.v != 0;
        else o.*(n->i) = v.v;
    }
}

static bool builtin_matches(const Builtin &p, const GenOpts &o) {
    for (const PresetVal &v : p.vals) {
        const OptName *n = opt_by_name(v.opt);
        if (!n) continue;
        if (n->b ? (n->inv ? !(o.*(n->b)) : o.*(n->b)) != (v.v != 0) : o.*(n->i) != v.v) return false;
    }
    return true;
}

struct UserPreset { std::string name, key; int tracker = -1, letters = -1; };
static std::vector<UserPreset> user_presets(void) {
    std::vector<UserPreset> v;
    for (int i = 0; i < RANDO_PRESETS_MAX; i++) {
        const char *s = g_settings.rando_presets[i], *a = strchr(s, '|');
        if (!s[0] || !a) continue;
        UserPreset p;
        p.name.assign(s, a - s);
        const char *b = strchr(a + 1, '|');
        p.key.assign(a + 1, b ? (size_t)(b - a - 1) : strlen(a + 1));
        if (b && isdigit((unsigned char)b[1]) && isdigit((unsigned char)b[2])) p.tracker = b[1] - '0', p.letters = b[2] - '0';
        v.push_back(p);
    }
    return v;
}

static void user_presets_store(const std::vector<UserPreset> &v) {
    memset(g_settings.rando_presets, 0, sizeof g_settings.rando_presets);
    for (size_t i = 0; i < v.size() && i < RANDO_PRESETS_MAX; i++) {
        char t[3] = "";
        if (v[i].tracker >= 0) snprintf(t, sizeof t, "%d%d", v[i].tracker, v[i].letters);
        snprintf(g_settings.rando_presets[i], sizeof g_settings.rando_presets[i], "%s|%s|%s", v[i].name.c_str(),
                 v[i].key.c_str(), t);
    }
    settings_save();
}

static UserPreset current_as_preset(const char *name) {
    UserPreset p;
    p.name = name;
    char k[48];
    opts_key(g_opts, k, sizeof k);
    p.key = k;
    p.tracker = g_settings.tracker ? 1 : 0;
    p.letters = g_settings.chaos_letters;
    return p;
}

static bool user_matches(const UserPreset &p) {
    char k[48];
    opts_key(g_opts, k, sizeof k);
    return p.key == k && (p.tracker < 0 || (p.tracker == (g_settings.tracker ? 1 : 0) && p.letters == g_settings.chaos_letters));
}

// a preset name: printable, no '|' (the line's separator), up to 32 characters, no spaces at the ends
static std::string clean_preset_name(const char *in) {
    std::string n;
    for (const char *p = in; *p && n.size() < 32; p++)
        if ((unsigned char)*p >= 0x20 && *p != '|' && *p != 0x7f) n += *p;
    while (!n.empty() && n.back() == ' ') n.pop_back();
    while (!n.empty() && n[0] == ' ') n.erase(0, 1);
    return n;
}

// the one step of Undo: the options before the last preset, shown while the options are still the preset's
static struct { bool on; GenOpts opts; bool tracker; int letters; char after[48]; bool tracker_after; int letters_after; } g_undo;

static void undo_mark(void) {
    g_undo.on = true;
    g_undo.opts = g_opts;
    g_undo.tracker = g_settings.tracker, g_undo.letters = g_settings.chaos_letters;
}
static void undo_mark_after(void) {
    opts_key(g_opts, g_undo.after, sizeof g_undo.after);
    g_undo.tracker_after = g_settings.tracker, g_undo.letters_after = g_settings.chaos_letters;
}
static bool undo_available(void) {
    if (!g_undo.on) return false;
    char k[48];
    opts_key(g_opts, k, sizeof k);
    return !strcmp(k, g_undo.after) && g_undo.tracker_after == g_settings.tracker && g_undo.letters_after == g_settings.chaos_letters;
}

static void set_opts_keep_seed(const GenOpts &t) {  // (the seed's name and random part stay)
    GenOpts n = t;
    strlcpy(n.seed, g_opts.seed, sizeof n.seed);
    n.rng = g_opts.rng;
    g_opts = n;
    tri_mode_from_opts();
}

static void apply_builtin(const Builtin &p) {
    undo_mark();
    GenOpts t = g_opts;
    builtin_apply(p, t);
    set_opts_keep_seed(t);
    undo_mark_after();
    LOG("[rando] preset %s\n", p.name);
}

static bool apply_user(const UserPreset &p) {
    GenOpts t = g_opts;
    std::string err = seed_id_decode(p.key.c_str(), t);
    if (!err.empty()) { LOG("[rando] preset %s not loaded: %s\n", p.name.c_str(), err.c_str()); return false; }
    undo_mark();
    set_opts_keep_seed(t);
    bool save = false;
    if (p.tracker >= 0 && (g_settings.tracker ? 1 : 0) != p.tracker) g_settings.tracker = p.tracker != 0, save = true;
    if (p.letters >= 0 && p.letters <= CHAOS_LETTERS_MAP && g_settings.chaos_letters != p.letters) g_settings.chaos_letters = p.letters, save = true;
    if (save) settings_save();
    undo_mark_after();
    LOG("[rando] preset %s (yours)\n", p.name.c_str());
    return true;
}

static void undo_preset(void) {
    set_opts_keep_seed(g_undo.opts);
    if (g_settings.tracker != g_undo.tracker || g_settings.chaos_letters != g_undo.letters) {
        g_settings.tracker = g_undo.tracker, g_settings.chaos_letters = g_undo.letters;
        settings_save();
    }
    g_undo.on = false;
}

// the preset the options match now ("" none): one of the player's (every option) first, else a built-in
static std::string current_preset(void) {
    for (const UserPreset &p : user_presets()) if (user_matches(p)) return p.name;
    for (const Builtin &b : builtins()) if (builtin_matches(b, g_opts)) return b.name;
    return "";
}

static std::string g_act_preset;       // the player's preset whose actions the right column shows ("": the list)
static std::string g_focus_preset;     // (FOCUS_PRESET: this preset's row)
static std::string g_replace_preset;   // Save as: a preset of this name exists, replace it?
static bool g_preset_confirm_delete;
static std::string g_preset_msg;       // (a name already taken)

static void rows_presets(void) {
    std::vector<UserPreset> mine = user_presets();
    if (!g_act_preset.empty()) {  // one of the player's: its actions
        int at = -1;
        for (size_t i = 0; i < mine.size(); i++) if (mine[i].name == g_act_preset) at = (int)i;
        if (at < 0) { g_act_preset.clear(), g_preset_confirm_delete = false; return; }
        if (g_preset_confirm_delete) {
            ImGui::TextColored(plastic_col(plastic_theme().warn), "%s", L("Delete this preset?"));
            ImGui::Spacing();
            opt_warn_next();
            if (opt_action("Delete preset", "", NULL)) {
                mine.erase(mine.begin() + at);
                user_presets_store(mine);
                g_act_preset.clear(), g_preset_confirm_delete = false, g_focus = FOCUS_RIGHT;
                return;
            }
            if (opt_action("Cancel", "", NULL)) g_preset_confirm_delete = false, g_focus = FOCUS_DELETE;
            take_focus(FOCUS_CANCEL);
            return;
        }
        bool cur = user_matches(mine[at]);
        if (cur) opt_disable_next();
        if (opt_action("Use", cur ? ui_tr("Current") : "", NULL)) apply_user(mine[at]);
        static char rename_buf[40];
        if (!opt_text_editing("Rename")) strlcpy(rename_buf, mine[at].name.c_str(), sizeof rename_buf);
        if (opt_text("Rename", rename_buf, sizeof rename_buf, "", NULL)) {
            std::string to = clean_preset_name(rename_buf);
            bool taken = false;
            for (const UserPreset &p : mine) if (p.name == to && p.name != mine[at].name) taken = true;
            for (const Builtin &b : builtins()) if (to == b.name) taken = true;
            if (taken) g_preset_msg = strf(L("A preset named \"%s\" already exists."), to.c_str());
            else if (!to.empty() && to != mine[at].name) {
                mine[at].name = to, g_act_preset = to, g_preset_msg.clear();
                user_presets_store(mine);
            }
        }
        if (cur) opt_disable_next();
        if (opt_action("Update to current", "", "Saves your current options into this preset.")) {
            mine[at] = current_as_preset(mine[at].name.c_str());
            user_presets_store(mine);
        }
        if (opt_action("Delete", "", NULL)) g_preset_confirm_delete = true, g_focus = FOCUS_CANCEL;
        take_focus(FOCUS_DELETE);
        if (!g_preset_msg.empty()) { opt_disable_next(); opt_shown_next(); opt_action(g_preset_msg.c_str(), "", NULL, false); }
        return;
    }
    if (!g_replace_preset.empty()) {  // Save as: the name exists
        ImGui::TextColored(plastic_col(plastic_theme().warn), L("Replace \"%s\"?"), g_replace_preset.c_str());
        ImGui::Spacing();
        if (opt_action("Replace", "", NULL)) {
            for (UserPreset &p : mine) if (p.name == g_replace_preset) p = current_as_preset(p.name.c_str());
            user_presets_store(mine);
            g_focus_preset = g_replace_preset, g_replace_preset.clear(), g_focus = FOCUS_PRESET;
            return;
        }
        if (opt_action("Cancel", "", NULL)) g_replace_preset.clear(), g_focus = FOCUS_SAVE_AS;
        take_focus(FOCUS_CANCEL);
        return;
    }
    std::string cur = current_preset();
    for (const Builtin &b : builtins())
        if (opt_action(ui_tr(b.name), cur == b.name ? ui_tr("Current") : "", b.desc, false)) apply_builtin(b);
    if (undo_available() && opt_action(ui_tr("Undo"), "", "Puts back the options you had before.", false)) {
        undo_preset();
        g_focus = FOCUS_RIGHT;
    }
    opt_section("Your presets");
    static char name_buf[40];
    bool full = mine.size() >= RANDO_PRESETS_MAX;
    if (full) opt_disable_next();
    if (opt_text("Save current as", name_buf, sizeof name_buf, "", full ?
                 "You've hit the 20 preset limit. Delete one first." : "Saves your current options under a name.")) {
        std::string n = clean_preset_name(name_buf);
        name_buf[0] = 0;
        bool builtin = false;
        for (const Builtin &b : builtins()) if (n == b.name) builtin = true;
        bool exists = false;
        for (const UserPreset &p : mine) if (p.name == n) exists = true;
        if (builtin) g_preset_msg = strf(L("\"%s\" is a built-in preset. Pick another name."), n.c_str());
        else if (exists) g_replace_preset = n, g_focus = FOCUS_CANCEL, g_preset_msg.clear();
        else if (!n.empty()) {
            mine.push_back(current_as_preset(n.c_str()));
            user_presets_store(mine);
            g_preset_msg.clear();
            LOG("[rando] preset saved: %s\n", n.c_str());
        }
    }
    take_focus(FOCUS_SAVE_AS);
    if (!g_preset_msg.empty()) { opt_disable_next(); opt_shown_next(); opt_action(g_preset_msg.c_str(), "", NULL, false); }
    for (const UserPreset &p : mine) {
        opt_shown_next();  // (the player's name for it)
        if (opt_action(p.name.c_str(), cur == p.name ? L("Current") : "", "Use, rename, update or delete it.", false))
            g_act_preset = p.name, g_preset_confirm_delete = false, g_preset_msg.clear(), g_focus = FOCUS_RIGHT;
        if (g_focus == FOCUS_PRESET && p.name == g_focus_preset) take_focus(FOCUS_PRESET);
    }
    if (g_focus == FOCUS_PRESET) g_focus = FOCUS_RIGHT;
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
            ImGui::TextColored(plastic_col(plastic_theme().warn), "%s", L("Delete this seed and its save files?"));
            ImGui::Spacing();
            opt_warn_next();
            if (opt_action("Delete seed and saves", "", NULL)) {
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
                if (!stat(to.c_str(), &sb)) g_gen_status = strf(L("A seed named \"%s\" already exists."), to_name.c_str());
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
    if (active && opt_action("Switch to vanilla", "", "Leave this seed and play the normal game. Your seed's saves "
                                                       "stay put."))
        play_seed("");
    for (const Seed &s : g_seeds) {
        bool is_active = active && s.name == active;
        std::string d = strf(L("Seed ID: %s"), s.id.empty() ? "-" : s.id.c_str()) +
                        (s.race ? "\n" + strf(L("Race mode. Seed hash: %s"), hash_names(s.id).c_str()) : std::string()) +
                        "\n\n" + L("Play it, read its spoiler log, copy its ID to share, rename or delete it. Each seed "
                                     "has its own save files.");
        opt_shown_next();  // (the seed's name, as the player gave it)
        if (opt_action(s.name.c_str(), is_active ? L("Playing") : "", d.c_str(), false))
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
        ImGui::TextDisabled(L("Seed ID: %s"), s.id.empty() ? "-" : s.id.c_str());
        if (s.race) ImGui::TextDisabled(L("Race mode. Seed hash: %s"), hash_names(s.id).c_str());
        if (active && s.name == active) ImGui::TextDisabled("%s", L("Playing"));
        ImGui::Spacing();
    }
}

// ---- in game, in a seed (the menu bar's Randomizer): this seed's sheet first - the player's own rows, its spoiler log
// and ID - and Randomizer Main Menu for the whole panel (a new seed, another seed). Outside a seed the panel at once.
// (user 2026-11-09)
static bool g_full;       // the whole panel is open over the sheet (B on its left column: back to the sheet)
static void seed_sheet(float scale, const char *active) {
    const Seed *sp = NULL;
    for (const Seed &x : g_seeds) if (x.name == active) sp = &x;
    GenOpts so = g_opts;  // (the seed's own options, for its room shuffle; a seed from before IDs: the rows all open)
    bool rooms = !sp || sp->id.empty() || !seed_id_decode(sp->id.c_str(), so).empty() || so.room_shuffle;
    ImGui::TextColored(plastic_col(plastic_theme().title), "%s", active);
    ImGui::TextDisabled(L("Seed ID: %s"), sp && !sp->id.empty() ? sp->id.c_str() : "-");
    if (sp && sp->race) ImGui::TextDisabled(L("Race mode. Seed hash: %s"), hash_names(sp->id).c_str());
    ImGui::Spacing();
    opt_begin("##rando_sheet", scale);
    row_tracker();
    row_door_letters(rooms);
    bool locked = sp && sp->race && !sp->race_done;  // (race mode: shut until the credits)
    if (locked) opt_disable_next();
    opt_button_next();
    if (opt_action("Spoiler log", locked ? "Locked" : "", NULL)) g_spoiler_seed = active;
    if (!sp || sp->id.empty()) opt_disable_next();
    if (opt_action("Copy seed ID", "", NULL) && sp) ImGui::SetClipboardText(sp->id.c_str());
    if (opt_action("Randomizer Main Menu", "", "Make a new seed or switch to another one.")) g_full = true;
    // Switch to vanilla on its own at the bottom of the rows (user 2026-11-10); phones' cards: a gap above it
    float row_h = ImGui::GetFontSize() * 1.9f, bottom = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - row_h;
    if (!PLAT_TOUCH && bottom > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(bottom);
    else ImGui::Dummy(ImVec2(0, row_h));
    if (opt_action("Switch to vanilla", "", "Leave this seed and play the normal game. Your seed's saves stay put."))
        play_seed("");
    opt_end();
}

extern "C" void rando_tab_draw(float scale) {
    static GameLayout scanned_version = GAME_UNSUPPORTED;
    if (scanned_version != game_version_current()) {
        g_scanned = false; g_spoiler_seed.clear(); scanned_version = game_version_current();
    }
    if (game_version_current() == GAME_UNSUPPORTED) {
        ImGui::TextWrapped("%s", ui_tr("Set up a supported game version before generating a seed.")); return;
    }
    if (game_version_current() == GAME_110)
        ImGui::TextWrapped("%s", ui_tr("v1.1.0 randomizer support is experimental. Generate a new seed for this game version."));

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
    if (frame - g_last_frame > 1) {  // (back on the tab)
        g_full = false;
        g_focus = FOCUS_NONE, g_in_right = false, g_confirm_delete = false;
        g_preset_confirm_delete = false, g_replace_preset.clear(), g_preset_msg.clear();
    }
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
    static const int cat_cycle = getenv("LARECOMP_RANDO_CAT_CYCLE") ? atoi(getenv("LARECOMP_RANDO_CAT_CYCLE")) : 0;
    static int cat_frames;  // (test) <frames>: each category in turn, that many frames each (screenshot series)
    if (cat_cycle > 0 && cat_frames++ % cat_cycle == 0) g_cat = cat_frames / cat_cycle % N_CAT;

    bool busy = g_gen_pid || g_gen_done;  // the generation overlay is up: the whole panel is greyed out under it
    // (the start screen: no game loaded, nothing playing - and the seed's patches mustn't be applied yet)
    const char *active = !g_start_screen && rando_seed_dir() ? rando_seed_dir() + 6 : NULL;
    if (active && !g_full && !busy) {
        seed_sheet(scale, active);
        if (g_full) g_focus = FOCUS_CAT;  // (the panel: the cursor on its category)
        return;
    }
    // B / Esc on the panel's left column, opened from the sheet: back to the sheet
    if (active && g_full && !g_in_right && !busy && !ui_text_busy() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        opt_pressed_back()) {
        g_back_frame = frame, g_full = false;
        return;
    }
    // B / Esc in the right column: a step back (delete confirm -> the seed's actions -> the seed list -> the category);
    // Left too, on a row that doesn't step with left / right
    if (g_in_right && !busy && !ui_text_busy() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
        (opt_pressed_back() || (opt_pressed_left() && !opt_focused_lr()))) {
        g_back_frame = frame;
        if (g_cat == CAT_PRESETS && g_preset_confirm_delete) g_preset_confirm_delete = false, g_focus = FOCUS_DELETE;
        else if (g_cat == CAT_PRESETS && !g_replace_preset.empty()) g_replace_preset.clear(), g_focus = FOCUS_SAVE_AS;
        else if (g_cat == CAT_PRESETS && !g_act_preset.empty())
            g_focus_preset = g_act_preset, g_act_preset.clear(), g_preset_msg.clear(), g_focus = FOCUS_PRESET;
        else if (g_confirm_delete) g_confirm_delete = false, g_focus = FOCUS_DELETE;
        else if (!g_act_seed.empty()) g_focus_seed = g_act_seed, g_act_seed.clear(), g_focus = FOCUS_SEED;
        else g_focus = FOCUS_CAT;
    }

    ImGui::BeginDisabled(busy);

    opt_begin("##rando", scale, true);
    opt_compact();
    std::string cur_preset = current_preset();
    for (const Builtin &builtin : builtins()) if (cur_preset == builtin.name) { cur_preset = L(builtin.name); break; }  // (Seeds and Presets share a line: half rows, a long name cut short)
    {  // (12 characters at most, cut between UTF-8 characters)
        size_t chars = 0, cut = 0;
        for (size_t i = 0; i < cur_preset.size(); i++)
            if (((unsigned char)cur_preset[i] & 0xC0) != 0x80 && ++chars == 11) cut = i;
        if (chars > 12) cur_preset = cur_preset.substr(0, cut) + "...";
    }
    for (int c = 0; c < N_CAT; c++) {
        if (c == CAT_SEEDS) opt_pair_next();
        if (g_in_right && c == g_cat) opt_mark_next();
        char n[16];
        snprintf(n, sizeof n, "%d", (int)g_seeds.size());
        bool open = opt_link(k_cat_name[c], NULL, c == CAT_SEEDS ? n : c == CAT_PRESETS ? cur_preset.c_str() : NULL);
        bool clicked = ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left);  // (the row fires on the release)
        if (c == g_cat) take_focus(FOCUS_CAT);
        bool pick = (opt_item_nav_focused() && g_cat != c) || open;  // (moving down the list shows each)
        if (pick && g_cat != c)
            g_act_seed.clear(), g_confirm_delete = false, g_act_preset.clear(), g_preset_confirm_delete = false,
            g_replace_preset.clear(), g_preset_msg.clear();
        if (pick) g_cat = c;
        if (open && !clicked) g_focus = FOCUS_RIGHT;  // (A / Right: the cursor goes to its rows)
        if (open && c == CAT_SEEDS) g_act_seed.clear(), g_confirm_delete = false;
        if (open && c == CAT_PRESETS) g_act_preset.clear(), g_preset_confirm_delete = false, g_replace_preset.clear();
    }

    // ---- a new seed
    opt_section("New seed");
    opt_text("Seed name", g_opts.seed, sizeof g_opts.seed, "Random",
             "Name your seed, or leave it empty for a random name.");
    {  // Seed ID: the ID of these options + this seed's random part; type / paste one to load a shared seed
        static char idbuf[48];
        static std::string id_msg;
        if (!g_opts.rng) g_opts.rng = new_rng();
        if (!opt_text_editing("Seed ID")) seed_id_encode(g_opts, idbuf, sizeof idbuf);
        std::string d = L("Enter a shared seed ID to load its options. The same ID on the same Multi version always "
                          "makes the same seed.");
        if (!id_msg.empty()) d += std::string("\n\n") + L(id_msg.c_str());
        if (opt_text("Seed ID", idbuf, sizeof idbuf, "", d.c_str())) {
            GenOpts t = g_opts;
            id_msg = seed_id_decode(idbuf, t);
            if (id_msg.empty()) {
                char keep[64];
                strlcpy(keep, g_opts.seed, sizeof keep);
                g_opts = t;
                strlcpy(g_opts.seed, keep, sizeof g_opts.seed);
                tri_mode_from_opts();
                id_msg = LN("Loaded. Your options now match this ID.");
            }
        }
        opt_pair_next();
        if (opt_action("Copy new seed's ID", "", "Copies the ID above so you can share the seed."))
            ImGui::SetClipboardText(idbuf);
        if (opt_action("New seed ID", "", "Rerolls the seed and keeps your options.")) { g_opts.rng = new_rng(); id_msg.clear(); }
    }
    if (opt_action("Generate seed", "", "Makes a seed with these options. It shows up under Seeds, ready to play."))
        start_generation();
    bool summary = ImGui::IsItemHovered() || opt_item_nav_focused();  // (this row only: the right column sums up the options)
    if (!g_gen_status.empty() && !busy) {  // (e.g. a rename that failed)
        opt_disable_next();
        opt_shown_next();  // (put together in the menus' language)
        opt_action(g_gen_status.c_str(), "", g_gen_status.c_str(), false);
    }

    // ---- the right column: the chosen category's rows, the description under them
    opt_split();
    if (summary) seed_summary(scale);
    else {
        if (g_cat == CAT_SEEDS && !g_act_seed.empty()) seed_header(active);
        else if (g_cat == CAT_PRESETS && !g_act_preset.empty()) {
            ImGui::TextColored(plastic_col(plastic_theme().title), "%s", g_act_preset.c_str());
            ImGui::Spacing();
        } else opt_section(k_cat_name[g_cat]);
        opt_list_begin();
        if (g_focus == FOCUS_RIGHT) g_focus = FOCUS_NONE, opt_focus_next_row();
        if (g_cat == CAT_SEEDS) rows_seeds(active);
        else if (g_cat == CAT_PRESETS) rows_presets();
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
