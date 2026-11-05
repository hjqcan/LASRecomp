// Gameplay tab (was Difficulty): walk speed (runtime/hle/qol.c rt_walk_speed), damage taken multiplier / one-hit KO
// (applied where the game computes Link's damage, see tools/patches.py DAMAGE_*_SITES); rt_hero_mode() reads the game's own Hero Mode flag (PlayerStatus+0x120 = save
// 0x55E0). Settings persist in larecomp.ini (g_settings.diff_*).
#include "../game_addresses.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "i18n.h"
#include "option_rows.h"

extern "C" {
#include "../hle.h"
#include "../settings.h"
int game_in_play(void);
int rt_hookshot_seed_forced(void);  // hle/hookshot.c
int rt_no_buff_seed_forced(void);   // hle/qol.c
#include "../livesplit.h"
}


#define HERO_FLAG (GA(LIVE_SAVE) + 0x55E0)  // PlayerStatus (save+0x54C0) + 0x120

extern "C" int rt_hero_mode(void) { return *(volatile uint8_t *)(uintptr_t)HERO_FLAG == 1; }

// damage in quarter hearts before the Hero Mode shift; returns the damage to subtract
extern "C" int32_t rt_damage(int32_t dmg, int32_t shift) {
    static int envd = -1;  // LARECOMP_DAMAGE=<mult> / ohko: test override
    if (envd < 0) {
        const char *e = getenv("LARECOMP_DAMAGE");
        envd = e != NULL;
        if (e) {
            if (!strcmp(e, "ohko")) g_settings.diff_ohko = true;
            else g_settings.diff_damage = (float)atof(e);
        }
    }
    int32_t base = dmg << (shift & 31);
    if (base <= 0) return base;  // healing / zero: untouched
    if (g_settings.diff_ohko) return 999;
    float m = g_settings.diff_damage > 0 ? g_settings.diff_damage : 1.0f;
    int32_t r = (int32_t)lroundf((float)base * m);
    if (ENV("LARECOMP_LOG_DAMAGE")) LOG("[difficulty] damage %d (shift %d) -> %d\n", dmg, shift, r < 1 ? 1 : r);
    return r < 1 ? 1 : r;  // at least a quarter heart
}

// Once per presented frame (from save_editor_frame). (The "Hero Mode on every file" lever, g_settings.diff_hero, left
// the Gameplay tab - user, 2026-10-11 - and is no longer applied, so nobody is stuck with it; a file started in Hero
// Mode keeps the game's own flag.)
extern "C" void difficulty_frame(void) {}

