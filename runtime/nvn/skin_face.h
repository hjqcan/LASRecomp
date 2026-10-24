// Link's body colour on his face maps (MI_eye_alb.* / MI_mouth_alb.* / MI_ear_alb.*, runtime/nvn/texpatch.c
// skin_apply): one mip level at a time, as RGBA pixels. Pure pixel code (no game state), so tools can run it too.
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    float th;          // target hue (0..1)
    float sf, vf;      // saturation / value factors from the skin's typical colour to the target
    bool grey;         // a grey target: keep the hue
    bool light;        // a very dark target: the face's dark details turn light (skin_face)
} SkinParams;

static inline float skin_sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static inline float skin_smooth(float e0, float e1, float x) { float t = skin_sat((x - e0) / (e1 - e0)); return t * t * (3 - 2 * t); }
static inline uint8_t skin_c8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)v; }

static inline void skin_hsv(const uint8_t *p, float *hue, float *s, float *v) {
    float r = p[0] / 255.0f, g = p[1] / 255.0f, bl = p[2] / 255.0f;
    float mx = fmaxf(r, fmaxf(g, bl)), mn = fminf(r, fminf(g, bl)), d = mx - mn, h = 0;
    if (d > 0) h = mx == r ? fmodf((g - bl) / d + 6, 6) : mx == g ? (bl - r) / d + 2 : (r - g) / d + 4;
    h *= 60;
    *hue = h > 180 ? h - 360 : h;
    *s = mx > 0 ? d / mx : 0;
    *v = mx;
}

// How much of a pixel is skin (0..1): a peach hue (-8..30 degrees), not grey, bright. The body map (tunic, belt,
// boots, arms) has browns close to skin: there it must also be brighter than the leather. In the mouth maps the
// open mouths' tongue (a muted red, green = blue: (148, 65, 66)) is not skin, though its hue and brightness are
// close: skin always has clearly more green than blue (>= 12 in every face map, the blush included).
static inline float skin_weight(const uint8_t *p, bool body, bool mouth) {
    float hue, s, v;
    skin_hsv(p, &hue, &s, &v);
    float m = (1 - skin_smooth(30, 42, hue)) * skin_smooth(-20, -8, hue) * skin_smooth(0.12f, 0.22f, s) *
              (1 - skin_smooth(0.9f, 0.97f, s)) * (body ? skin_smooth(0.55f, 0.68f, v) : skin_smooth(0.42f, 0.58f, v));
    if (mouth) m *= 1 - (1 - skin_smooth(4, 12, (float)p[1] - p[2])) * (1 - skin_smooth(0.68f, 0.78f, v));
    return m;
}

// The skin's colour for a pixel (its own shading kept), blended in by m.
static inline void skin_recolour(uint8_t *p, float m, const SkinParams *k) {
    if (m < 0.004f) return;
    float hue, s, v;
    skin_hsv(p, &hue, &s, &v);
    float nh = k->grey ? hue : k->th * 360 + (hue - 21) * 0.5f, ns = fminf(s * k->sf, 1), nv = fminf(v * k->vf, 1);
    nh = fmodf(nh + 720, 360) / 60;
    int sector = (int)nh % 6;
    float f = nh - floorf(nh), pp = nv * (1 - ns), q = nv * (1 - ns * f), t = nv * (1 - ns * (1 - f));
    float o[6][3] = {{nv, t, pp}, {q, nv, pp}, {pp, nv, t}, {pp, q, nv}, {t, pp, nv}, {nv, pp, q}};
    for (int ch = 0; ch < 3; ch++) p[ch] = skin_c8(p[ch] + (o[sector][ch] * 255 - p[ch]) * m + 0.5f);
}

