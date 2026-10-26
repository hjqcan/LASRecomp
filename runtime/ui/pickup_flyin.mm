// Silent pickups' fly-in (prototype; runtime/hle/silent_pickups.c): a heart piece, Heart Container or Secret Seashell
// picked up from the ground comes in without the item-get cutscene, and a small card slides in from the left edge for ~2 s instead, below the HUD's
// hearts (the HUD sits in a 16:9 box centred in the window: the card lines up with its left edge, so ultrawide
// windows show it beside the hearts too). Drawn by menu.mm under the menu, in the tracker / timer's parchment style
// with the game's own icons (texpatch_ui_rgba_list, from the player's game files; nothing of the game ships):
//   Piece of Heart     the heart filling in quarters, "2 / 4"   (4 / 4: "+1 Heart")
//   Heart Container    a full heart, "+1 Heart"
//   Secret Seashell    the seashell, "23 / 50" (the seed's own total)
// (rupees and recovery hearts: no card - the HUD shows them)
#include <chrono>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif

extern "C" {
int game_in_play(void);
bool silent_pickup_next(int *item, int *count, int *held);  // runtime/hle/silent_pickups.c
int silent_seashell_total(void);
bool inventory_items_page_shown(void);  // inventory_overlay.mm
int texpatch_ui_rgba_list(const char *const *names, int n, uint8_t **out, int *w, int *h);  // runtime/nvn/texpatch.c
#ifdef __APPLE__
void *ui_metal_device(void);
#else
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
#endif
}

enum { IT_SEASHELL = 22, IT_HEART_PIECE = 58, IT_HEART_CONTAINER = 59 };

// ---- icons (the game's own UI textures, made once): the HUD's full / empty heart, the status screen's Heart Container,
// the Secret Seashell
enum { IC_HEART, IC_HEART_EMPTY, IC_CONTAINER, IC_SHELL, IC_N };
static const char *const kIconTex[IC_N] = {"Hp_01^w", "Hp_00^w", "StHeartContainer_01^q", "CollectSecretSeashell_00^w"};
struct Icon { ImTextureID id; int w, h; ImVec2 uv0, uv1; };  // uv: the drawn part (transparent margins cut)
static Icon g_icon[IC_N];
static int g_icons_state;  // 0 not loaded, 1 loaded (one missing: drawn shapes / left out)

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

static void make_icon(Icon &ic, const uint8_t *px, int w, int h, bool crop) {
    int x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (crop) {  // (the opaque part: some icons sit small in a big canvas)
        x0 = w, y0 = h, x1 = 0, y1 = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (px[(y * w + x) * 4 + 3] > 24) x0 = x < x0 ? x : x0, y0 = y < y0 ? y : y0, x1 = x + 1 > x1 ? x + 1 : x1, y1 = y + 1 > y1 ? y + 1 : y1;
        if (x1 <= x0 || y1 <= y0) x0 = y0 = 0, x1 = w, y1 = h;
    }
    ic.id = upload(px, w, h);
    ic.w = x1 - x0, ic.h = y1 - y0;
    ic.uv0 = ImVec2((float)x0 / w, (float)y0 / h), ic.uv1 = ImVec2((float)x1 / w, (float)y1 / h);
}

static void load_icons(void) {
    if (g_icons_state) return;
    g_icons_state = 1;
    uint8_t *px[IC_N];
    int w[IC_N], h[IC_N];
    texpatch_ui_rgba_list(kIconTex, IC_N, px, w, h);
    for (int i = 0; i < IC_N; i++) {
        if (!px[i]) continue;
        make_icon(g_icon[i], px[i], w[i], h[i], i >= IC_CONTAINER);
        free(px[i]);
    }
}

// ---- the card on screen (and what comes next)
struct Card { int item, amount, held; double t0; bool on; };  // amount: containers; held: pieces / shells
static Card g_card;
static const double kIn = 0.22, kHold = 1.75, kOut = 0.25;  // seconds: ~2.2 s on screen

