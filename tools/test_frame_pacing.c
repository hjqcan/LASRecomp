#include <assert.h>
#include <stdio.h>
#include "../runtime/nvn/pace.h"
#include "../runtime/game_addresses.h"
static uint8_t game_manager[0x500];
static uintptr_t game_manager_ptr = (uintptr_t)game_manager;
static const GameAddrs test_addresses={.GAME_MGR=(uintptr_t)&game_manager_ptr};
const GameAddrs *g_ga=&test_addresses;
volatile uint64_t g_vsync_count,g_vsync_ns;
void slow_frame_present(uint64_t now,uint64_t cpu) { (void)now; (void)cpu; }
int64_t pace_gpu_ns(void) { return -1; }
int game_in_play(void) { return 1; }
bool world_on_screen(void) { return true; }
const char *game_level_name(void) { return "Field"; }
uint64_t game_camera_zone(void) { return 0; }
#include "../runtime/nvn/pace.c"

static bool las = true;
static uint64_t now = 1000000000;
__thread uint64_t g_blocked_ns;
__thread uint64_t g_rq_wait_ns, g_rq_lock_ns, g_submit_ns, g_gpu_wait_ns, g_present_ns, g_hooks_ns;
bool title_is_las(void) { return las; }
uint64_t plat_now_ns(void) { return now; }
uint64_t plat_thread_cpu_ns(void) { return now - g_blocked_ns; }
uint64_t rq_render_cpu_ns(void) { return 0; }
void plat_sleep_until_ns(uint64_t deadline) { assert(deadline >= now); now = deadline; }
static void frame(uint64_t busy, uint64_t wait) {
    now += busy + wait;
    g_blocked_ns += wait;
    pace_frame();
}
static unsigned recover(void) {
    unsigned n = 0;
    while (pace_is_slow() && n < 2000) { frame(12000000, 0); ++n; }
    assert(!pace_is_slow() && n < 2000);
    return n;
}
int main(int argc, char **argv) {
    (void)argv;
    uint64_t counters[6] = {0, 100, 0, 150, 0, 200};
    game_manager[0x4e3] = 1;
    pace_frame();
    for (unsigned i = 0; i < 60; ++i) frame(10000000, 6666667);
    assert(!pace_is_slow() && pace_frames_over() == 0);
    for (unsigned i = 0; i < 90; ++i) frame(20000000, 0);
    assert(pace_frames_over() > 0 && pace_frames_over() == 0);
    if (argc > 1) {
        assert(!pace_is_slow());
        assert(pace_end_counter(counters, counters + 2, 150) == 150);
        puts("PASS: pacing disabled preserves GPU counters");
        return 0;
    }
    assert(pace_is_slow());
    assert(pace_end_counter(counters, counters + 2, 150) == 100 + 12288000);
    assert(pace_end_counter(counters, counters + 4, 200) == 200);
    assert(pace_end_counter(NULL, counters + 2, 150) == 150);
    assert(game_manager[0x4e2] == 1);
    unsigned first = recover();
    assert(game_manager[0x4e2] == 0);
    for (unsigned i = 0; i < 90; ++i) frame(20000000, 0);
    assert(pace_is_slow());
    unsigned second = recover();
    assert(second >= first); // A rapid fallback retains the upstream backoff.
    for (unsigned i = 0; i < 90; ++i) frame(20000000, 0);
    assert(pace_is_slow());
    las = false;
    // The title may switch before its first present: never inflate another game's counter.
    assert(!pace_is_slow() && pace_end_counter(counters, counters + 2, 150) == 150);
    for (unsigned i = 0; i < 120; ++i) frame(30000000, 0);
    assert(!pace_is_slow() && pace_frames_over() == 0);
    las = true;
    frame(10000000, 0);
    assert(!pace_is_slow());
    for (unsigned i = 0; i < 90; ++i) frame(20000000, 0);
    assert(pace_is_slow());
    unsigned fresh = recover();
    assert(fresh < second);
    puts("PASS: LAS pacing, idle exclusion, counter pairs, fallback backoff and generic-title isolation");
}
