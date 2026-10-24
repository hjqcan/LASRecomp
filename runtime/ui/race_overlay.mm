// Speedrun overlays, drawn by menu.mm under the menu:
//   the run timer (Gameplay tab "Timer"; runtime/hle/run_timer.c) in the bottom-left corner while Link is in play - the
//   game's HUD keeps the top (hearts, item buttons) and the bottom right (rupees) - on a small see-through parchment
//   panel like the tracker's, gold once the run is finished
//   the seed hash of a race seed (Randomizer tab "Race mode"; seed_hash.h) on the title / file select screen: five of
//   the game's own item icons in the bottom-right corner, so racers can check they have the same seed
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "imgui.h"
#include "seed_hash.h"
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif

extern "C" {
#include "../rando.h"
#include "../settings.h"
int game_in_play(void);
extern int g_start_screen;
int run_timer_state(uint64_t *us, bool *counting);  // runtime/hle/run_timer.c
bool inventory_items_page_shown(void);              // inventory_overlay.mm
int texpatch_ui_rgba_list(const char *const *names, int n, uint8_t **out, int *w, int *h);  // runtime/nvn/texpatch.c
uint64_t renderer_present_count(void);
#ifdef __APPLE__
void *ui_metal_device(void);
#else
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
#endif
}
bool rando_seed_race(const char *seed, char *id, size_t idcap);  // rando_tab.mm: a race seed (and its ID)

// ---- the active seed's hash (a race seed only), looked up again when the seed changes
static std::string g_hash_seed;
static bool g_hash_on;
static int g_hash[5];

static bool hash_wanted(void) {
    if (g_start_screen || game_in_play() || renderer_present_count() < 30) return false;  // (the title / file select)
    const char *dir = rando_seed_dir();
    if (!dir) return false;
    if (g_hash_seed != dir) {
        g_hash_seed = dir;
        char id[64] = "";
        g_hash_on = rando_seed_race(dir + 6, id, sizeof id) && id[0];  // ("rando/<seed>")
        if (g_hash_on) seed_hash(id, g_hash);
    }
    return g_hash_on;
}

extern "C" bool race_overlay_wanted(void) { return (g_settings.timer && game_in_play()) || hash_wanted(); }

// ---- the icons (made once each from the player's game files; kept for the session)
struct Icon { ImTextureID id; int w, h; int state; };  // state 0 not loaded, 1 ok, -1 not in the game files
static Icon g_icons[32];

static ImTextureID upload(const uint8_t *rgba, int w, int h) {
#ifdef __APPLE__
    id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
    if (!dev) return (ImTextureID)0;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba bytesPerRow:w * 4];
    return (ImTextureID)(__bridge_retained void *)t;
#else
    return (ImTextureID)vk_imgui_texture_rgba(rgba, w, h);
#endif
}

static void load_icons(const int *idx, int n) {
    const char *names[5];
    int want[5], k = 0;
    for (int i = 0; i < n; i++)
        if (!g_icons[idx[i]].state) {
            bool dup = false;
            for (int j = 0; j < k; j++) dup |= want[j] == idx[i];
            if (!dup) want[k] = idx[i], names[k++] = kSeedHashIcons[idx[i]].tex;
        }
    if (!k) return;
    uint8_t *px[5];
    int w[5], h[5];
    texpatch_ui_rgba_list(names, k, px, w, h);
    for (int i = 0; i < k; i++) {
        Icon &ic = g_icons[want[i]];
        ic.state = -1;
        if (!px[i]) continue;
        ic.id = upload(px[i], w[i], h[i]);
        ic.w = w[i], ic.h = h[i];
        if (ic.id) ic.state = 1;
        free(px[i]);
    }
}

