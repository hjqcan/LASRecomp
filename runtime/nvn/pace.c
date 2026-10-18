// Frame pacing, shared by the Metal (metal.m) and Vulkan (vulkan/device.c) presents: each presented frame calls
// pace_frame() on the presenting thread, and the backends pass the game's timing counters through pace_end_counter().
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../rt.h"
#include "pace.h"

// The game advances its world one fixed step per frame in its 60 fps mode, and two steps per frame in its 30 fps mode
// (measured: Link walks twice as far per frame there) - so a 60-mode frame that takes 20 ms plays in slow motion, while
// the 30 mode plays at full speed. The game reads its timing counters (GPU time per frame) as on the Switch: they set
// its dynamic resolution (main pass 1920x1080 down to 1280x720 from ~12 ms, the floor from ~15 ms) and, with frames
// really missing 60, keep it in the 30 fps mode (only from ~15 ms: below that it keeps trying 60 - slow motion half the
// time). But our frames can be slow for reasons GPU time doesn't show (command translation, a slower CPU). So each
// presented frame's busy time is measured (present to present, minus the time the presenting thread was blocked in
// kernel waits, sleeps and the window's image: idle); when frames don't fit 60 (12 of the last 60 over 16.2 ms), the
// counters report a frame of at least PACE_SLOW_NS: the game takes its 30 fps mode, at full speed (and its lower
// resolution with it - the same reading). Back to the real times after 120 frames with headroom (busy under 13 ms on average,
// at most 2 of the last 60 frames over). The game itself also tries 60 again now and then.
// Backoff (issue #70): a machine right at the edge (a handheld on a low power profile) gains headroom at 30 fps just by
// cooling down, goes back to 60, and doesn't fit again a few seconds later - 60 / 30 flicker. Falling back within 10 s
// of going back doubles the headroom needed next time (up to 32x); a minute at 60 resets it. LARECOMP_PACE=0: off.
#define PACE_SLOW_NS 20000000ull
extern __thread uint64_t g_blocked_ns;  // kernel.c: this thread's time blocked in waits / sleeps
static bool pace_slow;
static uint64_t stat_pace_over;

bool pace_is_slow(void) { return pace_slow; }
uint64_t pace_frames_over(void) {
    uint64_t n = stat_pace_over;
    stat_pace_over = 0;
    return n;
}

void pace_frame(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_PACE"); on = !e || atoi(e) != 0; }
    static uint64_t last, last_wait;
    static uint8_t over[60];
    static int idx, nover, calm, backoff = 1;
    static uint64_t back_at;  // when frame pacing last went back to 60 (0: not yet)
    static double avg;  // busy ms, moving average (~20 frames)
    uint64_t now = plat_now_ns(), w = g_blocked_ns;
    uint64_t p = now - last, wt = w - last_wait;
    bool first = !last;
    last = now, last_wait = w;
    if (first || p > 100000000ull) return;  // (loading, a hitch: not a pace)
    double busy = (p > wt ? p - wt : 0) / 1e6;
    avg = avg ? avg + (busy - avg) * 0.05 : busy;
    static int log = -1;  // LARECOMP_LOG_PACE=1: each frame's period, vsync wait and busy time
    if (log < 0) log = getenv("LARECOMP_LOG_PACE") != NULL;
    if (log) LOG("[pace] period %.2f blocked %.2f busy %.2f ms\n", p / 1e6, wt / 1e6, busy);
    int o = busy > 16.2;
    stat_pace_over += o;
    nover += o - over[idx];
    over[idx] = (uint8_t)o;
    idx = (idx + 1) % 60;
    if (!on) return;  // (measured and counted all the same: LARECOMP_LOG_PACE, the stats)
    if (!pace_slow && back_at && backoff > 1 && now - back_at > 60000000000ull) {
        backoff = 1;
        LOG("[pace] a minute at 60: headroom needed back to normal\n");
    }
    if (!pace_slow && nover >= 12) {
        pace_slow = true, calm = 0;
        if (back_at && now - back_at < 10000000000ull && backoff < 32) backoff *= 2;  // (flickering: wait longer)
        LOG("[pace] frames don't fit 60 fps (%d of the last 60 over 16.2 ms busy): the game's 30 fps mode%s\n", nover,
            backoff > 1 ? " (backing off)" : "");
    } else if (pace_slow) {
        // headroom: busy well under a 60 fps frame on average, hardly a frame over (single spikes are normal play)
        calm = avg < 13.0 && nover <= 2 ? calm + 1 : 0;
        if (calm >= 120 * backoff) {
            pace_slow = false, nover = 0, back_at = now;
            memset(over, 0, sizeof over);
            LOG("[pace] %d frames with headroom: back to the game's own choice\n", calm);
        }
    }
}

uint64_t pace_end_counter(const uint64_t *start_entry, const uint64_t *entry, uint64_t value) {
    if (!pace_slow || !start_entry || (const uint8_t *)entry - (const uint8_t *)start_entry != 16) return value;
    uint64_t min = start_entry[1] + (uint64_t)((unsigned __int128)PACE_SLOW_NS * 384 / 625);
    return value < min ? min : value;
}
