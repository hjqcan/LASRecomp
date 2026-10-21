// Run timer (Gameplay tab "Timer", drawn by runtime/ui/race_overlay.mm) and LiveSplit auto-splitting (runtime/livesplit.c).
//
// One run per attempt: it starts from 0:00 every time gameplay starts from the title's file select (a new file or a
// loaded one) and lasts until the next such start, or a quit to the start screen / desktop (a reboot: RT_ON_RESET).
// The title screen plays its movie (title.webm, runtime/hle/movie.c): Link coming into play after that is a new
// attempt. A Load from the pause menu's System page, Retry after a game over: the same attempt (no title).
// Nothing is saved. The clock always runs (it costs
// nothing); Timer shows it - set before the game starts, the row is greyed out in game - and LiveSplit gets it.
// Real time: it never stops - level loads and transitions, the map, the Items page, the game's pause menu and ours all
// count (user 2026-10-20).
//   end      the Wind Fish ending: the "Ending" level or the ending movie (runtime/hle/movie.c); the time stays, gold
//   splits   each instrument obtained (live save +0x556C bits 7-14), then the ending
#include "../game_addresses.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../livesplit.h"
#include "../rando.h"
#include "../settings.h"

extern int game_in_play(void);
extern const char *game_level_name(void);
extern uint64_t renderer_present_count(void);

#define KEYITEMS (GA_LIVE_SAVE + 0x556C)  // key items: bits 7-14 the instruments
#define INSTRUMENTS(v) ((v) >> 7 & 0xFF)

enum { RUN_NONE = 0, RUN_GOING = 1, RUN_DONE = 2 };

static volatile uint64_t g_us;     // game time of the run, microseconds
static volatile int g_state;       // RUN_*
static volatile int g_counting;    // the clock is running right now
static volatile int g_movie_end;   // the ending movie opened
static uint64_t g_last_ns, g_sync_ns;
static uint32_t g_inst;            // instruments seen this run
static volatile int g_title;       // the title screen came up: the next start of play is a new attempt
static int g_was_in_play;

RT_ON_RESET(run_timer_reset) {  // (quit to the start screen / another seed: the session is over)
    g_us = 0, g_state = RUN_NONE, g_counting = 0, g_movie_end = 0, g_last_ns = 0, g_title = 0, g_was_in_play = 0;
}

static void fmt_time(uint64_t us, char *out, size_t n) {
    uint64_t ms = us / 1000;
    snprintf(out, n, "%llu:%02llu:%02llu.%03llu", (unsigned long long)(ms / 3600000), (unsigned long long)(ms / 60000 % 60),
             (unsigned long long)(ms / 1000 % 60), (unsigned long long)(ms % 1000));
}

static void ls_gametime(void) {
    char t[48], line[64];
    fmt_time(g_us, t, sizeof t);
    snprintf(line, sizeof line, "setgametime %s", t);
    livesplit_cmd(line);
}

// LiveSplit (re)connected: bring it to the run's state (network thread; reads only)
void run_timer_livesplit_sync(void (*send)(const char *line, void *ctx), void *ctx) {
    if (g_state != RUN_GOING) return;
    char t[48], line[64];
    fmt_time(g_us, t, sizeof t);
    snprintf(line, sizeof line, "setgametime %s", t);
    send("starttimer", ctx);
    send("initgametime", ctx);
    send(line, ctx);
    send("unpausegametime", ctx);
}

static uint32_t instruments_now(void) { return INSTRUMENTS(LD32(KEYITEMS)); }

static void start_run(void) {
    g_us = 0, g_state = RUN_GOING, g_counting = 0;
    g_inst = instruments_now();
    LOG("[timer] run started (gameplay began)\n");
    livesplit_cmd("reset");  // (a new session is a new run)
    livesplit_cmd("starttimer");
    livesplit_cmd("initgametime");
    ls_gametime();
    livesplit_cmd("unpausegametime");  // (LiveSplit's game time runs with ours: real time, kept in step below)
}

static void finish_run(const char *why) {
    g_state = RUN_DONE, g_counting = 0;
    char t[48];
    fmt_time(g_us, t, sizeof t);
    LOG("[timer] run finished (%s): %s\n", why, t);
    ls_gametime();
    livesplit_cmd("split");
    livesplit_cmd("pausegametime");
    // the credits: a race seed's spoiler log opens from now on (runtime/ui/rando_tab.mm)
    const char *dir = rando_seed_dir();
    if (dir) {
        char p[1100];
        snprintf(p, sizeof p, "%s/race_done", dir);
        FILE *f = fopen(p, "w");
        if (f) { fprintf(f, "%s\n", t); fclose(f); }
    }
}

// runtime/hle/movie.c: the title screen's movie opened (the title / file select is up)
void run_timer_title(void) { g_title = 1; }

// runtime/hle/movie.c: the ending movie
void run_timer_ending_movie(void) { g_movie_end = 1; }

// race_overlay.mm: the run (state RUN_*, time in microseconds, counting right now)
int run_timer_state(uint64_t *us, bool *counting) {
    if (us) *us = g_us;
    if (counting) *counting = g_counting;
    return g_state;
}

// Game thread, once per frame (runtime/hle/input.c)
void run_timer_tick(void) {
    uint64_t now = plat_now_ns(), dt = g_last_ns && now > g_last_ns ? now - g_last_ns : 0;
    g_last_ns = now;
    if (g_settings.livesplit) livesplit_poke();  // (connects in the background, also before the run)
    bool in_play = game_in_play() != 0, entered = in_play && !g_was_in_play;
    g_was_in_play = in_play;
    if (g_state == RUN_NONE || (entered && g_title)) {  // (a new attempt: from 0:00)
        if (!in_play) return;
        g_title = 0;
        start_run();
    }
    if (g_state != RUN_GOING) return;
    // (test) LARECOMP_TIMER_TEST_END=<frame>: the ending at that presented frame
    bool test_end = ENV("LARECOMP_TIMER_TEST_END") && renderer_present_count() >= (uint64_t)atoll(ENV("LARECOMP_TIMER_TEST_END"));
    if (g_movie_end || (in_play && !strcmp(game_level_name(), "Ending")) || test_end) {
        finish_run(g_movie_end ? "ending movie" : test_end ? "test" : "Ending level");
        return;
    }
    // instruments: a split each
    if (in_play) {
        uint32_t inst = instruments_now();
        for (uint32_t b = inst & ~g_inst; b; b &= b - 1) {
            LOG("[timer] instrument %d at %llu us: split\n", __builtin_ctz(b) + 1, (unsigned long long)g_us);
            ls_gametime();
            livesplit_cmd("split");
        }
        g_inst = inst;
    }
    if (g_counting) g_us += (dt < 10000000000ull ? dt : 10000000000ull) / 1000;  // (at most 10 s a frame: the Mac asleep)
    g_counting = 1;
    if (now - g_sync_ns > 5000000000ull) {  // (keep LiveSplit's game time in step)
        ls_gametime();
        g_sync_ns = now;
    }
}