// A face map's level (w x h RGBA, rows packed), recoloured in place.
// With a very dark body colour (k->light) the dark details would vanish into the skin. Then:
// - lines and small marks (pupils, brows, closed mouths, ear lines) turn light: near white, keeping a little of their
//   own shading (the darkest lightest);
// - an open mouth (mouth maps: a filled shape, red inside or large) keeps its dark-red inside and tongue, and gets a
//   light outline instead, so it still reads as an open mouth (the item-get smile, shouts, gasps).
// Shapes are told apart per connected dark area: its thickness (distance from the skin; the maps' edges don't count,
// as the faces are mirrored at the left edge) and its colour. Sizes scale with the level (the maps are 256 wide).
static inline void skin_face(uint8_t *img, uint32_t w, uint32_t h, bool mouth, const SkinParams *k) {
    uint32_t n = w * h;
    float *m = malloc(n * sizeof(float));
    for (uint32_t i = 0; i < n; i++) m[i] = skin_weight(img + i * 4, false, mouth);
    if (k->light) {
        float sc = w / 256.0f, *d = malloc(n * sizeof(float));
        int32_t *lab = malloc(n * sizeof(int32_t)), *stack = malloc(n * sizeof(int32_t));
        // distance from the skin (chamfer 1 / 1.414, two passes), over the dark areas (skin under half)
        const float big = 1e9f, a = 1, b = 1.41421f;
        for (uint32_t i = 0; i < n; i++) d[i] = m[i] < 0.5f ? big : 0;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t i = y * w + x;
                if (!d[i]) continue;
                float v = d[i];
                if (x) v = fminf(v, d[i - 1] + a);
                if (y) {
                    v = fminf(v, d[i - w] + a);
                    if (x) v = fminf(v, d[i - w - 1] + b);
                    if (x + 1 < w) v = fminf(v, d[i - w + 1] + b);
                }
                d[i] = v;
            }
        for (uint32_t y = h; y-- > 0;)
            for (uint32_t x = w; x-- > 0;) {
                uint32_t i = y * w + x;
                if (!d[i]) continue;
                float v = d[i];
                if (x + 1 < w) v = fminf(v, d[i + 1] + a);
                if (y + 1 < h) {
                    v = fminf(v, d[i + w] + a);
                    if (x + 1 < w) v = fminf(v, d[i + w + 1] + b);
                    if (x) v = fminf(v, d[i + w - 1] + b);
                }
                d[i] = v;
            }
        // connected dark areas (8-neighbours): an open mouth or not
        for (uint32_t i = 0; i < n; i++) lab[i] = -1;
        for (uint32_t i0 = 0; i0 < n; i0++) {
            if (!d[i0] || lab[i0] >= 0) continue;
            uint32_t sp = 0, cnt = 0;
            float maxd = 0, sr = 0, sg = 0;
            stack[sp++] = (int32_t)i0;
            lab[i0] = (int32_t)i0;
            while (sp) {
                uint32_t i = (uint32_t)stack[--sp], x = i % w, y = i / w;
                cnt++;
                maxd = fmaxf(maxd, d[i]);
                sr += img[i * 4];
                sg += img[i * 4 + 1];
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int xx = (int)x + dx, yy = (int)y + dy;
                        if (xx < 0 || yy < 0 || xx >= (int)w || yy >= (int)h) continue;
                        uint32_t j = (uint32_t)yy * w + (uint32_t)xx;
                        if (d[j] && lab[j] < 0) { lab[j] = (int32_t)i0; stack[sp++] = (int32_t)j; }
                    }
            }
            // (the mouth lines: brown, green 0.36 of red, at most 10 px from the skin; open mouths: red inside,
            // green under 0.2 of red, or a large shape)
            bool open = mouth && (maxd >= 15 * sc || (sr > 50.0f * cnt && sg < 0.25f * sr && maxd >= 3 * sc));
            if (open) d[i0] = -d[i0];  // (marked on the area's first pixel, where lab[] points)
        }
        float ow = fmaxf(2.5f * sc, 1);  // (the open mouths' outline, inside the shape)
        for (uint32_t i = 0; i < n; i++) {
            uint8_t *p = img + i * 4;
            float hue, s, v;
            skin_hsv(p, &hue, &s, &v);
            float keep = 0;  // (how much of the pixel's own colour stays: an open mouth's inside)
            if (lab[i] >= 0 && d[lab[i]] < 0) keep = skin_smooth(ow, ow + 1.5f * sc, fabsf(d[i]));
            float fm = (1 - m[i]) * (1 - keep);
            float lv = 0.93f - v * 0.35f;  // (near white; the darkest details lightest)
            uint8_t to[3] = {skin_c8(lv * 255), skin_c8(lv * 252), skin_c8(lv * 246)};  // (a touch warm)
            uint8_t orig[4] = {p[0], p[1], p[2], p[3]};
            skin_recolour(orig, 1, k);  // (the skin's colour here: blended in by m below)
            for (int ch = 0; ch < 3; ch++)
                p[ch] = skin_c8(p[ch] * keep * (1 - m[i]) + to[ch] * fm + orig[ch] * m[i] + 0.5f);
        }
        free(d); free(lab); free(stack);
    } else {
        for (uint32_t i = 0; i < n; i++) skin_recolour(img + i * 4, m[i], k);
    }
    free(m);
}
