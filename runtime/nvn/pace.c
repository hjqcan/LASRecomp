// Frame pacing, shared by the Metal (metal.m) and Vulkan (vulkan/device.c) presents: each presented frame calls
// pace_frame() on the presenting thread, and the backends pass the game's timing counters through pace_end_counter().
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../game_addresses.h"
#include "../rt.h"
#include "pace.h"

// The game advances its world one fixed step per frame in its 60 fps mode, and two steps per frame in its 30 fps mode
// (measured: Link walks twice as far per frame there) - so a 60-mode frame that takes 20 ms plays in slow motion, while
// the 30 mode plays at full speed. The game picks the mode itself every frame (its frame controller f_700000c910, from
// 0x700000ca44): the extra vsyncs it waits next become those its last frame missed, one fewer when that frame's CPU time
// and GPU time (its timing counters) were both under its limits (13.33 / 14.17 ms), kept between a least and a most
// (0 and 1, on the game manager). One missed vsync takes it to 30, and one quick frame - or one that came a vsync early
// after a late one - back to 60: no hysteresis. Our frames can be slow for reasons its GPU time doesn't show (command
// translation, a slower CPU), and a machine near the edge flipped 60 / 30 every few frames (a Steam Deck at 1x: 35-45
// fps, issue #40), also while the counters below reported slow frames.
// So each presented frame's busy time is measured (present to present, minus the time the presenting thread was
// blocked in kernel waits, sleeps and the window's image: idle); when frames don't fit 60 (12 of the last 60 over
// 16.2 ms), the game is held in its 30 fps mode: its least extra vsyncs becomes 1, and the counters report a frame of at
// least PACE_SLOW_NS as before (its own dynamic resolution, which read them too, is off: tools/patches.py
// GAME_DYNRES_SITE). It can't flip back on a quick frame then; we let it go back on headroom:
//   clear: frames that would cost under 11 ms at 60 on average and none of the last 60 over 14.5 ms, 60 frames in a
//     row (2 s at 30) - right away, whatever the backoff (a light scene after a heavy one doesn't wait);
//   enough: under 13.5 ms on average and at most 1 of the last 60 over 16.2 ms, 150 frames in a row (5 s at 30) times
//     the backoff.
// (A 30 fps frame's cost at 60: its busy time less its second world step, see `cost` below.)
// Backoff (issues #70, #40): a machine right at the edge (a handheld on a low power profile) gains headroom at 30 fps
// just by cooling down, goes back to 60, and doesn't fit again a few seconds later - 60 / 30 flicker. Falling back
// within 20 s of going back doubles the wait for "enough" (up to 16x: 80 s); a minute at 60 resets it.
// LARECOMP_PACE=0: off (the game's own choice, flips and all).
#define PACE_SLOW_NS 20000000ull
extern __thread uint64_t g_blocked_ns;  // kernel.c: this thread's time blocked in waits / sleeps
static bool pace_slow;
static uint64_t stat_pace_over;

// Holds the game in its 30 fps mode (see above): the least number of extra vsyncs its frame controller may choose
// (game manager +0x4e2, normally 0) is 1 while slow, the game's own value again after. Re-asserted every frame while held.
#define FW_MIN_EXTRA_VSYNCS 0x4e2  // (+0x4e3: the most, 1 - its 30 fps mode; +0x4e0 / +0x4e1: the current choice)
static void hold_30(bool on) {
    static bool held;
    static uint8_t saved;
    uint64_t fw = *(volatile uint64_t *)(uintptr_t)GA_GAME_MGR;
    if (!fw) { held = false; return; }  // (between boots / before the game set up)
    volatile uint8_t *min = (volatile uint8_t *)(uintptr_t)(fw + FW_MIN_EXTRA_VSYNCS), *max = min + 1;
    if (on && *max >= 1) {
        if (!held) saved = *min, held = true;
        *min = 1;
    } else if (held) {
        *min = saved, held = false;
    }
}

bool pace_is_slow(void) { return title_is_las() && pace_slow; }
uint64_t pace_frames_over(void) {
    uint64_t n = stat_pace_over;
    stat_pace_over = 0;
    return n;
}

// LARECOMP_SPIKE_MS=<ms> [LARECOMP_SPIKE_EVERY=<frames>, default 60]: the presenting (game) thread sleeps that long
// every that many frames from frame 2700 on (in play after a scripted load): a CPU-side hitch with the GPU idle
// (dynamic resolution tests).
static void spike_test(void) {
    static long ms = -1, every, frame;
    if (ms < 0) {
        ms = getenv("LARECOMP_SPIKE_MS") ? atol(getenv("LARECOMP_SPIKE_MS")) : 0;
        every = getenv("LARECOMP_SPIKE_EVERY") ? atol(getenv("LARECOMP_SPIKE_EVERY")) : 60;
        if (every < 1) every = 60;
    }
    if (ms > 0 && ++frame > 2700 && frame % every == 0) plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000);
}

