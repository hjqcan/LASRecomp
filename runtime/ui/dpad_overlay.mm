// D-pad items' HUD cross (runtime/hle/dpad_items.c): next to the game's X / Y buttons, a D-pad (Kenney's, CC0:
// dpad_art.h) with the item of each direction at the end of its arm, drawn as the game's own item icon (from the
// player's game files), the count on bombs, arrows and powder as on X / Y. Directions without an item stay empty.
// On the Items page it flies (smaller) to the left of the Y button there, clear of the page, and back when
// the page closes; there it shows even with no items (none: it isn't on the HUD, so it doesn't fly - it's just there). Up while the world is on screen and on the Items page
// (where the directions are set).
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>

#include "imgui.h"
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif

#include "../game_addresses.h"
#include "dpad_art.h"

extern "C" {
#include "../hle.h"
#include "../settings.h"
uint8_t *texpatch_ui_rgba(const char *name, int *w, int *h);  // runtime/nvn/texpatch.c
#ifdef __APPLE__
void *ui_metal_device(void);
#else
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
#endif
int dpad_items_slot(int d);
int game_in_play(void);
uint64_t renderer_present_count(void);
bool world_on_screen(void);
bool inventory_items_page_shown(void);
}

// item slot -> its icon (the Items page's: a bottle holding a fairy has its own)
static const char *icon_name(int slot, uint64_t ps) {
    switch (slot) {
    case 2: return "ItemBomb_00^w";
    case 3: return "ItemBow_00^w";
    case 4: return "ItemFookShot_00^w";
    case 5: return "ItemBoomerang_00^w";
    case 6: return "ItemMagicRod_00^w";
    case 7: return "ItemShovel_00^w";
    case 8: return "ItemMagicPowder_00^w";
    case 9: return "ItemRocsFeather_00^w";
    case 12: return "ItemOcarina_03^w";
    case 13: case 14: case 15: return LD8(ps + 0xc0) >> (slot - 13) & 1 ? "ItemBottle_01^w" : "ItemBottle_00^w";
    }
    return NULL;
}

// count shown with the item (-1: none)
static int item_count(int slot, uint64_t ps) {
    return slot == 2 ? LD8(ps + 0x9e) : slot == 3 ? LD8(ps + 0x9f) : slot == 8 ? LD8(ps + 0xa0) : -1;
}

struct Icon { const char *name; ImTextureID id; int state; };  // state 1 ok, -1 not in the game files
static Icon g_icons[24];

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

// the D-pad's two images (white, its alpha from dpad_art.h): 0 the cross, 1 its outline
static ImTextureID dpad_tex(int which) {
    static ImTextureID tex[2];
    static bool tried[2];
    if (!tried[which]) {
        tried[which] = true;
        const uint8_t *rl = which ? k_dpad_rim : k_dpad_fill;
        size_t n = which ? sizeof k_dpad_rim : sizeof k_dpad_fill, at = 0;
        static uint8_t px[128 * 128 * 4];
        for (size_t i = 0; i + 1 < n; i += 2)
            for (int k = 0; k < rl[i] && at < 128 * 128; k++, at++)
                px[at * 4] = px[at * 4 + 1] = px[at * 4 + 2] = 255, px[at * 4 + 3] = rl[i + 1];
        tex[which] = upload(px, 128, 128);
    }
    return tex[which];
}

static const Icon *icon(const char *name) {
    int i = 0;
    for (; i < 24 && g_icons[i].name; i++)
        if (!strcmp(g_icons[i].name, name)) return &g_icons[i];
    if (i == 24) return NULL;
    Icon &ic = g_icons[i];
    ic.name = name, ic.state = -1;
    int w, h;
    if (uint8_t *px = texpatch_ui_rgba(name, &w, &h)) {
        ic.id = upload(px, w, h);
        if (ic.id) ic.state = 1;
        free(px);
    }
    if (ic.state < 0) LOG("[dpad] icon %s not in the game files\n", name);
    return &ic;
}

static bool any_item(void) {
    for (int d = 0; d < 4; d++)
        if (dpad_items_slot(d) >= 0) return true;
    return false;
}

// on the Items page always (where the directions are set), in play once a direction has an item
extern "C" bool dpad_overlay_wanted(void) {
    if (g_settings.dpad_mode != DPAD_ITEMS || !game_in_play()) return false;
    return inventory_items_page_shown() || (world_on_screen() && any_item());
}

// (menu.mm's overlay reuse) what dpad_overlay_draw draws now, as text
static bool g_flying;    // (the D-pad on its way to or from the Items page: a new drawing every frame)
static bool g_page_was;  // (the Items page was up at the last drawing)

extern "C" void dpad_overlay_key(char *out, size_t cap) {
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
    static unsigned frame;  // (on the way, or the Items page opened / closed since the last drawing: draw it again)
    bool page = inventory_items_page_shown();
    int n = g_flying || page != g_page_was ? snprintf(out, cap, "fly %u ", ++frame) : snprintf(out, cap, "%d ", page);
    for (int d = 0; d < 4 && ps; d++) {
        int s = dpad_items_slot(d);
        n += snprintf(out + n, cap > (size_t)n ? cap - n : 0, "%s.%d ", s >= 0 ? icon_name(s, ps) : "-", s >= 0 ? item_count(s, ps) : 0);
    }
}

