// Dynamic resolution's controller (runtime/nvn/output_scale.c) driven by a simulated GPU: no game files, no window.
// Build + run: tools/test_dynres.py (prints each case's factor path; exits non-zero when a case misbehaves).
// The GPU: the game's part G = (a + b f^2) x scene x noise at factor f (a: what doesn't scale with the resolution), the
// window step W (upscale / SMAA / menu: the window's size only). A frame takes G + W of GPU; with vsync and the
// frames waiting for the display (fifo, as on Windows' Intel driver) it lasts the next multiple of 16.7 ms, with
// mailbox max(16.7, G + W). Cases from an Intel UHD 770 (Field, Dynamic): G and W as measured at the factor it sat at.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/settings.h"
#include "../runtime/nvn/output_scale.h"

Settings g_settings = {.res_mode = RES_DYNAMIC};
bool g_gpu_integrated = true;

static unsigned g_rng = 1;
static double rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return ((g_rng >> 8) & 0xffff) / 65535.0; }

typedef struct {
    const char *name;
    double a, b, w;     // G = a + b f^2, W
    int fifo;           // frames wait for the display
    double light_at;    // > 0: the scene gets lighter (x0.6) from then on (s)
    double secs;
    double wobble;      // the scene's load swings +-this over 20 s (0: 3%; frame to frame +-2%: the floor's 30.6 fps)
    double heavy_at, heavy, heavy_until;  // > 0: G x heavy from heavy_at (s) until heavy_until (0: the end)
} Case;

typedef struct {
    double f_end, fps_end, hold_end;  // hold_end: the frame rate the controller says it holds (0: none)
    int changes_last60, hold_changes;  // hold_changes: the held rate changed (30 <-> 20 <-> 60), the whole run
    double f_min_last60, f_max_last60;
    double hold_t;  // when the held rate last changed (s)
} Result;

static Result run(const Case *c) {
    g_rng = 7;
    double now = 1.0, f = output_scale_dynamic_factor(), g_prev = -1, w_prev = -1;
    double frames_end = 0, t_end = 0, last_f = f, f_min = 1, f_max = 0, last_hold = 0, hold_t = 0;
    int changes = 0, hold_changes = 0;
    printf("%s\n  t=0s %.2f", c->name, f);
    int col = 0;
    while (now < c->secs) {
        OutScale o;
        output_scale(3840, 2160, false, now, g_prev, w_prev, &o);
        f = output_scale_dynamic_factor();
        if (f != last_f) {
            printf(" | %.0fs %.2f", now, f);
            if (++col % 8 == 0) printf("\n ");
            if (now > c->secs - 60) changes++;
            last_f = f;
        }
        double hold = output_scale_dynamic_hold_fps();
        if (hold != last_hold) {
            printf(" | %.0fs hold %.0f", now, hold);
            if (++col % 8 == 0) printf("\n ");
            hold_changes++, last_hold = hold, hold_t = now;
        }
        if (now > c->secs - 60) { f_min = fmin(f_min, f); f_max = fmax(f_max, f); }
        double scene = (1 + (c->wobble > 0 ? c->wobble : 0.03) * sin(now * 2 * M_PI / 20)) * (c->light_at > 0 && now > c->light_at ? 0.6 : 1.0);
        if (c->heavy_at > 0 && now > c->heavy_at && (c->heavy_until <= 0 || now < c->heavy_until)) scene *= c->heavy;
        double g = (c->a + c->b * f * f) * scene * (0.98 + 0.04 * rnd()), w = c->w * (0.99 + 0.02 * rnd());
        double gw = g + w, p = c->fifo ? ceil(gw / 16.667 - 1e-9) * 16.667 : gw;
        if (p < 16.667) p = 16.667;
        g_prev = g, w_prev = w;
        now += p / 1000;
        if (now > c->secs - 10) frames_end++, t_end += p / 1000;
    }
    Result r = {f, frames_end / t_end, last_hold, changes, hold_changes, f_min, f_max, hold_t};
    printf("\n  end: factor %.2f, %.1f fps over the last 10 s (holding %.0f), %d change%s in the last 60 s (%.2f-%.2f)\n\n",
           r.f_end, r.fps_end, r.hold_end, changes, changes == 1 ? "" : "s", f_min, f_max);
    return r;
}

