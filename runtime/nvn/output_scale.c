// Output resolution, shared by the Metal and Vulkan backends (each calls output_scale() once per presented frame).
// Render targets the game sizes for the Switch's 1920x1080 screen are allocated at (sx, sy) times their size; the
// image is then drawn into (vx, vy, vw, vh) of the window. Video tab "Internal resolution" (g_settings.res_mode):
//   Match display  render at the window's own pixel size (default)
//   1x .. 4x       N times the Switch's docked image: 1080 lines x N (wide windows keep their shape: 32:9 at 2x =
//                  7680 x 2160); capped to the GPU's 16384-pixel texture limit. 1x on an integrated GPU: 720 lines,
//                  what the game's own scaler picked there (UHD 770); its scaler is off now (see rt_game_dynres)
//   .75x / .50x / .25x  of Match display (scaled up to the window - with MetalFX on macOS when chosen)
//   Dynamic        Match display, lowered in steps while the GPU can't hold 60 fps and raised again when it can.
//                  The render targets stay allocated at the Match display size (ax, ay) and the frame is drawn into
//                  their top-left part at (sx, sy): no reallocation at a step, so no hitch (each step reallocated
//                  every screen-sized texture before: ~325 ms on an M3 Max, seconds at 4K on Vulkan, issue #100).
// Test overrides: LARECOMP_SCALE=<f> (render height = 1080 x f), LARECOMP_WIDE_TEST=<f> (forced shape),
// LARECOMP_DYN_TEST=<f> (Dynamic pinned at factor f of the allocation: shots at a fixed part), LARECOMP_DYN_START=<f>
// (Dynamic starts at factor f).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "output_scale.h"

#define MAX_TEX 16384.0

// The game's own dynamic resolution (tools/patches.py GAME_DYNRES_SITE) is off: the resolution is the one chosen here,
// and only our Dynamic changes it. LARECOMP_GAME_DYNRES=1 (test): the game lowers its main pass under GPU load again.
int rt_game_dynres(void) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_GAME_DYNRES") && atoi(getenv("LARECOMP_GAME_DYNRES"));
    return on;
}