// The timer's panel: the tracker's game-styled parchment (runtime/ui/tracker_overlay.mm: dark brown frame, parchment,
// gold rim, brown ink), see-through so the game shows under it
static void timer_panel(ImDrawList *dl, ImVec2 a, ImVec2 z, float u, bool done) {
    float r = 10 * u;
    dl->AddRectFilled(ImVec2(a.x + 2 * u, a.y + 3 * u), ImVec2(z.x + 2 * u, z.y + 3 * u), IM_COL32(0, 0, 0, 45), r);
    dl->AddRectFilled(ImVec2(a.x - 2 * u, a.y - 2 * u), ImVec2(z.x + 2 * u, z.y + 2 * u), IM_COL32(0x4A, 0x30, 0x20, 150), r + 2 * u);
    dl->AddRectFilled(a, z, done ? IM_COL32(0xF6, 0xE2, 0xA8, 165) : IM_COL32(0xF3, 0xE6, 0xC6, 140), r);
    dl->AddRect(a, z, done ? IM_COL32(0xE8, 0xB8, 0x4A, 255) : IM_COL32(0xC8, 0x9B, 0x3C, 200), r, 0, (done ? 3.0f : 2.0f) * u);
}

// The run timer's text (tenths) and state (2: finished); false when it isn't shown
static bool timer_text(char *t, size_t cap, int *st) {
    if (!(g_settings.timer && game_in_play() && !inventory_items_page_shown())) return false;  // (not over the Items page: it still counts)
    uint64_t us = 0;
    bool counting = false;
    *st = run_timer_state(&us, &counting);
    uint64_t ds = us / 100000;  // tenths
    if (ds >= 36000) snprintf(t, cap, "%llu:%02llu:%02llu.%llu", (unsigned long long)(ds / 36000),
                              (unsigned long long)(ds / 600 % 60), (unsigned long long)(ds / 10 % 60), (unsigned long long)(ds % 10));
    else snprintf(t, cap, "%llu:%02llu.%llu", (unsigned long long)(ds / 600), (unsigned long long)(ds / 10 % 60),
                  (unsigned long long)(ds % 10));
    return true;
}

// (menu.mm's overlay reuse) what race_overlay_draw would draw now, as text: the same key = the same drawing at the
// same display size. false: not reusable (the seed hash: its icons load over the first frames)
extern "C" bool race_overlay_key(char *out, size_t cap) {
    if (hash_wanted()) return false;
    char t[32];
    int st = 0;
    if (timer_text(t, sizeof t, &st)) snprintf(out, cap, "timer %s %d %d", t, st == 2, (int)g_settings.timer_pos);
    else snprintf(out, cap, "-");
    return true;
}

