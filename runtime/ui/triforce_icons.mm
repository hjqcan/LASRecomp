// Triforce hunt counter icon for our overlays (Items page, tracker): made at runtime from the player's own game files
// (nothing of the game ships). The map-memo triangle (MapMark_05^w) recoloured gold is the building block: three copies
// of its gold body placed tip to corner (one on top, two below, the hollow upside-down triangle in the middle), then one
// dark outline and a drop shadow around the whole shape. Each display size gets its own texture, area-averaged from the
// full-size image, so the edges and the hollow stay clean small too.
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>

#include "imgui.h"
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif

extern "C" {
uint8_t *texpatch_ui_rgba(const char *name, int *w, int *h);  // runtime/nvn/texpatch.c
#ifdef __APPLE__
void *ui_metal_device(void);
#else
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
#endif
}

static ImTextureID upload(const std::vector<uint8_t> &rgba, int w, int h) {
#ifdef __APPLE__
    id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
    if (!dev) return (ImTextureID)0;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:w * 4];
    return (ImTextureID)(__bridge_retained void *)t;  // (kept for the session)
#else
    return (ImTextureID)vk_imgui_texture_rgba(rgba.data(), w, h);
#endif
}

// RGBA float image (straight alpha)
struct Img {
    int w = 0, h = 0;
    std::vector<float> p;
    float *at(int x, int y) { return &p[((size_t)y * w + x) * 4]; }
};

// The triangle's gold body (its bright pixels; the mark's own dark rim and shadow left out), cropped to the body,
// recoloured: a gold gradient (top bright) at the body's own brightness
static bool gold_body(Img &out) {
    int w, h;
    uint8_t *src = texpatch_ui_rgba("MapMark_05^w", &w, &h);
    if (!src) return false;
    auto body_a = [&](int x, int y) {  // soft mask: bright and opaque
        const uint8_t *p = src + ((size_t)y * w + x) * 4;
        float v = fmaxf(p[0], fmaxf(p[1], p[2])) / 255.0f, a = p[3] / 255.0f;
        return fminf(1.0f, fmaxf(0.0f, (v - 0.35f) / 0.25f)) * a;
    };
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (body_a(x, y) > 0.5f) x0 = x < x0 ? x : x0, y0 = y < y0 ? y : y0, x1 = x > x1 ? x : x1, y1 = y > y1 ? y : y1;
    if (x1 < 0) { free(src); return false; }
    out.w = x1 - x0 + 1, out.h = y1 - y0 + 1;
    out.p.assign((size_t)out.w * out.h * 4, 0);
    for (int y = 0; y < out.h; y++) {
        float g = 1.0f - 0.28f * y / (float)(out.h > 1 ? out.h - 1 : 1);
        for (int x = 0; x < out.w; x++) {
            const uint8_t *p = src + ((size_t)(y + y0) * w + x + x0) * 4;
            float v = fminf(1.0f, fmaxf(p[0], fmaxf(p[1], p[2])) / 255.0f * 1.05f), *q = out.at(x, y);
            q[0] = 0.98f * g * v, q[1] = 0.80f * g * v, q[2] = 0.25f * g * v, q[3] = body_a(x + x0, y + y0);
        }
    }
    free(src);
    return true;
}

// The Triforce at full size: the three bodies, a dark rim (R px) round the whole shape and a drop shadow below
static bool compose(Img &t) {
    Img b;
    if (!gold_body(b)) return false;
    const int R = 2, SH = 3, pad = R + 1;
    int W = b.w, H = b.h;
    t.w = 2 * W + 2 * pad, t.h = 2 * H + 2 * pad + SH;
    std::vector<float> rgb((size_t)t.w * t.h * 3, 0), a((size_t)t.w * t.h, 0);
    auto put = [&](int ox, int oy) {  // (the bodies only touch at points: keep the stronger pixel)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float *s = b.at(x, y);
                size_t i = (size_t)(y + oy) * t.w + x + ox;
                if (s[3] > a[i]) { a[i] = s[3]; for (int k = 0; k < 3; k++) rgb[i * 3 + k] = s[k]; }
            }
    };
    put(pad + W / 2, pad);
    put(pad, pad + H);
    put(pad + W, pad + H);
    t.p.assign((size_t)t.w * t.h * 4, 0);
    const float rim[3] = {0.16f, 0.11f, 0.03f};
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.w; x++) {
            size_t i = (size_t)y * t.w + x;
            float near = 0, shadow = 0;  // rim: the body within R px; shadow: the shape SH px above
            for (int dy = -R; dy <= R; dy++)
                for (int dx = -R; dx <= R; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= t.w || yy >= t.h || dx * dx + dy * dy > R * R + 1) continue;
                    near = fmaxf(near, a[(size_t)yy * t.w + xx]);
                    if (yy - SH >= 0) shadow = fmaxf(shadow, a[(size_t)(yy - SH) * t.w + xx]);
                }
            float ba = a[i], ra = near * (1 - ba), sa = shadow * 0.45f;
            float *q = t.at(x, y);
            float oa = ba + ra * (1 - ba) + sa * (1 - ba) * (1 - ra);  // body over rim over shadow
            for (int k = 0; k < 3; k++)
                q[k] = oa > 0 ? (rgb[i * 3 + k] * ba + rim[k] * ra * (1 - ba)) / oa : 0;
            q[3] = oa;
        }
    return true;
}