// Dynamic resolution: a factor on the Match display scale, 1/3 .. 1 in 0.05 steps (1/3: 640x360 of a 1080p window).
// Down quickly while over budget; up only after sustained headroom, and the wait before the next raise doubles each
// time a raise is undone within 10 s (hunting between two steps: 24-33 changes a minute in the field before).
// It reacts to the GPU only: a lower resolution doesn't help a frame the CPU / the game made slow (a level streaming
// in, a shader compiling: 100+ ms frames with the GPU nearly idle, which used to step it down on a fast GPU and, with
// the slow raises, keep it low for a minute). With GPU times (Metal's command buffers, Vulkan's timestamps) it goes by
// those alone. Without, by the frame times with the spikes left out: the mean of the half second's frames up to 3x their
// median (a frame missing one vsync, 33 ms, still counts: that's how a slow GPU shows at 60 Hz). Either way a spike
// neither steps it down nor, so, doubles the raise wait. LARECOMP_DYN_NO_GPU=1: ignore the GPU times (tests the
// frame-time path); LARECOMP_SPIKE_MS (pace.c): CPU-side spikes.
//
// The GPU time has two parts: the game's (G, scales with the factor: about its area, factor^2) and the window step's
// (W: the image drawn into the window - upscale, SMAA - and the menu: depends on the window's size only). The budget
// is for G + W (87% of a 60 Hz frame); a raise must be predicted to fit with room to spare (G x (new / old)^2 + W under
// 90% of it: no fixed raise threshold, so it settles one step under where it would go over instead of hunting), and a
// step down goes straight to where G + W is predicted to fit (at most 0.25 at once).
// Held: when even the floor (1/3) misses 60 fps and the frames wait for the display (vsync: a frame every 33 ms
// for G + W = 17), going lower gains nothing, so the budget becomes 87% of that frame interval (whole 60 Hz refreshes:
// 33.3 / 50 ms) and the factor goes back up as far as it fits there (a large W - 4K SMAA / FSR 1 on an integrated
// GPU - or a slow GPU at a big window). A hold keeps measuring: G + W over the hold's budget for a second lowers the
// factor; at the floor for 1.5 s, the hold steps down from 30 to 20 fps (G at the floor can grow after the hold began:
// UHD 770 4K 19.4 -> 27.9 ms, frames every 49-65 ms while it said 30). It steps back up - 20 to 30, 30 to looking for
// 60 - only when the floor is predicted to fit the faster rate with 20% to spare for 2 s (after 10 s, doubling each
// time that fails or is undone within 30 s), and starts over when W changes (another upscaler / anti-aliasing / window
// size). Frames that don't wait (mailbox: a frame every G + W) keep going down to the floor: there every step down is
// a faster frame. tools/test_dynres.py: the controller against a simulated GPU (the UHD 770 cases).
#define DYN_N 30            // frames per decision (~half a second)
#define DYN_FLOOR (1.0 / 3)
#define DYN_BUDGET 14.5     // ms of GPU work per frame at 60 fps (G + W)
#define DYN_RAISE_FIT 0.9   // a raise: predicted G + W under this share of the budget
#define DYN_HOLD_UP_FIT 0.8 // a hold steps up: the floor's predicted G + W under this share of the faster rate's budget
#define DYN_HOLD_MAX 3      // vsyncs a hold waits at most (60 Hz: 20 fps)
static double g_dyn = 1.0;
static double g_dyn_gpu_sum, g_dyn_win_sum, g_last_t, g_dyn_dt[DYN_N];
static int g_dyn_n, g_dyn_gpu_n, g_dyn_good;
static double g_dyn_cool_until, g_dyn_raise_wait = 3.0, g_dyn_raise_at;
static double g_dyn_last_raise = -100, g_dyn_last_lower = -100;
static double g_hold_dt, g_hold_w;               // held: the frame interval at the floor (> 0) and W then
static double g_probe_at, g_probe_wait = 10.0;  // looking for 60 again (from a hold)
static bool g_probing;
// held: G at the floor followed through the scene's load - measured there as the hold began, then scaled by how G
// changed at each factor since (G right after a step is the reference at the new factor): the part of G that
// doesn't scale with the resolution makes area estimates optimistic (UHD 770 1080p: ~6.5 of 9.1 ms at 0.50)
static double g_floor_g, g_ref_g;
static bool g_ref_pending;
static int g_hold_miss, g_hold_up_votes;  // held: decisions in a row over the hold's budget / with room for the next rate
static int g_hold_met_votes;              // held at 20: decisions in a row whose frames already met 30 (with G + W in its budget)
static double g_hold_up_at = -100;        // when the hold last stepped up (20 -> 30)

static int hold_vsyncs(void) { return (int)lround(g_hold_dt * 60 / 1000); }
static bool at_floor(void) { return g_dyn <= DYN_FLOOR + 1e-6; }
// the 0.05 grid, the floor below its last step
static double dyn_grid(double f) { return f <= DYN_FLOOR + 1e-6 ? DYN_FLOOR : fmax(DYN_FLOOR, round(f * 20) / 20); }
// the factor where G + W is predicted to fit `budget` (G scales with the area), on the grid
static double dyn_fit(double G, double W, double budget) {
    double fit = g_dyn * sqrt(fmax(0, budget - W) / G);
    return fmax(DYN_FLOOR, floor(fit * 20 + 1e-6) / 20);
}

static DynSplit g_split;  // sums since the last decision
static int g_split_n;
void output_scale_note_split(const DynSplit *d) {
    if (g_split_n >= 600) { memset(&g_split, 0, sizeof g_split); g_split_n = 0; }  // (not deciding: Dynamic is off)
    g_split.game += d->game; g_split.win += d->win; g_split.span += d->span; g_split.frames += d->frames;
    g_split.w_slot += d->w_slot; g_split.w_acquire += d->w_acquire; g_split.w_present += d->w_present; g_split.w_other += d->w_other;
    g_split.win_counted = d->win_counted;
    g_split_n++;
}