extern "C" void dpad_overlay_draw(ImVec2 display, ImFont *font, float scale) {
    uint64_t ps = LD64(GA(PLAYER_STATUS_PTR));
    if (!ps) return;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    // the game's HUD sits in a 16:9 box centred in the window; its 1280x720 layout has X / Y top right (Y at 476, 296
    // from the centre, y up; runtime/hle/escape.c). The D-pad goes under X. LARECOMP_DPAD_POS=x,y (layout): elsewhere.
    float bw = fminf(display.x, display.y * 16 / 9), bh = bw * 9 / 16;
    float bx = (display.x - bw) * 0.5f, by = (display.y - bh) * 0.5f, u = bw / 1280;
    static float lx = 1e9f, ly;
    if (lx > 1e8f) {
        lx = 548, ly = 182;
        if (const char *e = getenv("LARECOMP_DPAD_POS")) sscanf(e, "%f,%f", &lx, &ly);
    }
    // the Items page: left of its Y button (the same spot as the HUD's), 60% the size: above the line under the page title.
    // 0.2 s from one to the other, eased.
    static const float px = 388, py = 304, ps_ = 0.6f, fly = 0.2f;
    static double t0 = -1e9;
    static float from, at;  // (0 the HUD's spot, 1 the Items page's)
    bool page = inventory_items_page_shown();
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();  // (ImGui's clock
                                                                                                      // stops while the drawing is reused)
    if (t0 < -1e8 || !any_item()) from = page ? 1 : 0, t0 = now - fly, g_page_was = page;  // (just there)
    else if (page != g_page_was) {
        from = at, t0 = now, g_page_was = page;
        LOG("[dpad] the D-pad flies %s (frame %llu)\n", page ? "to the Items page" : "back to the HUD",
            (unsigned long long)renderer_present_count());
    }
    float k = fminf(1, (float)(now - t0) / fly);
    k = k * k * (3 - 2 * k);
    at = from + ((page ? 1 : 0) - from) * k;
    g_flying = k < 1;
    float cx = lx + (px - lx) * at, cy = ly + (py - ly) * at, sz = 1 + (ps_ - 1) * at;
    ImVec2 c(bx + (cx + 640) * u, by + (360 - cy) * u);
    float pad = 74 * sz * u, arm = 50 * sz * u, h = 30 * sz * u;  // (the D-pad's size; centre to an item; an item's half size)
    static const ImVec2 dir[4] = {ImVec2(-1, 0), ImVec2(0, -1), ImVec2(1, 0), ImVec2(0, 1)};  // Left Up Right Down
    ImVec2 a(c.x - pad * 0.5f, c.y - pad * 0.5f), z(c.x + pad * 0.5f, c.y + pad * 0.5f);
    if (ImTextureID t = dpad_tex(0)) {
        dl->AddImage(t, ImVec2(a.x + 1.5f * u, a.y + 2 * u), ImVec2(z.x + 1.5f * u, z.y + 2 * u), ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(0, 0, 0, 90));  // (a soft shadow)
        dl->AddImage(t, a, z, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(0x2C, 0x34, 0x40, 235));
    }
    if (ImTextureID t = dpad_tex(1)) dl->AddImage(t, a, z, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(0xF4, 0xEE, 0xDC, 240));
    for (int d = 0; d < 4; d++) {
        int s = dpad_items_slot(d);
        const char *nm = s >= 0 ? icon_name(s, ps) : NULL;
        if (!nm) continue;
        ImVec2 p(c.x + dir[d].x * arm, c.y + dir[d].y * arm);
        const Icon *ic = icon(nm);
        if (ic && ic->state == 1) dl->AddImage(ic->id, ImVec2(p.x - h, p.y - h), ImVec2(p.x + h, p.y + h));
        float r = h * 0.8f;
        int n = item_count(s, ps);
        if (n >= 0) {  // (a yellow tag with the number, as the game's)
            char txt[8];
            snprintf(txt, sizeof txt, "%d", n);
            float size = 19 * sz * u;
            ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, txt);
            float w = fmaxf(ts.x + 8 * sz * u, 22 * sz * u), th = 15 * sz * u;
            ImVec2 ta(p.x + r * 0.95f - w * 0.5f, p.y + r * 0.62f), tz(ta.x + w, ta.y + th);
            dl->AddRectFilled(ta, tz, IM_COL32(0xF4, 0xD6, 0x2A, 255), th * 0.5f);
            dl->AddText(font, size, ImVec2(roundf((ta.x + tz.x - ts.x) * 0.5f), roundf((ta.y + tz.y - ts.y) * 0.5f)),
                        IM_COL32(0x3B, 0x2A, 0x14, 255), txt);
        }
    }
}