void pace_frame(void) {
    spike_test();
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_PACE"); on = !e || atoi(e) != 0; }
    static uint64_t last, last_wait;
    static uint8_t over[60];
    static float recent[60];  // the last 60 frames' cost (ms at 60 fps)
    static int idx, nover, calm, clear, backoff = 1;
    static uint64_t back_at;  // when frame pacing last went back to 60 (0: not yet)
    static double avg;  // cost (ms at 60 fps), moving average (~20 frames)
    static uint64_t last_tcpu;
    // LAS interprets these inflated counters as a fixed-step frame-mode request.
    // Other adapters must retain their actual timestamps and presentation intervals.
    if (!title_is_las()) {
        pace_slow = false; stat_pace_over = 0;
        last = last_wait = back_at = last_tcpu = 0;
        idx = nover = calm = clear = 0; backoff = 1; avg = 0;
        memset(over, 0, sizeof over);
        memset(recent, 0, sizeof recent);
        return;
    }
    uint64_t now = plat_now_ns(), w = g_blocked_ns, tcpu = plat_thread_cpu_ns();
    uint64_t p = now - last, wt = w - last_wait, tc = tcpu - last_tcpu;
    bool first = !last;
    last = now, last_wait = w, last_tcpu = tcpu;
    if (first || p > 100000000ull) return;  // (loading, a hitch: not a pace)
    double busy = (p > wt ? p - wt : 0) / 1e6;
    // What the frame would cost at 60 fps: a frame of the game's 30 fps mode (two vsyncs or more) runs its world twice,
    // and the second step - about half this thread's CPU time then (measured: 3.0 -> 6.0 ms, 4.9 -> 9.3 ms) - is work
    // a 60 fps frame doesn't have. Without that, the 30 fps mode's frames looked 3-5 ms slower than they would be at 60
    // and the way back needed far more headroom than it should.
    double step2 = tc / 2e6 < busy / 2 ? tc / 2e6 : busy / 2;  // (at most half: a coarse thread clock can't take it all)
    double cost = p > 25000000ull ? busy - step2 : busy;
    avg = avg ? avg + (cost - avg) * 0.05 : cost;
    static int log = -1;  // LARECOMP_LOG_PACE=1: each frame's period, vsync wait and busy time
    if (log < 0) log = getenv("LARECOMP_LOG_PACE") != NULL;
    extern __thread uint64_t g_rq_wait_ns;  // rqueue.c: waits for the render thread (translation), counted as busy
    static uint64_t last_rq;
    uint64_t rqw = g_rq_wait_ns - last_rq;
    last_rq = g_rq_wait_ns;
    if (log) {  // (plus this thread's CPU time per frame and the render thread's: work, independent of waits; and where
                // this thread's frame went in the renderer's frontend - rqueue.h)
        extern uint64_t rq_render_cpu_ns(void);
        extern __thread uint64_t g_rq_lock_ns, g_submit_ns, g_gpu_wait_ns, g_present_ns, g_hooks_ns;
        static uint64_t last_cpu, last_rcpu, l_lock, l_sub, l_gpu, l_pres, l_hooks;
        uint64_t cpu = plat_thread_cpu_ns(), rcpu = rq_render_cpu_ns();
        LOG("[pace] period %.2f blocked %.2f busy %.2f cost %.2f ms (queue wait %.2f, cpu %.2f, render cpu %.2f, gpu wait %.2f, submit %.2f, "
            "present %.2f, hooks %.2f, lock %.2f)\n", p / 1e6, wt / 1e6, busy, cost, rqw / 1e6, last_cpu ? (cpu - last_cpu) / 1e6 : 0.0,
            last_rcpu ? (rcpu - last_rcpu) / 1e6 : 0.0, (g_gpu_wait_ns - l_gpu) / 1e6, (g_submit_ns - l_sub) / 1e6,
            (g_present_ns - l_pres) / 1e6, (g_hooks_ns - l_hooks) / 1e6, (g_rq_lock_ns - l_lock) / 1e6);
        last_cpu = cpu;
        last_rcpu = rcpu;
        l_lock = g_rq_lock_ns, l_sub = g_submit_ns, l_gpu = g_gpu_wait_ns, l_pres = g_present_ns, l_hooks = g_hooks_ns;
    }
    int o = cost > 16.2;
    stat_pace_over += o;
    nover += o - over[idx];
    over[idx] = (uint8_t)o;
    recent[idx] = (float)cost;
    idx = (idx + 1) % 60;
    if (!on) return;  // (measured and counted all the same: LARECOMP_LOG_PACE, the stats)
    if (!pace_slow && back_at && backoff > 1 && now - back_at > 60000000000ull) {
        backoff = 1;
        LOG("[pace] a minute at 60: headroom needed back to normal\n");
    }
    if (!pace_slow && nover >= 12) {
        pace_slow = true, calm = clear = 0;
        if (back_at && now - back_at < 20000000000ull && backoff < 16) backoff *= 2;  // (flickering: wait longer)
        LOG("[pace] frames don't fit 60 fps (%d of the last 60 over 16.2 ms busy): holding the game's 30 fps mode%s\n", nover,
            backoff > 1 ? " (backing off)" : "");
    } else if (pace_slow) {
        float peak = 0;
        for (int i = 0; i < 60; i++) peak = recent[i] > peak ? recent[i] : peak;
        clear = avg < 11.0 && peak < 14.5f ? clear + 1 : 0;
        calm = avg < 13.5 && nover <= 1 ? calm + 1 : 0;
        if (clear >= 60 || calm >= 150 * backoff) {
            LOG("[pace] %d frames with %s headroom (%.1f ms a frame at 60 on average, at most %.1f): back to the game's own choice\n",
                clear >= 60 ? clear : calm, clear >= 60 ? "clear" : "enough", avg, peak);
            pace_slow = false, nover = 0, back_at = now;
            memset(over, 0, sizeof over);
            memset(recent, 0, sizeof recent);
        }
    }
    hold_30(pace_slow);
}

uint64_t pace_end_counter(const uint64_t *start_entry, const uint64_t *entry, uint64_t value) {
    if (!title_is_las() || !pace_slow || !start_entry || (const uint8_t *)entry - (const uint8_t *)start_entry != 16) return value;
    uint64_t min = start_entry[1] + (uint64_t)((unsigned __int128)PACE_SLOW_NS * 384 / 625);
    return value < min ? min : value;
}