static double g_sum_ms;  // (Metal) spans summed since the last decision
static int g_sum_n;
void output_scale_note_sum(double ms) {
    if (g_sum_n >= 600) g_sum_ms = 0, g_sum_n = 0;
    g_sum_ms += ms, g_sum_n++;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

// game_ms: the game's GPU time of the last frame (< 0: unknown); win_ms: the window step's (< 0: unknown / not counted)
static void dyn_update(double now, double game_ms, double win_ms) {
    static int no_gpu = -1;
    if (no_gpu < 0) no_gpu = getenv("LARECOMP_DYN_NO_GPU") && atoi(getenv("LARECOMP_DYN_NO_GPU"));
    if (no_gpu) game_ms = -1;
    static int start = -1;  // LARECOMP_DYN_START=<f>: Dynamic starts at factor f (tests: climbing back from the floor)
    if (start < 0) {
        start = 0;
        if (getenv("LARECOMP_DYN_START")) g_dyn = dyn_grid(fmin(1.0, atof(getenv("LARECOMP_DYN_START"))));
    }
    double dt = g_last_t > 0 ? (now - g_last_t) * 1000.0 : 16.7;
    g_last_t = now;
    if (dt > 250) return;  // (loading, menus, a stall: not the GPU's steady state)
    g_dyn_dt[g_dyn_n++] = dt;
    if (game_ms >= 0) { g_dyn_gpu_sum += game_ms; g_dyn_win_sum += fmax(0, win_ms); g_dyn_gpu_n++; }
    if (g_dyn_n < DYN_N) return;  // decide every ~half second
    // frame times: median, and the mean without the outliers (single CPU-side spikes)
    double sorted[DYN_N], kept = 0;
    memcpy(sorted, g_dyn_dt, sizeof sorted);
    qsort(sorted, DYN_N, sizeof *sorted, cmp_double);
    double med = (sorted[DYN_N / 2 - 1] + sorted[DYN_N / 2]) / 2;
    int nkept = 0;
    for (int i = 0; i < DYN_N; i++) if (sorted[i] <= 3 * med) { kept += sorted[i]; nkept++; }
    bool gpu = g_dyn_gpu_n >= DYN_N / 2;
    double avg_dt = kept / nkept, G = gpu ? g_dyn_gpu_sum / g_dyn_gpu_n : -1, W = gpu ? g_dyn_win_sum / g_dyn_gpu_n : 0;
    double avg_gpu = gpu ? G + W : -1;
    int spikes = DYN_N - nkept;
    g_dyn_gpu_sum = g_dyn_win_sum = 0;
    g_dyn_n = g_dyn_gpu_n = 0;
    double sum_ms = g_sum_n ? g_sum_ms / g_sum_n : -1;
    g_sum_ms = 0, g_sum_n = 0;
    if (now < g_dyn_cool_until) return;
    double prev = g_dyn;
    static int log = -1;  // LARECOMP_LOG_DYNRES=1: every decision, not only the steps
    if (log < 0) log = getenv("LARECOMP_LOG_DYNRES") != NULL;
    double budget = DYN_BUDGET;
    bool hold_up = false;  // (20 -> 30 fps this time: the factor was set to fit it already)
    if (gpu) {
        if (g_hold_dt > 0 && fabs(W - g_hold_w) > fmax(1.0, 0.25 * g_hold_w)) {  // another upscaler / AA / window size
            LOG("[dynres] window step %.1f -> %.1f ms: hold at %.0f fps ended\n", g_hold_w, W, 1000 / g_hold_dt);
            g_hold_dt = 0, g_probing = false, g_probe_wait = 10.0, g_hold_miss = g_hold_up_votes = g_hold_met_votes = 0;
        }
        // held at 20 while the frames already come at 30 (every 33.3 ms) with G + W inside the 30 hold's budget (10%
        // to spare), 1 s in a row: it holds 30 again right away (no probe wait - unless the last step up was undone)
        int hvs = g_hold_dt > 0 ? hold_vsyncs() : 0;
        double faster = (hvs - 1) * 1000 / 60.0;
        bool met = hvs > 2 && avg_dt <= 1.05 * faster && avg_gpu <= faster * 0.87 * DYN_RAISE_FIT;
        g_hold_met_votes = met ? g_hold_met_votes + 1 : 0;
        if (g_hold_dt > 0 && g_ref_pending) g_ref_g = G, g_ref_pending = false;  // (the first measure at a new factor)
        if (g_hold_dt > 0 && g_ref_g > 0) {
            // the next faster rate: 20 -> 30 fps, 30 -> looking for 60; the floor must fit it with room, 2 s in a row
            double g_floor = g_floor_g * G / g_ref_g;
            int vs = hvs;
            double up_budget = vs > 2 ? (vs - 1) * 1000 / 60.0 * 0.87 : DYN_BUDGET;
            bool room = g_floor + W <= up_budget * DYN_HOLD_UP_FIT;
            g_hold_up_votes = room ? g_hold_up_votes + 1 : 0;
            bool met_now = met && g_hold_met_votes >= 2 && (g_probe_wait <= 10.0 || now >= g_probe_at);
            if ((room && g_hold_up_votes >= 4 && now >= g_probe_at) || met_now) {
                g_hold_up_votes = g_hold_miss = g_hold_met_votes = 0;
                if (vs > 2) {
                    g_hold_dt = (vs - 1) * 1000 / 60.0;
                    g_hold_up_at = now, g_probe_at = now + g_probe_wait;
                    double to = fmin(g_dyn, dyn_fit(G, W, g_hold_dt * 0.87 * DYN_RAISE_FIT));
                    if (met_now)
                        LOG("[dynres] frames already meet %.0f fps (every %.1f ms, game %.1f + window %.1f ms at %.2f): holding "
                            "%.0f fps\n", 1000 / g_hold_dt, avg_dt, G, W, g_dyn, 1000 / g_hold_dt);
                    else
                        LOG("[dynres] the floor would fit %.0f fps now (game %.1f ms at %.2f: ~%.1f at %.2f, window %.1f): holding "
                            "%.0f fps, %.2f -> %.2f\n", 1000 / g_hold_dt, G, g_dyn, g_floor, DYN_FLOOR, W, 1000 / g_hold_dt, g_dyn, to);
                    if (to != g_dyn) g_floor_g *= G / g_ref_g, g_ref_pending = true, g_dyn = to;
                    hold_up = true;
                } else {
                    LOG("[dynres] the floor would fit 60 fps now (game %.1f ms at %.2f: ~%.1f at %.2f, window %.1f): looking for it\n",
                        G, g_dyn, g_floor, DYN_FLOOR, W);
                    g_hold_dt = 0, g_probing = true;
                }
            }
        }
        if (g_hold_dt > 0) budget = g_hold_dt * 0.87;
        // 60 out of reach at the floor and the frames wait for the display: lower gains nothing, hold that frame rate
        // (whole refreshes of a 60 Hz display: a floor that alternates 1 and 2 of them - 40 fps - beats 30 at a higher
        // factor; at most 3 refreshes, 20 fps)
        double vsyncs = fmin(floor(avg_dt / (1000 / 60.0) + 0.1), DYN_HOLD_MAX);
        static int votes;  // (two half seconds in a row)
        bool held_back = g_hold_dt == 0 && at_floor() && avg_gpu > DYN_BUDGET && vsyncs >= 2 && avg_dt >= 1.25 * avg_gpu;
        votes = held_back ? votes + 1 : 0;
        if (held_back && votes >= 2) {
            g_hold_dt = vsyncs * 1000 / 60.0, g_hold_w = W;
            g_floor_g = g_ref_g = G, g_ref_pending = false;
            g_hold_miss = g_hold_up_votes = g_hold_met_votes = 0;
            if (g_probing) g_probe_wait = fmin(g_probe_wait * 2, 160.0);
            g_probing = false;
            g_probe_at = now + g_probe_wait;
            budget = g_hold_dt * 0.87;
            if (W >= 0.4 * 1000 / 60.0 || W >= G)
                LOG("[dynres] window step %.1f ms is the limit (game %.1f ms at %.2f, a frame every %.1f ms): 60 fps out of "
                    "reach, holding %.0f fps; the factor goes up while G + W fits %.1f ms\n", W, G, g_dyn, avg_dt,
                    1000 / g_hold_dt, budget);
            else
                LOG("[dynres] game %.1f + window %.1f ms at %.2f (a frame every %.1f ms): 60 fps out of reach, holding %.0f "
                    "fps; the factor goes up while G + W fits %.1f ms\n", G, W, g_dyn, avg_dt, 1000 / g_hold_dt, budget);
        }
        if (g_probing && g_hold_dt == 0 && avg_gpu <= DYN_BUDGET && avg_dt < 20) g_probing = false, g_probe_wait = 10.0;
    }
    bool slow = gpu ? avg_gpu > budget : avg_dt > 17.4;
    // held, but the frames came slower than the hold's (the display waits one more refresh): over budget too
    if (gpu && g_hold_dt > 0 && avg_dt > 1.2 * g_hold_dt && avg_gpu > 0.8 * budget) slow = true;
    bool hold_wait = hold_up;  // held and over its budget, not yet long enough to act on it (or the hold just stepped up)
    if (gpu && g_hold_dt > 0) {
        g_hold_miss = slow && !hold_up ? g_hold_miss + 1 : 0;
        if (slow && at_floor() && g_hold_miss >= 3) {  // nothing lower: the hold steps down (30 -> 20 fps)
            int vs = hold_vsyncs();
            if (vs < DYN_HOLD_MAX) {
                LOG("[dynres] game %.1f + window %.1f ms at %.2f misses the %.0f fps hold's %.1f ms (a frame every %.1f ms): "
                    "holding %.0f fps\n", G, W, g_dyn, 1000 / g_hold_dt, budget, avg_dt, 60.0 / (vs + 1));
                if (now - g_hold_up_at < 30.0) g_probe_wait = fmin(g_probe_wait * 2, 160.0);  // (the step up didn't hold)
                g_hold_dt = (vs + 1) * 1000 / 60.0;
                budget = g_hold_dt * 0.87;
                g_floor_g = g_ref_g = G, g_ref_pending = false;
                g_probe_at = now + g_probe_wait;
                g_hold_up_votes = 0;
            }
            g_hold_miss = 0;
            hold_wait = true;
        } else if (slow && (at_floor() || g_hold_miss < 2)) hold_wait = true;
        if (slow && !hold_wait) g_hold_miss = 0;
    }
    if (hold_wait) g_dyn_good = 0;
    else if (slow) {
        double down = g_dyn - ((gpu ? avg_gpu > budget * 1.3 : avg_dt > 22) ? 0.1 : 0.05);
        if (gpu && G > 0) {  // straight to where G + W is predicted to fit
            down = fmin(down, dyn_fit(G, W, budget));
            down = fmax(down, g_dyn - 0.25);
        }
        g_dyn = fmax(DYN_FLOOR, down);
        g_dyn_good = 0;
        // the raise didn't hold (spikes can't undo one any more: they're not in the measure)
        if (now - g_dyn_last_raise < 10.0) g_dyn_raise_wait = fmin(g_dyn_raise_wait * 2, 60.0);
        g_dyn_last_lower = now;
        g_dyn_raise_at = now + g_dyn_raise_wait;
    } else if (g_dyn < 1.0) {
        g_dyn_good++;
        // headroom for a while, and not too soon after the last step down. With GPU timing: the next step is predicted
        // to fit (G scales with the area); without: frames at 60
        double up = fmin(1.0, dyn_grid(g_dyn + 0.05));  // (from the floor: 0.40)
        bool fits = gpu ? G * (up / g_dyn) * (up / g_dyn) + W <= budget * DYN_RAISE_FIT : avg_dt < 16.9;
        bool room = fits && g_dyn_good >= 6 && now >= g_dyn_raise_at;
        if (room) { g_dyn = up; g_dyn_good = 0; g_dyn_last_raise = now; g_dyn_raise_at = now + g_dyn_raise_wait; }
        else if (now - g_dyn_last_lower > 60.0 && g_dyn_raise_wait > 3.0) g_dyn_raise_wait = 3.0;  // (a minute of calm: forget the hunting)
    }
    g_dyn = dyn_grid(g_dyn);
    if (gpu && g_hold_dt > 0 && g_dyn != prev && !g_ref_pending && g_ref_g > 0)  // the floor's G as of now; G's new reference next time
        g_floor_g *= G / g_ref_g, g_ref_pending = true;
    if (g_dyn != prev || log) {
        if (g_dyn != prev) g_dyn_cool_until = now + 1.0;
        char split[320] = "";
        if (g_split_n) {  // (Vulkan) per frame: GPU by part, and where the CPU waited
            double n = g_split_n, nf = g_split.frames > 0 ? g_split.frames : n;
            snprintf(split, sizeof split, "; per frame: gpu game %.1f + window %.1f%s, span %.1f, gap %.1f ms; cpu waits slot %.1f acquire %.1f "
                     "present %.1f other %.1f ms", g_split.game / n, g_split.win / n, g_split.win_counted ? "" : " (not counted)",
                     g_split.span / nf, g_split.span / nf - (g_split.game + g_split.win) / n, g_split.w_slot / n, g_split.w_acquire / n,
                     g_split.w_present / n, g_split.w_other / n);
        }
        if (sum_ms >= 0) snprintf(split, sizeof split, "; spans summed %.1f ms", sum_ms);
        char hold[48] = "";
        if (g_hold_dt > 0) snprintf(hold, sizeof hold, ", holding %.0f fps", 1000 / g_hold_dt);
        LOG("[dynres] %.2f -> %.2f (frame %.1f ms, median %.1f, %d spike%s left out, gpu %.1f ms = game %.1f + window %.1f, "
            "budget %.1f%s, next raise in %.0f s%s)\n", prev, g_dyn, avg_dt, med, spikes, spikes == 1 ? "" : "s", avg_gpu, G, W,
            budget, hold, fmax(0, g_dyn_raise_at - now), split);
    }
    memset(&g_split, 0, sizeof g_split);
    g_split_n = 0;
}

double output_scale_dynamic_factor(void) { return g_dyn; }
double output_scale_dynamic_hold_fps(void) { return g_hold_dt > 0 ? 1000 / g_hold_dt : 0; }

void output_scale_log(unsigned long long frame, double dw, double dh, const OutScale *o) {
    static double psx, psy, pdw, pdh;
    static int nlog;
    if ((o->sx != psx || o->sy != psy || dw != pdw || dh != pdh) && nlog++ < 200)
        LOG("[scale] frame %llu drawable %.0fx%.0f -> render scale %.3fx%.3f (reshape %d)\n", frame, dw, dh, o->sx, o->sy,
            o->shape != 1.0);
    psx = o->sx, psy = o->sy, pdw = dw, pdh = dh;
}

void output_scale(double dw, double dh, bool reshape_allowed, double now, double game_ms, double win_ms, OutScale *o) {
    double fit = fmin(dw / 1920.0, dh / 1080.0);
    static double forced = -1, wide_test = -1;
    if (forced < 0) forced = getenv("LARECOMP_SCALE") ? atof(getenv("LARECOMP_SCALE")) : 0;
    if (wide_test < 0) wide_test = getenv("LARECOMP_WIDE_TEST") ? atof(getenv("LARECOMP_WIDE_TEST")) : 0;
    double shape = wide_test > 0 ? wide_test : (dw / dh) / (16.0 / 9.0);
    bool reshape = reshape_allowed && fabs(shape - 1.0) > 0.005;
    if (!reshape) shape = 1.0;
    // render height in units of the Switch's 1080 lines
    double h, dyn = 1.0;
    int mode = g_settings.res_mode;
    if (forced > 0) h = forced;
    else if (mode >= 1 && mode <= 8) h = mode == 1 && g_gpu_integrated ? 720 / 1080.0 : mode;
    else {
        h = reshape ? dh / 1080.0 : fit;  // Match display
        if (mode == RES_DYNAMIC) {
            static double pinned = -1;
            if (pinned < 0) pinned = getenv("LARECOMP_DYN_TEST") ? atof(getenv("LARECOMP_DYN_TEST")) : 0;
            if (pinned > 0) dyn = pinned;
            else { dyn_update(now, game_ms, win_ms); dyn = g_dyn; }
        }
        else if (mode == RES_Q75) h *= 0.75;
        else if (mode == RES_Q25) h *= 0.25;
        else if (mode == RES_Q50) h *= 0.5;
    }
    h = fmax(h, 0.1);  // (.25x of a small window)
    double ax = h * shape, ay = h;
    double cap = fmin(MAX_TEX / (1920.0 * ax), MAX_TEX / (1080.0 * ay));
    if (cap < 1.0) { ax *= cap; ay *= cap; }
    o->ax = ax; o->ay = ay;
    o->sx = ax * dyn; o->sy = ay * dyn; o->shape = shape;
    // where the image goes in the window: fills it when reshaped, else a fitted 16:9 box
    if (reshape) { o->vw = dw; o->vh = dh; }
    else { o->vw = 1920.0 * fit; o->vh = 1080.0 * fit; }
    if (wide_test > 0) {
        double f2 = fmin(dw / (1920.0 * wide_test), dh / 1080.0);
        o->vw = 1920.0 * wide_test * f2; o->vh = 1080.0 * f2;
    }
    o->vx = (dw - o->vw) / 2; o->vy = (dh - o->vh) / 2;
}
