// Phones and tablets: with Aspect ratio = Unlocked and a window wider or taller than 16:9, the game's HUD goes to the real
// screen corners instead of the centred 16:9 box, in every room so it never jumps at a door; and optionally its X / Y item
// buttons are hidden while the on-screen controls (which show those items) are up.
//
// Game thread (rt_hud_corners_tick, from the HUD's state update, HUD_HEARTS_SITE, x0 = the HUD): each group pane under
// Cm_HUD's N_HUD_00 gets translate x (+0x30) = authored x -/+ the shift, then the game's own "matrix dirty" bit
// (+0x58 |= 0x10, as at PROMPT_POS_SITE). Absolute writes: the same each frame, the authored x kept from the first.
//
// Render thread: layout draws go into the 16:9 box (renderer.m / vulkan/exec.c). Those draws get the window-wide viewport
// (centred stretch k) and a copy of their uniforms with the projection's x row divided by k: an unmoved pane lands on the
// same pixels, a moved one in the bars. Boxed rooms are cropped to 16:9 at present, so while the HUD is there the
// bars are cleared after the scene composite and the frame is shown whole (widescreen_present_crop).
#include "../game_addresses.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"
#include "../settings.h"

float widescreen_factor(void);
bool world_on_screen(void);
extern int game_in_play(void);

static volatile float g_left, g_right, g_up;  // layout units (1280 x 720 box) the HUD's groups moved by

float hud_corners_left(void) { return g_left; }
float hud_corners_right(void) { return g_right; }
float hud_corners_up(void) { return g_up; }

#if PLAT_TOUCH

static volatile bool g_active;
bool touch_visible(void);
void menu_safe_insets(float *l, float *r);  // share of the window's width

// (direct children of N_HUD_00 only: sub-layouts reuse these names)
enum { LEFT = -1, KEEP = 0, RIGHT = 1 };
static const struct { const char *name; int side; bool xy; } kPanes[] = {
    {"N_Life_00", LEFT, false},  {"N_Key_00", LEFT, false},         {"N_Item_00", RIGHT, true},
    {"N_Item_01", RIGHT, true},  {"N_SetItem_00", RIGHT, true},    {"N_Rupee_00", RIGHT, false},
    {"N_ShellRadar_00", RIGHT, false},
};
#define NPANES (sizeof kPanes / sizeof kPanes[0])
static uint64_t g_hud, g_pane[NPANES], g_retry;
static int g_tries;
static float g_authored[NPANES], g_authored_y[NPANES];
static bool g_moved, g_hid[NPANES];

static bool name_is(uint64_t p, const char *n) { return !strcmp((const char *)(uintptr_t)(p + 0xB0), n); }
static uint64_t g_ra[256], g_rs[256];
static int g_nr;
static bool mapped(uint64_t p) {  // (a pointer found by scanning, or a stale pane's parent: any value at all)
    for (int k = 0; k < g_nr; k++)
        if (p >= g_ra[k] && p + 0x800 > p && p + 0x800 <= g_ra[k] + g_rs[k]) return true;
    return false;
}
static bool child_of_hud(uint64_t p) {
    uint64_t up = LD64(p + 0x18);
    return mapped(up) && name_is(up, "N_HUD_00");
}

// breadth first from the HUD over pointers into guest data, as runtime/ui/tracker_overlay.mm finds its panes
static void find_panes(uint64_t hud) {
    enum { MAX_SEEN = 1 << 15, TAB = 1 << 17 };
    static uint64_t q[MAX_SEEN], tab[TAB];
    memset(tab, 0, sizeof tab);
    unsigned n = 1, found = 0;
    q[0] = hud;
    memset(g_pane, 0, sizeof g_pane);
    for (unsigned i = 0; i < n && found < NPANES; i++)
        for (uint32_t off = 0; off < 0x800 && found < NPANES; off += 8) {
            uint64_t p = LD64(q[i] + off);
            if (!mapped(p)) continue;
            unsigned h = (unsigned)(p * 0x9E3779B1u >> 15) & (TAB - 1);
            while (tab[h] && tab[h] != p) h = (h + 1) & (TAB - 1);
            if (tab[h] || n >= MAX_SEEN) continue;
            tab[h] = p, q[n++] = p;
            if (*(const char *)(uintptr_t)(p + 0xB0) != 'N' || !child_of_hud(p)) continue;
            for (unsigned k = 0; k < NPANES; k++)
                if (!g_pane[k] && name_is(p, kPanes[k].name)) {
                    g_pane[k] = p, g_authored[k] = *(const float *)(uintptr_t)(p + 0x30), g_hid[k] = false;
                    g_authored_y[k] = *(const float *)(uintptr_t)(p + 0x34);
                    found++;
                }
        }
    LOG("[hud] corners: %u of %u panes found from %llx (%u objects)\n", found, (unsigned)NPANES, (unsigned long long)hud, n);
}

static void set_xy(unsigned i, float x, float y) {
    uint64_t p = g_pane[i];
    if (*(const float *)(uintptr_t)(p + 0x30) == x && *(const float *)(uintptr_t)(p + 0x34) == y) return;
    *(float *)(uintptr_t)(p + 0x30) = x;
    *(float *)(uintptr_t)(p + 0x34) = y;
    *(uint8_t *)(uintptr_t)(p + 0x58) |= 0x10;
}