static double now_s(void) {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static Card g_wait[8];  // cards waiting for the one on screen
static int g_nwait;

static void take_next(double t) {
    int item, count, held;
    while (silent_pickup_next(&item, &count, &held)) {
        (void)count;
        if (item != IT_HEART_PIECE && item != IT_HEART_CONTAINER && item != IT_SEASHELL) continue;
        // the same kind again while its card is still up (or waiting): that card counts on - heart pieces / seashells
        // show the new count, containers "+2 Hearts" - and one on screen stays up a bit longer
        Card *last = g_nwait ? &g_wait[g_nwait - 1] : g_card.on ? &g_card : NULL;
        if (last && last->item == item && (last != &g_card || t - g_card.t0 < kIn + kHold)) {
            if (item == IT_HEART_CONTAINER) last->amount++;
            else last->held = held;
            if (last == &g_card) g_card.t0 = fmax(g_card.t0, t - kIn);
            continue;
        }
        Card c = {item, 1, held, t, true};
        if (!g_card.on) g_card = c;
        else if (g_nwait < 8) g_wait[g_nwait++] = c;
    }
    if (g_card.on && t - g_card.t0 > kIn + kHold + kOut) {
        g_card.on = false;
        if (g_nwait) {
            g_card = g_wait[0];
            g_card.t0 = t;
            memmove(g_wait, g_wait + 1, sizeof(Card) * (size_t)--g_nwait);
        }
    }
}

extern "C" bool pickup_flyin_wanted(void) {
    take_next(now_s());
    return g_card.on && game_in_play();
}

// the parchment panel (tracker_overlay.mm / race_overlay.mm style)
static void panel(ImDrawList *dl, ImVec2 a, ImVec2 z, float u) {
    float r = 10 * u;
    dl->AddRectFilled(ImVec2(a.x + 2 * u, a.y + 3 * u), ImVec2(z.x + 2 * u, z.y + 3 * u), IM_COL32(0, 0, 0, 45), r);
    dl->AddRectFilled(ImVec2(a.x - 2 * u, a.y - 2 * u), ImVec2(z.x + 2 * u, z.y + 2 * u), IM_COL32(0x4A, 0x30, 0x20, 170), r + 2 * u);
    dl->AddRectFilled(a, z, IM_COL32(0xF3, 0xE6, 0xC6, 200), r);
    dl->AddRect(a, z, IM_COL32(0xC8, 0x9B, 0x3C, 220), r, 0, 2.0f * u);
}

// a heart (the HUD's own full / empty heart icons; drawn circles without them): `quarters` 0-4 filled, the rest the
// HUD's empty heart; quarters fill top left, top right, bottom right, bottom left
static void heart(ImDrawList *dl, ImVec2 p, float s, int quarters) {
    const Icon &full = g_icon[IC_HEART], &empty = g_icon[IC_HEART_EMPTY];
    ImVec2 c(p.x + s * 0.5f, p.y + s * 0.5f);
    ImVec2 q0[4] = {p, ImVec2(c.x, p.y), c, ImVec2(p.x, c.y)};
    for (int i = 0; i < 4; i++) {
        bool on = i < quarters;
        dl->PushClipRect(q0[i], ImVec2(q0[i].x + s * 0.5f, q0[i].y + s * 0.5f), true);
        const Icon &ic = on ? full : empty;
        if (ic.id) {
            dl->AddImage(ic.id, p, ImVec2(p.x + s, p.y + s), ic.uv0, ic.uv1);
        } else {  // (two circles and a point)
            ImU32 fill = on ? IM_COL32(0xF0, 0x3A, 0x32, 255) : IM_COL32(0x5A, 0x14, 0x10, 255);
            float r = s * 0.26f;
            dl->AddCircleFilled(ImVec2(p.x + s * 0.29f, p.y + s * 0.36f), r, fill);
            dl->AddCircleFilled(ImVec2(p.x + s * 0.71f, p.y + s * 0.36f), r, fill);
            dl->AddTriangleFilled(ImVec2(p.x + s * 0.06f, p.y + s * 0.45f), ImVec2(p.x + s * 0.94f, p.y + s * 0.45f),
                                  ImVec2(p.x + s * 0.5f, p.y + s * 0.92f), fill);
        }
        dl->PopClipRect();
    }
}

static void image(ImDrawList *dl, int which, ImVec2 p, float s, ImU32 tint) {
    const Icon &ic = g_icon[which];
    if (!ic.id) return;
    float k = s / (float)(ic.w > ic.h ? ic.w : ic.h);
    float iw = ic.w * k, ih = ic.h * k;
    ImVec2 q(p.x + (s - iw) * 0.5f, p.y + (s - ih) * 0.5f);
    dl->AddImage(ic.id, q, ImVec2(q.x + iw, q.y + ih), ic.uv0, ic.uv1, tint);
}

extern "C" void pickup_flyin_draw(ImVec2 display, ImFont *font, float scale) {
    if (!g_card.on || inventory_items_page_shown()) return;
    load_icons();
    double t = now_s() - g_card.t0;
    // slide: in from the left edge (ease out), hold, back out (ease in)
    float k = t < kIn ? (float)(t / kIn) : t < kIn + kHold ? 1.0f : 1.0f - (float)((t - kIn - kHold) / kOut);
    k = fmaxf(0, fminf(1, k));
    float e = t < kIn ? 1 - (1 - k) * (1 - k) * (1 - k) : k * k * (3 - 2 * k);

    char title[64], value[48];
    int it = g_card.item;
    int quarters = 4;
    if (it == IT_HEART_PIECE) {
        int n = g_card.held % 4;
        quarters = n ? n : 4;
        snprintf(title, sizeof title, "Piece of Heart");
        if (n) snprintf(value, sizeof value, "%d / 4", n);
        else snprintf(value, sizeof value, "4 / 4   +1 Heart");
    } else if (it == IT_HEART_CONTAINER) {
        snprintf(title, sizeof title, g_card.amount > 1 ? "Heart Containers" : "Heart Container");
        if (g_card.amount > 1) snprintf(value, sizeof value, "+%d Hearts", g_card.amount);
        else snprintf(value, sizeof value, "+1 Heart");
    } else if (it == IT_SEASHELL) {
        snprintf(title, sizeof title, "Secret Seashells");
        snprintf(value, sizeof value, "%d / %d", g_card.held, silent_seashell_total());
    } else {
        return;
    }

    float icon = 64 * scale, pad = 14 * scale, gap = 14 * scale;
    float ts = 28 * scale, vs = 42 * scale;
    // Baloo's line box is tall: lines placed by their cap height (~0.5 of the size, from ~0.25 down)
    float tw = title[0] ? font->CalcTextSizeA(ts, FLT_MAX, 0, title).x : 0, vw = font->CalcTextSizeA(vs, FLT_MAX, 0, value).x;
    float textw = fmaxf(tw, vw);
    float texth = (title[0] ? ts * 0.62f + 6 * scale : 0) + vs * 0.62f;
    float h = fmaxf(icon, texth) + 2 * pad, w = pad + icon + gap + textw + pad * 1.4f;
    // the HUD's 16:9 box: the card's resting place is its left edge, below the hearts (3 rows at 20 hearts)
    float bw = fminf(display.x, display.y * 16 / 9), bx = (display.x - bw) * 0.5f;
    float rest = bx + 28 * scale, x = -w - 8 * scale + (rest + w + 8 * scale) * e;
    float y = display.y * 0.215f;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    ImVec2 a(roundf(x), roundf(y)), z(roundf(x + w), roundf(y + h));
    panel(dl, a, z, scale);
    ImVec2 ip(a.x + pad, a.y + (h - icon) * 0.5f);
    if (it == IT_HEART_PIECE) heart(dl, ip, icon, quarters);
    else if (it == IT_HEART_CONTAINER && g_icon[IC_CONTAINER].id) image(dl, IC_CONTAINER, ip, icon, IM_COL32_WHITE);
    else if (it == IT_HEART_CONTAINER) heart(dl, ip, icon, 4);
    else image(dl, IC_SHELL, ip, icon, IM_COL32_WHITE);
    ImU32 ink = IM_COL32(0x3B, 0x2A, 0x14, 255), sub = IM_COL32(0x6A, 0x4E, 0x2C, 255);
    float tx = a.x + pad + icon + gap, ty = a.y + (h - texth) * 0.5f;
    if (title[0]) {
        dl->AddText(font, ts, ImVec2(roundf(tx), roundf(ty - ts * 0.22f)), sub, title);
        ty += ts * 0.62f + 6 * scale;
    }
    dl->AddText(font, vs, ImVec2(roundf(tx), roundf(ty - vs * 0.22f + 1.2f * scale)), IM_COL32(255, 248, 228, 150), value);
    dl->AddText(font, vs, ImVec2(roundf(tx), roundf(ty - vs * 0.22f)), ink, value);
}