// Plain levers, each at the game's own value by default (Default, off), as option rows (option_rows.h).
extern "C" void difficulty_draw(float scale) {
    opt_begin("##gameplay", scale);
    opt_section("General");
    {
        // what the game is told the console's language is: its text follows this (runtime/settings.c), and the menus
        // at once. Each language by its own name (the row leaves names it has no translation for as they are). The
        // menus' own languages (pt-BR) have no game text: the game stays English.
        const char *code = settings_menu_language(), *auto_name = "English";
        for (int i = 1; i < N_LANGUAGES; i++)
            if (!strcmp(code, g_language_codes[i])) auto_name = g_language_endonyms[i];
        char autol[64];
        snprintf(autol, sizeof autol, L("Auto (%s)"), auto_name);
        const char *names[N_LANGUAGES];
        names[0] = autol;
        for (int i = 1; i < N_LANGUAGES; i++) names[i] = g_language_endonyms[i];
        bool menus_only = strcmp(code, "en-US") && !strcmp(settings_language(), "en-US");
        if (opt_choice("Language", &g_settings.language, names, N_LANGUAGES, LANG_AUTO,
                       menus_only ? "The language of the menus. The game has no text in this language, so it stays English."
                                  : "The language of the menus and the game's text. The game's text changes after a restart."))
            settings_save();
    }
    if (rt_hookshot_seed_forced()) {  // (the seed's logic counts on it: always on, the toggle greyed out)
        bool on = true;
        opt_disable_next();
        opt_toggle("Hookshot anything", &on, false,
                   "This seed was made with Hookshot Anything, so it stays on.");
    } else if (opt_toggle("Hookshot anything", &g_settings.cheat_hookshot, false,
                          "The Hookshot latches onto any wall or object, not just Hookshot targets."))
        settings_save();
    if (rt_no_buff_seed_forced()) {
        bool on = true;
        opt_disable_next();
        opt_toggle("Disable Guardian Acorns / Pieces of Power", &on, false,
                   "This seed was made with it, so it stays on.");
    } else if (opt_toggle("Disable Guardian Acorns / Pieces of Power", &g_settings.no_buff_drops, false,
                          "Disables Guardian Acorns and Pieces of Power from spawning."))
        settings_save();
    if (opt_toggle("Mirror mode", &g_settings.mirror_mode, false,
                   "Mirrors the game world."))
        settings_save();
    {
        static const char *const names[] = {LN("Normal"), LN("Brisk"), LN("Running"), LN("Sprinting"), LN("Zoomies")};
        static const float mult[] = {1, 1.25f, 1.5f, 1.75f, 2.5f};
        int wi = 0;  // (the closest step to the setting)
        for (int i = 0; i < 5; i++)
            if (fabsf(mult[i] - g_settings.walk_speed) < fabsf(mult[wi] - g_settings.walk_speed)) wi = i;
        if (opt_choice("Walk speed", &wi, names, 5, 0, "How fast Link walks. Doesn't change the Pegasus Boots or swimming.")) {
            g_settings.walk_speed = mult[wi];
            settings_save();
        }
        if (wi == 0) opt_disable_next();
        if (opt_toggle("Indoors", &g_settings.walk_speed_dungeons, false,
                       "Use the walk speed in houses, caves and dungeons too."))
            settings_save();
    }
    opt_section("Difficulty");
    static const char *const names[] = {LN("Half"), LN("Default"), LN("2x"), LN("3x"), LN("4x"), LN("5x"),
                                         LN("6x"), LN("7x"), LN("8x")};
    static const float mult[] = {0.5f, 1, 2, 3, 4, 5, 6, 7, 8};
    int di = 1;  // (the closest step to the setting: older files may hold any multiplier)
    for (int i = 0; i < 9; i++)
        if (fabsf(mult[i] - g_settings.diff_damage) < fabsf(mult[di] - g_settings.diff_damage)) di = i;
    if (opt_choice("Damage taken", &di, names, 9, 1,
                   "How much damage each hit does, from half up to eight times normal.")) {
        g_settings.diff_damage = mult[di];
        settings_save();
    }
    if (opt_toggle("One-hit KO", &g_settings.diff_ohko, false, "One hit and Link is down, no matter how many hearts he has."))
        settings_save();
    if (opt_toggle("Hide Hero Mode icon", &g_settings.hide_hero_icon, false,
                   "Hides the Hero Mode emblem in the corner of the screen while playing a Hero Mode file."))
        settings_save();

    // run timer / LiveSplit (runtime/hle/run_timer.c, runtime/livesplit.c, runtime/ui/race_overlay.mm)
    opt_section("Speedrun");
    // (set before a save is loaded: free on the start screen, the title and the file select; locked while a file is
    // in play - game_in_play: Link exists in a level - and free again back at the title)
    bool locked = game_in_play();
    if (locked) opt_disable_next();
    if (opt_toggle("Timer", &g_settings.timer, false,
                   locked ? "Turn this on before you load a save."
                          : "Shows a run timer in the corner, from the start of gameplay to the ending."))
        settings_save();
    {  // (any time, also mid-session)
        static const char *const pos[] = {LN("Bottom left"), LN("Bottom right")};
        if (!g_settings.timer) opt_disable_next();
        if (opt_choice("Timer position", &g_settings.timer_pos, pos, 2, TIMER_POS_LEFT, "Which corner the timer sits in."))
            settings_save();
    }
    if (opt_toggle("LiveSplit", &g_settings.livesplit, false,
                   "Connects to LiveSplit's Server component. It starts the timer, splits on each instrument and stops "
                   "at the ending, with load times removed.")) {
        settings_save();
        livesplit_poke();
    }
    {
        char d[512];  // (in the menus' language already: the row leaves it as it is)
        snprintf(d, sizeof d, "%s%s%s", L("The address LiveSplit's Server listens on, written as host:port."),
                 g_settings.livesplit ? "\n\n" : "",
                 !g_settings.livesplit ? "" : livesplit_connected() ? L("Connected.") : L("Not connected."));
        if (!g_settings.livesplit) opt_disable_next();
        if (opt_text("Server", g_settings.livesplit_host, sizeof g_settings.livesplit_host, "localhost:16834", d)) {
            if (!g_settings.livesplit_host[0]) snprintf(g_settings.livesplit_host, sizeof g_settings.livesplit_host, "localhost:16834");
            settings_save();
        }
    }
    opt_end();
}
