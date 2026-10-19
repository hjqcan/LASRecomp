#include <assert.h>
#include <stdio.h>
#include "../runtime/nvn/pace.h"

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
    while (pace_is_slow() && n < 2000) { frame(10000000, 0); ++n; }
    assert(!pace_is_slow() && n < 2000);
    return n;
}
int main(int argc, char **argv) {
    (void)argv;
    uint64_t counters[6] = {0, 100, 0, 150, 0, 200};
    pace_frame();
    for (unsigned i = 0; i < 60; ++i) frame(10000000, 20000000);
    assert(!pace_is_slow() && pace_frames_over() == 0);
    for (unsigned i = 0; i < 13; ++i) frame(20000000, 0);
    assert(pace_frames_over() == 13 && pace_frames_over() == 0);
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
    unsigned first = recover();
    for (unsigned i = 0; i < 13; ++i) frame(20000000, 0);
    assert(pace_is_slow());
    unsigned second = recover();
    assert(second >= first + 110); // A rapid fallback needs another 120 calm frames.
    for (unsigned i = 0; i < 13; ++i) frame(20000000, 0);
    assert(pace_is_slow());
    las = false;
    // The title may switch before its first present: never inflate another game's counter.
    assert(!pace_is_slow() && pace_end_counter(counters, counters + 2, 150) == 150);
    for (unsigned i = 0; i < 120; ++i) frame(30000000, 0);
    assert(!pace_is_slow() && pace_frames_over() == 0);
    las = true;
    frame(10000000, 0);
    assert(!pace_is_slow());
    for (unsigned i = 0; i < 13; ++i) frame(20000000, 0);
    assert(pace_is_slow());
    unsigned fresh = recover();
    assert(fresh < second);
    puts("PASS: LAS pacing, idle exclusion, counter pairs, fallback backoff and generic-title isolation");
}