extern "C" void race_overlay_draw(ImVec2 display, ImFont *font, float scale) {
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    char t[32];
    int st = 0;
    if (timer_text(t, sizeof t, &st)) {
        float size = 62 * scale;
        // (digits at a fixed width: the panel and the time don't wobble as it counts)
        float dw = font->CalcTextSizeA(size, FLT_MAX, 0, "0").x, tw = 0;
        for (const char *c = t; *c; c++) {
            char one[2] = {*c, 0};
            tw += *c >= '0' && *c <= '9' ? dw : font->CalcTextSizeA(size, FLT_MAX, 0, one).x;
        }
        // Baloo's line box is tall: the panel hugs the digits (cap height ~ 0.5 of the size, from ~0.3 down)
        float padx = 20 * scale, pady = 10 * scale, top = size * 0.24f, bot = size * 0.80f;
        float pw = tw + 2 * padx, ph = (bot - top) + 2 * pady;
        // bottom left, or bottom right. The game's HUD sits in a 16:9 box centred in the window (wider windows: the
        // corners are free): hearts top left, item buttons top right, and bottom right the "Now saving." note (about
        // 0.79-1.0 of the box's width, 0.86-0.94 of its height; found 2026-10-20) - at the right, the panel goes above
        // that note when they'd meet.
        float ax = g_settings.timer_pos == TIMER_POS_RIGHT ? display.x - 30 * scale - pw : 30 * scale;
        ImVec2 a(ax, display.y - 30 * scale - ph), z(a.x + pw, a.y + ph);
        if (g_settings.timer_pos == TIMER_POS_RIGHT) {
            float bw = fminf(display.x, display.y * 16 / 9), bx = (display.x - bw) * 0.5f;
            float sx0 = bx + 0.79f * bw, sx1 = bx + bw, sy0 = 0.86f * display.y, sy1 = 0.94f * display.y;
            if (z.x > sx0 && a.x < sx1 && z.y > sy0 && a.y < sy1) a.y = sy0 - 10 * scale - ph, z.y = a.y + ph;
        }
        timer_panel(dl, a, z, scale, st == 2);
        ImU32 ink = st == 2 ? IM_COL32(0x8A, 0x55, 0x00, 255) : IM_COL32(0x3B, 0x2A, 0x14, 255);
        float x = a.x + padx, y = a.y + pady - top;
        for (const char *c = t; *c; c++) {
            char one[2] = {*c, 0};
            float gw = font->CalcTextSizeA(size, FLT_MAX, 0, one).x, cw = *c >= '0' && *c <= '9' ? dw : gw;
            ImVec2 p(roundf(x + (cw - gw) * 0.5f), roundf(y));
            dl->AddText(font, size, ImVec2(p.x, p.y + 1.2f * scale), IM_COL32(255, 248, 228, 150), one);  // (a light emboss)
            dl->AddText(font, size, p, ink, one);
            x += cw;
        }
    }
    if (hash_wanted()) {  // a column at the right edge (beside the file select's boxes): "SEED HASH", the five icons
        load_icons(g_hash, 5);  // with their names
        float icon = 64 * scale, row = 76 * scale, pad = 18 * scale, label = 30 * scale, name = 34 * scale, gap = 14 * scale;
        float namew = 0;
        for (int i = 0; i < 5; i++) namew = fmaxf(namew, font->CalcTextSizeA(name, FLT_MAX, 0, kSeedHashIcons[g_hash[i]].name).x);
        float w = fmaxf(pad + icon + gap + namew + pad, pad + font->CalcTextSizeA(label, FLT_MAX, 0, "SEED HASH").x + pad);
        float h = pad + label + 8 * scale + 5 * row + pad * 0.5f;
        ImVec2 a(display.x - 24 * scale - w, (display.y - h) * 0.5f), b(a.x + w, a.y + h);
        dl->AddRectFilled(a, b, IM_COL32(12, 20, 16, 205), 14 * scale);
        dl->AddRect(a, b, IM_COL32(236, 194, 84, 200), 14 * scale, 0, fmaxf(1.0f, 2 * scale));
        dl->AddText(font, label, ImVec2(a.x + pad, a.y + pad * 0.6f), IM_COL32(236, 194, 84, 255), "SEED HASH");
        for (int i = 0; i < 5; i++) {
            const Icon &ic = g_icons[g_hash[i]];
            const char *nm = kSeedHashIcons[g_hash[i]].name;
            ImVec2 p(a.x + pad, a.y + pad + label + 8 * scale + i * row + (row - icon) * 0.5f);
            if (ic.state == 1) {
                float k = icon / (float)(ic.w > ic.h ? ic.w : ic.h);
                float iw = ic.w * k, ih = ic.h * k;
                ImVec2 q(p.x + (icon - iw) * 0.5f, p.y + (icon - ih) * 0.5f);
                dl->AddImage(ic.id, q, ImVec2(q.x + iw, q.y + ih));
            }  // (the name too: to say it out loud, and the only mark when the icon isn't in the game files)
            ImVec2 ts = font->CalcTextSizeA(name, FLT_MAX, 0, nm);
            dl->AddText(font, name, ImVec2(roundf(p.x + icon + gap), roundf(p.y + (icon - ts.y) * 0.5f)),
                        IM_COL32(240, 236, 224, 255), nm);
        }
    }
}