// area-averaged (premultiplied) to fit size x size, as RGBA8
static std::vector<uint8_t> resample(Img &s, int size, int *ow, int *oh) {
    float k = (float)size / (float)(s.w > s.h ? s.w : s.h);
    int w = (int)lroundf(s.w * k), h = (int)lroundf(s.h * k);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    std::vector<uint8_t> out((size_t)w * h * 4, 0);
    float sx = (float)s.w / w, sy = (float)s.h / h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc[4] = {0, 0, 0, 0}, wt = 0;
            float fx0 = x * sx, fx1 = (x + 1) * sx, fy0 = y * sy, fy1 = (y + 1) * sy;
            for (int yy = (int)fy0; yy < (int)ceilf(fy1) && yy < s.h; yy++)
                for (int xx = (int)fx0; xx < (int)ceilf(fx1) && xx < s.w; xx++) {
                    float cw = fminf(fx1, xx + 1.0f) - fmaxf(fx0, (float)xx), ch = fminf(fy1, yy + 1.0f) - fmaxf(fy0, (float)yy);
                    float c = cw * ch, *p = s.at(xx, yy);
                    for (int q = 0; q < 3; q++) acc[q] += p[q] * p[3] * c;
                    acc[3] += p[3] * c;
                    wt += c;
                }
            uint8_t *d = &out[((size_t)y * w + x) * 4];
            float al = wt > 0 ? acc[3] / wt : 0;
            for (int q = 0; q < 3; q++) d[q] = (uint8_t)lroundf(255 * fminf(1, acc[3] > 0 ? acc[q] / acc[3] : 0));
            d[3] = (uint8_t)lroundf(255 * fminf(1, al));
        }
    *ow = w, *oh = h;
    return out;
}

// LARECOMP_TRIFORCE_ICON_DUMP=<prefix> (test): each texture as made, raw RGBA8 (<prefix>_<w>x<h>.rgba)
static void dump(const std::vector<uint8_t> &rgba, int w, int h) {
    const char *pre = getenv("LARECOMP_TRIFORCE_ICON_DUMP");
    if (!pre) return;
    char p[1024];
    snprintf(p, sizeof p, "%s_%dx%d.rgba", pre, w, h);
    if (FILE *f = fopen(p, "wb")) { fwrite(rgba.data(), 1, rgba.size(), f); fclose(f); }
}

struct Tex { ImTextureID id; int w, h; };

// Draws the Triforce fitted in a box of `size` pixels centred at c; false when the game files don't have its triangle
// (the caller draws its own then)
bool triforce_icon_draw(ImDrawList *dl, ImVec2 c, float size, ImU32 tint) {
    static Img full;
    static int state;  // 0 not made, 1 made, -1 not available
    if (!state) state = compose(full) ? 1 : -1;
    if (state < 0) return false;
    static std::map<int, Tex> cache;  // (one texture per display size)
    int px = (int)lroundf(size);
    if (px < 4) px = 4;
    auto it = cache.find(px);
    if (it == cache.end()) {
        int w, h;
        std::vector<uint8_t> r = resample(full, px, &w, &h);
        dump(r, w, h);
        it = cache.emplace(px, Tex{upload(r, w, h), w, h}).first;
    }
    const Tex &t = it->second;
    if (!t.id) return false;
    ImVec2 a(floorf(c.x - t.w * 0.5f), floorf(c.y - t.h * 0.5f));  // (whole pixels: the texture drawn 1:1)
    dl->AddImage(t.id, a, ImVec2(a.x + t.w, a.y + t.h), ImVec2(0, 0), ImVec2(1, 1), tint);
    return true;
}