static int g_fail;
static int near(double a, double b) { return fabs(a - b) < 0.5; }
static void check(int ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n\n", what); g_fail = 1; }
}

int main(int argc, char **argv) {
    // (each case is its own process: the controller's state is static) argv[1]: the case's index
    static const Case cases[] = {
        // 4K window, bilinear: G 16.3 W 0.9 at 0.50 -> 30.6 fps (fifo halving)
        {"4K bilinear (G 16.3 + W 0.9 at 0.50, fifo)", 4.1, 48.8, 0.9, 1, 0, 180},
        // 4K, SMAA + FSR 1: G 13.1 W 20.2 at 0.50 -> 20 fps
        {"4K SMAA + FSR 1 (G 13.1 + W 20.2 at 0.50, fifo)", 3.3, 39.2, 20.2, 1, 0, 180},
        // 1080p, bilinear: G 10.9 W 0.2 at 0.65, hunting 0.65 <-> 0.75 before (the old raise needed G < 10.5, a step
        // down G + W > 14.5): G = 0.3 + 25.1 f^2 (10.9 at 0.65, 14.4 at 0.75), the load swinging +-5%
        {"1080p bilinear (G 10.9 + W 0.2 at 0.65, fifo)", 0.3, 25.1, 0.2, 1, 0, 180, 0.05},
        // 1080p, SMAA + FSR 1: G 9.1 W 8.1 at 0.50 -> 30 fps (G = 6.5 + 10.4 f^2: a large part that doesn't scale)
        {"1080p SMAA + FSR 1 (G 9.1 + W 8.1 at 0.50, fifo)", 6.5, 10.4, 8.1, 1, 0, 180},
        // control: the frames don't wait (mailbox): every step down is a faster frame, so down to the floor as before
        {"4K bilinear, mailbox (G 16.3 + W 0.9 at 0.50)", 4.1, 48.8, 0.9, 0, 0, 120},
        // the scene gets lighter (x0.6) at 90 s: 60 fps fits at the floor again
        {"4K bilinear, lighter scene from 90 s (fifo)", 4.1, 48.8, 0.9, 1, 90, 240},
        // pure f^2 GPU (nothing fixed): the most optimistic model for steps down
        {"4K bilinear, G all f^2 (G 16.3 at 0.50, fifo)", 0, 65.2, 0.9, 1, 0, 180},
        // a fast GPU: 4K at 1.0 fits (G 11 + W 1)
        {"fast GPU, 4K (G 11 + W 1 at 1.00)", 2, 9, 1, 1, 0, 60},
        // a busier scene: its load swings +-15%: it follows the scene, the hold stays
        {"4K bilinear, load swinging +-15% (fifo)", 4.1, 48.8, 0.9, 1, 0, 180, 0.15},
        // 1080p bilinear with the load swinging +-15%: steps with the scene, slowly (the raise wait doubles)
        {"1080p bilinear, load swinging +-15% (fifo)", 0.3, 25.1, 0.2, 1, 0, 180, 0.15},
        // the game's own scaler off (it rendered 1280x720 here before): 1080p at 0.50 G 13.9-14.5 ms, 43.7 fps
        {"1080p bilinear, game scaler off (G 14.2 + W 0.2 at 0.50, fifo)", 3, 44.8, 0.2, 1, 0, 180},
        // 4K, SMAA + FSR 1 with the game scaler off: G + W 44 ms at 0.50 (no reaction before: at the floor already)
        {"4K SMAA + FSR 1, game scaler off (G 23.8 + W 20.2 at 0.50, fifo)", 6, 71.2, 20.2, 1, 0, 180},
        // a hold whose G grows (UHD 770 4K bilinear: 19.4 -> 27.9 ms at 0.50 after holding 30; G mostly fixed here):
        // x1.44 from 60 s - the floor still fits 30
        {"4K bilinear, G x1.44 from 60 s (G 19.4 at 0.50, fifo)", 10, 37.6, 0.9, 1, 0, 180, 0, 60, 1.44},
        // a slower GPU holding 30 at the floor (G 18.2 there), G x1.8 from 60 s: not even the floor fits 30 any more
        // (frames every 50 ms) - the hold steps down to 20
        {"4K bilinear, held 30, G x1.8 from 60 s (fifo)", 14, 37.6, 0.9, 1, 0, 180, 0, 60, 1.8},
        // the same, x1.8 from 60 s to 150 s: 20, then back to 30 once the floor fits it with room
        {"4K bilinear, held 30, G x1.8 from 60 s to 150 s (fifo)", 14, 37.6, 0.9, 1, 0, 300, 0, 60, 1.8, 150},
        // x1.8 swinging +-15%: 20 fps, no hunting between 20 and 30
        {"4K bilinear, held 30, G x1.8 from 30 s, load swinging +-15% (fifo)", 14, 37.6, 0.9, 1, 0, 240, 0.15, 30, 1.8},
    };
    int n = sizeof cases / sizeof *cases;
    if (argc < 2) { printf("%d\n", n); return 0; }
    int i = atoi(argv[1]);
    Result r = run(&cases[i]);
    // (the floor is 1/3 since the game's scaler is off: the cases that held 30 at 0.50 reach 60 under it)
    switch (i) {
    case 0: check(r.f_end >= 0.35 && r.fps_end > 58 && r.changes_last60 == 0, "4K bilinear: 60 under 0.50"); break;
    case 1: check(r.fps_end > 28 && r.changes_last60 <= 2, "4K FSR: 30 fps at the floor"); break;
    case 2: check(r.fps_end > 58 && r.changes_last60 == 0 && r.f_end >= 0.65, "1080p bilinear: settle at 60"); break;
    case 3: check(r.fps_end > 58 && r.changes_last60 <= 2, "1080p FSR: 60 at the floor"); break;
    case 4: check(r.fps_end > 58 && r.f_end <= 0.45, "mailbox: 60 under 0.50"); break;
    case 5: check(r.fps_end > 58, "lighter scene: back to 60"); break;
    case 6: check(r.f_end >= 0.35 && r.fps_end > 58 && r.changes_last60 == 0, "4K bilinear f^2: 60 under 0.50"); break;
    case 7: check(r.f_end == 1.0 && r.fps_end > 58, "fast GPU: full resolution"); break;
    case 8: check(r.f_min_last60 >= 0.35 && r.fps_end > 58 && r.changes_last60 <= 6, "4K bilinear +-15%: 60, steps with the scene"); break;
    case 9: check(r.fps_end > 58 && r.changes_last60 <= 4, "1080p bilinear +-15%: 60, few steps"); break;
    case 10: check(r.fps_end > 58 && r.changes_last60 == 0, "1080p game scaler off: 60"); break;
    case 11: check(near(r.hold_end, 20) && r.fps_end > 19 && r.changes_last60 <= 2 && r.hold_changes <= 2, "4K FSR 44 ms: hold 20"); break;
    case 12: check(near(r.hold_end, 30) && r.fps_end > 28 && r.changes_last60 <= 2, "G x1.44: hold 30 at a lower factor"); break;
    case 13: check(near(r.hold_end, 20) && r.fps_end > 19 && r.changes_last60 <= 2 && r.hold_changes <= 3, "G x2.2: hold steps down to 20"); break;
    case 14: check(near(r.hold_end, 30) && r.fps_end > 28 && r.changes_last60 <= 2 && r.hold_changes <= 4 && r.hold_t < 154,
                   "G x1.8 then back: 20, then 30 within a few seconds"); break;
    case 15: check(near(r.hold_end, 20) && r.fps_end > 19 && r.hold_changes <= 3, "G x2.2 +-15%: 20, no hunting"); break;
    }
    // a hold is real: the frames come at least at the rate it says
    if (r.hold_end > 0) check(r.fps_end >= 0.95 * r.hold_end, "the held frame rate is met");
    return g_fail;
}