void rt_hud_corners_tick(uint64_t hud) {
    float f = widescreen_factor();
    bool in = game_in_play() && world_on_screen();
    bool want = in && g_settings.aspect == ASPECT_UNLOCKED && (f > 1.01f || f < 0.99f) && !ENV("LARECOMP_NO_HUD_CORNERS");
    bool hide = in && (g_settings.touch_hide_xy_hud || ENV("LARECOMP_HIDE_XY_HUD")) && touch_visible();
    if (!want && !hide && !g_moved) {
        bool any = false;
        for (unsigned i = 0; i < NPANES; i++) any |= g_hid[i];
        if (!any) { g_active = false, g_left = g_right = 0; return; }
    }
    extern uint64_t renderer_present_count(void);
    extern int kernel_data_regions(uint64_t *a, uint64_t *s, int max);
    g_nr = kernel_data_regions(g_ra, g_rs, 256);
    bool stale = false, missing = false;  // (a pane gone: its memory may be something else now, don't write to it)
    for (unsigned i = 0; i < NPANES; i++) {
        missing |= !g_pane[i];
        stale |= g_pane[i] && !(name_is(g_pane[i], kPanes[i].name) && child_of_hud(g_pane[i]));
    }
    if (hud != g_hud) g_tries = 0;
    if (hud != g_hud || stale || (missing && g_tries < 5)) {  // (a new HUD, each screen load; not all there yet: once a second)
        if (renderer_present_count() < g_retry) return;
        g_hud = hud, g_retry = renderer_present_count() + 60, g_moved = false, g_tries++;
        find_panes(hud);
    }
    float l = 0, r = 0, up = 0;
    if (want && f < 1) up = 360 * (1 / f - 1);  // (a tablet: the band's spare height above it)
    else if (want) {
        float il, ir;
        menu_safe_insets(&il, &ir);  // (the camera cutout: on one side only in landscape)
        float span = 1280 * f;       // layout units across the window
        l = 640 * (f - 1) - il * span, r = 640 * (f - 1) - ir * span;
        if (l < 0) l = 0;
        if (r < 0) r = 0;
    }
    for (unsigned i = 0; i < NPANES; i++) {
        if (!g_pane[i]) continue;
        set_xy(i, g_authored[i] + (kPanes[i].side == LEFT ? -l : kPanes[i].side == RIGHT ? r : 0), g_authored_y[i] + up);
        if (!kPanes[i].xy) continue;
        uint8_t *fl = (uint8_t *)(uintptr_t)(g_pane[i] + 0x58);
        if (hide && (*fl & 1)) *fl &= ~1, g_hid[i] = true;  // (bit 0: visible; the game may set it again)
        else if (!hide && g_hid[i]) *fl |= 1, g_hid[i] = false;
    }
    g_moved = want, g_active = want, g_left = l, g_right = r, g_up = up;
}

// ---- render thread
static uint64_t g_bars_frame = ~0ull, g_crop_frame = ~0ull;
static float g_crop_u0, g_crop_u1, g_crop_v0, g_crop_v1;

bool hud_corners_active(void) { return g_active; }

// A copy of a layout draw's vertex uniforms for the window-wide viewport (stretch k), NULL when this isn't one.
const void *hud_corners_ubo(const float *u, uint64_t size, float k, float ky) {
    if (!g_active || !u || (size != 560 && size != 128) || (k <= 1.001f && ky <= 1.001f)) return NULL;
    if (u[15] != 1 || u[12] != 0 || u[13] != 0 || !(u[0] > 0) || u[5] == 0) return NULL;  // (orthographic projection)
    static float ring[4096][140];
    static unsigned at;
    float *c = ring[at++ & 4095];
    memcpy(c, u, size);  // (560: panes; 128: their text)
    for (int i = 0; i < 4; i++) c[i] /= k, c[4 + i] /= ky;
    return c;
}

void hud_corners_note_crop(uint64_t frame, float u0, float u1, float v0, float v1) {
    g_crop_frame = frame, g_crop_u0 = u0, g_crop_u1 = u1, g_crop_v0 = v0, g_crop_v1 = v1;
}
bool hud_corners_shown_whole(uint64_t frame) { return g_active && g_bars_frame == frame; }

// (the scene composite is drawn) the bars to clear before the HUD, in the target's pixels: false when the last frame had none
bool hud_corners_bars(uint64_t frame, int w, int h, int bar[4]) {
    if (!g_active || g_crop_frame + 2 < frame || g_crop_frame > frame) return false;
    g_bars_frame = frame;
    bar[0] = (int)(w * g_crop_u0), bar[1] = (int)(w * g_crop_u1 + 0.999f);
    bar[2] = (int)(h * g_crop_v0), bar[3] = (int)(h * g_crop_v1 + 0.999f);
    return bar[0] > 0 || bar[1] < w || bar[2] > 0 || bar[3] < h;
}

#else

void rt_hud_corners_tick(uint64_t hud) { (void)hud; }
bool hud_corners_active(void) { return false; }
const void *hud_corners_ubo(const float *u, uint64_t size, float k, float ky) { (void)u, (void)size, (void)k, (void)ky; return NULL; }
void hud_corners_note_crop(uint64_t frame, float u0, float u1, float v0, float v1) { (void)frame, (void)u0, (void)u1, (void)v0, (void)v1; }
bool hud_corners_shown_whole(uint64_t frame) { (void)frame; return false; }
bool hud_corners_bars(uint64_t frame, int w, int h, int bar[4]) { (void)frame, (void)w, (void)h, (void)bar; return false; }

#endif
