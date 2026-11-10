// The settings menus' plastic pieces (plastic.h).
//
// Each piece is an ImGui draw callback in the draw list where it belongs, so it lands under the text drawn after it
// and over what came before (panel, then the wells, then a row, then the row's text). The callback draws one quad with
// our own fragment shader: a rounded-box distance gives the shape, a bevel across its edge gives a normal, and that
// normal is lit (a key light from the top left: diffuse, a rim, a clear-coat sheen, a soft highlight leaning toward
// the mouse); outside the shape a soft drop shadow. Over a dark scene the light pieces come down a little, all alike
// (the glare guard): the game image is first shrunk to a small blurred copy (64 x 32), whose edges give how bright
// the game around the menu is. The same shader runs on Metal (MSL) and Vulkan (GLSL, plastic_vk.c): one body, two
// small wrappers.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "i18n.h"
#include "imgui_internal.h"
#include "plastic.h"

static ImVec4 hex(unsigned rgb, float a = 1) {
    return ImVec4((rgb >> 16 & 255) / 255.0f, (rgb >> 8 & 255) / 255.0f, (rgb & 255) / 255.0f, a);
}
static ImU32 hex32(unsigned rgb, unsigned a = 255) { return IM_COL32(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, a); }

const PlasticTheme &plastic_theme(void) {
    static PlasticTheme t;
    static bool init;
    if (!init) {
        init = true;
        // the item screen's colours (pause menu): olive-brown paper in patches (the panel), in an olive frame with a
        // khaki-gold inner edge; rows, buttons and tabs terracotta in a khaki-gold rim (cream text), the selection pale
        // gold in a lighter gold rim (dark brown text), Delete / warnings a muted red. The terracotta is a little
        // darker than the item screen's (#A57955) so cream text keeps 4.5:1.
        t.panel = hex(0x433110, 0.78f);   // (the patches: panel <-> panel2, about #493812 on average)
        t.panel2 = hex(0x4F4017, 0.78f);
        t.inset = hex(0x3D3213, 0.72f);
        t.frame = hex(0x4F5231);
        t.frame_inner = hex(0xAA9B67);
        t.row = hex(0x836044);
        t.row_sel = hex(0xE5C589);
        t.row_mark = hex(0x6E5220);  // (the category shown beside its list, the cursor elsewhere; a tab under the mouse)
        t.row_hot = hex(0xE5C589);   // (ImGui's own buttons under the cursor: as the selection, dark text)
        t.button = hex(0x836044);
        t.danger = hex(0x8A3A2C);
        t.tab = hex(0x75563B);
        t.tab_sel = hex(0xE5C589);
        t.ring = hex(0xAA9B67);
        t.ring_sel = hex(0xECD78E);
        t.text = hex32(0xFFF3DC);
        t.text_dim = hex32(0xC9B691);
        t.text_sel = hex32(0x3A2410);
        t.text_button = hex32(0xFFF3DC);
        t.section = hex32(0xE5C589);
        t.value = hex32(0xFFF3DC);
        t.value_changed = hex32(0xFFEDB0);
        t.warn = hex32(0xF2A48C);
        t.title = hex32(0xECD78E);
        t.arrow = hex32(0xF0DC9A);
        t.ring_w = 4.0f;
        t.pill_gloss = 0.18f;  // (a soft sheen: a bright one washed out the cream text)
        t.edge = 0.08f;
        t.mottle = 150.0f;
        t.dim = 0.45f;
        t.glare = 0.14f;
    }
    return t;
}

// one piece, as the shader reads it (16-byte rows), and the frame's values after it (push constants on Vulkan)
struct PShape {
    float rect[4];  // x0 y0 x1 y1, pixels
    float col[4];
    float col2[4];
    float p[4];  // radius, depth, gloss, kind
    float q[4];  // ring width, slab: patch size / pill: edge darkening, -, -
};
struct PFrame { float vp[2], mouse[2], scale, glare, has_bg, pad; };

// ---- the shader body, the same for MSL and GLSL (float2 etc. are #defines in GLSL)
static const char kShade[] = R"SH(
struct Shape { float4 rect; float4 col; float4 col2; float4 p; float4 q; };

float sdbox(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
float2 sdgrad(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + r;
    float2 sg = float2(p.x < 0.0 ? -1.0 : 1.0, p.y < 0.0 ? -1.0 : 1.0);
    float2 g = max(q.x, q.y) > 0.0 ? normalize(max(q, 0.0) + 1e-4) : (q.x > q.y ? float2(1.0, 0.0) : float2(0.0, 1.0));
    return g * sg;
}
// over a dark scene a light piece comes down a little - evenly (lum: how bright the game is around the menu)
float3 glare_guard(float3 col, float lum, float glare) {
    if (glare <= 0.0) return col;
    float k = smoothstep(0.10, 0.45, lum);
    return col * mix((1.0 - glare) * float3(1.0, 0.975, 0.93), float3(1.0, 1.0, 1.0), k);  // (darker, still warm)
}
float piece_margin(Shape s) { return s.p.y * 2.6 + 3.0; }
// soft value noise (the panel's patches)
float hash2(float2 p) { return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float vnoise(float2 p) {
    float2 i = floor(p), f = fract(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash2(i), b = hash2(i + float2(1.0, 0.0)), c = hash2(i + float2(0.0, 1.0)), d = hash2(i + float2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// premultiplied colour of the piece (and its shadow) at pixel px
float4 shade(float2 px, Shape s, float2 mouse, float scale, float glare, float lum) {
    float3 L = normalize(float3(-0.45, -0.75, 0.85));
    float3 H = normalize(L + float3(0.0, 0.0, 1.0));
    float2 c = (s.rect.xy + s.rect.zw) * 0.5, hs = (s.rect.zw - s.rect.xy) * 0.5;
    float2 lp = px - c;
    float r = min(s.p.x, min(hs.x, hs.y));
    float depth = s.p.y, gloss = s.p.z;
    int kind = int(s.p.w + 0.5);
    float px1 = max(scale, 0.75);  // (one design pixel)
    float3 base = s.col.rgb;
    float alpha = s.col.a;

    if (kind == 1) {  // ---- inset: a recessed well (no drop shadow: an inner one along its top)
        float d = sdbox(lp, hs, r);
        float cov = clamp(0.5 - d, 0.0, 1.0);
        if (cov <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
        float2 g = sdgrad(lp, hs, r);
        float bev = depth * 0.8;
        float u = 1.0 - clamp(-d / bev, 0.0, 1.0);
        float3 n = normalize(float3(-g * u * 0.8, sqrt(max(1.0 - u * u, 0.0)) + 0.1));
        float lf = 0.6 + 0.4 * max(dot(n, L), 0.0) / L.z;
        float3 col = base * lf;
        float dsh = sdbox(lp - float2(0.0, depth * 0.9), hs, r);
        float is = smoothstep(-depth * 1.4, depth * 0.3, dsh);
        col *= 1.0 - 0.38 * is;
        float lip = (1.0 - smoothstep(0.0, 2.0 * px1, -d)) * clamp(g.y, 0.0, 1.0);
        col = col + (1.0 - col) * lip * 0.30;
        col = glare_guard(col, lum, glare);
        return float4(col * cov * alpha, cov * alpha);
    }

    float d;
    float u;
    float2 g;
    float3 n;
    float w = s.q.x;
    if (kind == 3) {  // ---- ring: a rounded moulding (a tube) around the panel
        float2 hc = hs - w * 0.5;
        float rc = max(r - w * 0.5, 0.0);
        float dc = sdbox(lp, hc, rc);
        if (dc < -w * 0.5 - depth * 2.0 - 2.0) return float4(0.0, 0.0, 0.0, 0.0);  // (inside the hole, past its shadow)
        d = abs(dc) - w * 0.5;
        g = sdgrad(lp, hc, rc);
        u = clamp(dc / (w * 0.5), -1.0, 1.0);
        n = normalize(float3(g * u * 0.9, sqrt(max(1.0 - u * u, 0.0)) + 0.08));
    } else {  // ---- slab / pill: a raised piece with a rounded bevel
        d = sdbox(lp, hs, r);
        g = sdgrad(lp, hs, r);
        float bev = kind == 0 ? depth * 1.2 : max(min(depth * 1.1, min(hs.x, hs.y) * 0.9), 1.0);
        u = 1.0 - clamp(-d / bev, 0.0, 1.0);
        n = float3(g * u * 0.95, sqrt(max(1.0 - u * u, 0.0)) + 0.04);
        float2 pil = lp / hs * (kind == 2 ? 0.10 : 0.025);  // (a slight pillow over the whole top)
        n = normalize(float3(n.xy + pil, n.z));
    }
    float cov = clamp(0.5 - d, 0.0, 1.0);

    // drop shadow (outside the piece; a ring also shades the panel just inside it)
    float2 so = float2(depth * 0.18, depth * 0.55);
    float ds = sdbox(lp - so, hs, r);
    float sh = (1.0 - smoothstep(-depth * 0.7, depth * 1.6, ds)) * (kind == 0 ? 0.55 : 0.42);
    if (kind == 3) {
        float din = sdbox(lp - so * 0.8, hs - w, max(r - w, 0.0));
        float hole = sdbox(lp, hs - w, max(r - w, 0.0));
        if (hole < 0.0) sh = smoothstep(-depth * 1.3, 0.0, din) * 0.45;
    }
    sh *= alpha;
    if (cov <= 0.0) return float4(0.0, 0.0, 0.0, sh > 0.002 ? sh : 0.0);

    if (kind == 3) base = mix(s.col2.rgb, s.col.rgb, smoothstep(-0.30, 0.05, u));  // (inner stripe, outer moulding)
    float rim_line = 0.0;
    if (kind == 0 && s.q.y > 0.0) {  // the panel in soft patches of col / col2
        float2 pp = (px - s.rect.xy) / s.q.y;
        float m = vnoise(pp) * 0.65 + vnoise(pp * 2.3 + float2(17.0, 5.0)) * 0.35;
        base = mix(s.col.rgb, s.col2.rgb, smoothstep(0.2, 0.8, m));
    }
    float rimk = 0.0;
    if (kind == 2 && w > 0.0) {  // a rim of col2 round the edge; inside it the fill darkens toward the rim (q.y)
        float id = -d;
        float inner = smoothstep(w, w + max(min(hs.x, hs.y) * 0.8, 1.0), id);
        base *= 1.0 - s.q.y * (1.0 - inner);
        rimk = 1.0 - smoothstep(w - 0.6 * px1, w + 0.6 * px1, id);
        rim_line = 1.0 - smoothstep(0.0, 1.3 * px1, abs(id - w));
    }
    float diff = max(dot(n, L), 0.0);
    float lf = 0.58 + 0.42 * diff / L.z;
    float3 col = base * lf;
    if (kind == 2) col *= mix(1.06, 0.80, smoothstep(-0.9, 1.0, lp.y / hs.y));  // (a pill: lit on top, its belly in shade)
    // specular: where the bevel turns toward the light (the rim catching it)
    float spw = kind == 0 ? 28.0 : 36.0;
    float spec = max(pow(max(dot(n, H), 0.0), spw) - pow(H.z, spw), 0.0) * gloss * (kind == 3 ? 1.6 : 1.25);
    col += float3(spec, spec, spec);
    // rim light: the top-left edge lit, the bottom-right in its own shade
    float2 ld = normalize(-L.xy);
    float rim = u * u * dot(g, ld);
    col = rim > 0.0 ? col + (1.0 - col) * rim * 0.38 * (0.4 + gloss) : col * (1.0 + rim * 0.30);
    // clear-coat sheen
    float sheen = 0.0;
    if (kind == 2) {  // the classic gloss: a bright band over the top half, fading down
        float ins = max(min(depth * 0.55, hs.y * 0.3), 1.0);
        float2 sc = float2(0.0, -hs.y * 0.42);
        float2 sb = float2(hs.x - ins * 1.2, hs.y * 0.46);
        float d2 = sdbox(lp - sc, sb, min(r, sb.y));
        float t = clamp((lp.y + hs.y) / hs.y, 0.0, 1.0);
        sheen = (1.0 - smoothstep(-0.8 * px1, 0.8 * px1, d2)) * mix(0.50, 0.06, t) * gloss;
    } else if (kind == 0) {  // a broad soft diagonal reflection across the slab
        float2 q = lp / hs;
        float band = dot(q, normalize(float2(0.55, 1.0)));
        sheen = smoothstep(0.55, 0.0, abs(band + 0.62)) * 0.085 * gloss * (1.0 - u);
    }
    // a soft highlight leaning toward the mouse (or from the top left without one)
    float2 mp = mouse.x >= 0.0 ? mouse : c + float2(-hs.x, -hs.y * 1.4);
    float2 hc2 = c + clamp((mp - c) * 0.35, -hs * 0.8, hs * 0.8) + float2(0.0, -hs.y * 0.25);
    float R = kind == 0 ? max(hs.x, hs.y) * 0.65 : max(hs.x * 0.55, hs.y * 1.2);
    float2 dh = (px - hc2) / float2(1.0, 0.75);
    sheen += exp(-dot(dh, dh) / (R * R)) * (kind == 0 ? 0.05 : 0.10) * gloss;
    col = col + (1.0 - col) * sheen;
    // the rim: its own colour, lit more evenly than the bevel it sits on (it reads all the way round)
    if (rimk > 0.0) col = mix(col, s.col2.rgb * (0.80 + 0.30 * diff / L.z) + float3(spec, spec, spec), rimk);
    // a fine dark edge: the piece reads as a separate part
    float edge = 1.0 - smoothstep(0.0, 1.6 * px1, -d);
    if (kind == 3) edge = max(edge, 1.0 - smoothstep(0.0, 1.4 * px1, d + w));
    col *= 1.0 - 0.30 * edge;
    col *= 1.0 - 0.22 * rim_line;  // (a fine line between a rim and its fill)

    col = glare_guard(col, lum, glare);
    float a = cov * alpha;
    return float4(col * a, a + sh * (1.0 - a));  // (premultiplied: the piece over its shadow)
}
)SH";

// ---- the pieces: callbacks
static PFrame g_frame;
static bool g_piece_ok;  // the pipeline exists (and on Metal: an encoder is set) - else a callback draws nothing
static int g_repeat = -1;  // LARECOMP_UI_PERF_REPEAT=<n> (test): every piece drawn n times, to measure its cost

static float g_fbs = 1;  // pixels per ImGui unit (the start screen on a Retina display: 2; the game's overlay: 1)
static void piece_clip(const ImDrawCmd *cmd, float *x0, float *y0, float *x1, float *y1) {
    *x0 = fmaxf(cmd->ClipRect.x * g_fbs, 0), *y0 = fmaxf(cmd->ClipRect.y * g_fbs, 0);
    *x1 = fminf(cmd->ClipRect.z * g_fbs, g_frame.vp[0]), *y1 = fminf(cmd->ClipRect.w * g_fbs, g_frame.vp[1]);
}
// the piece in pixels
static PShape piece_px(const void *data) {
    PShape s = *(const PShape *)data;
    for (float &v : s.rect) v *= g_fbs;
    s.p[0] *= g_fbs, s.p[1] *= g_fbs, s.q[0] *= g_fbs;
    if ((int)s.p[3] == PK_SLAB) s.q[1] *= g_fbs;  // (the patches' size)
    return s;
}
// the frame's values, from the target's size in pixels (mouse, scale: ImGui's units)
static void frame_set(float w, float h, ImVec2 mouse, float scale) {
    float dw = ImGui::GetIO().DisplaySize.x;
    g_fbs = dw > 0 ? w / dw : 1;
    g_frame.vp[0] = w, g_frame.vp[1] = h;
    bool mouse_ok = mouse.x > -1e30f && mouse.x >= 0 && mouse.y >= 0;
    g_frame.mouse[0] = mouse_ok ? mouse.x * g_fbs : -1, g_frame.mouse[1] = mouse_ok ? mouse.y * g_fbs : -1;
    g_frame.scale = scale * g_fbs;
    g_frame.glare = plastic_theme().glare;
    g_frame.has_bg = 0;
}
static int repeat_count(void) {
    if (g_repeat < 0) g_repeat = getenv("LARECOMP_UI_PERF_REPEAT") ? atoi(getenv("LARECOMP_UI_PERF_REPEAT")) : 1;
    return g_repeat < 1 ? 1 : g_repeat;
}

#ifdef __APPLE__
#import <Metal/Metal.h>

static const char kMslHead[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
)MSL";
static const char kMslMain[] = R"MSL(
struct Frame { float2 vp; float2 mouse; float scale; float glare; float has_bg; float pad; };
struct VOut { float4 pos [[position]]; float2 px; float lum [[flat]]; };

// how bright the game is around the menu: the small copy's edges
static float scene_lum(constant Frame &f, texture2d<float> bg, sampler smp) {
    if (f.has_bg < 0.5) return 1.0;
    float lum = 0.0;
    for (int i = 0; i < 8; i++) {
        float t = (i + 0.5) / 8.0;
        float3 a = bg.sample(smp, float2(0.04, t)).rgb, b = bg.sample(smp, float2(0.96, t)).rgb;
        float3 c = bg.sample(smp, float2(t, 0.05)).rgb, d = bg.sample(smp, float2(t, 0.95)).rgb;
        lum += dot(a + b + c + d, float3(0.2126, 0.7152, 0.0722)) / 32.0;
    }
    return lum;
}

vertex VOut v_main(uint vid [[vertex_id]], constant Shape &s [[buffer(0)]], constant Frame &f [[buffer(1)]],
                   texture2d<float> bg [[texture(0)]], sampler smp [[sampler(0)]]) {
    float2 corner = float2(vid & 1, vid >> 1);
    float m = piece_margin(s);
    float2 px = mix(s.rect.xy - m, s.rect.zw + m, corner);
    VOut o;
    o.pos = float4(px.x / f.vp.x * 2.0 - 1.0, 1.0 - px.y / f.vp.y * 2.0, 0.0, 1.0);
    o.px = px;
    o.lum = scene_lum(f, bg, smp);  // (per corner, not per pixel)
    return o;
}

fragment float4 f_main(VOut in [[stage_in]], constant Shape &s [[buffer(0)]], constant Frame &f [[buffer(1)]]) {
    float4 c = shade(in.px, s, f.mouse, f.scale, f.glare, in.lum);
    if (c.a <= 0.0) discard_fragment();
    return c;
}

// the small copy: each texel the average of 4 x 4 taps over its part of the game image (uv: its rect)
struct BOut { float4 pos [[position]]; };
vertex BOut v_bg(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    BOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}
fragment float4 f_bg(BOut in [[stage_in]], constant float4 &uv [[buffer(0)]], texture2d<float> scene [[texture(0)]],
                     sampler smp [[sampler(0)]]) {
    float2 n = float2(64.0, 32.0);
    float2 base = floor(in.pos.xy);
    float3 sum = float3(0.0);
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            float2 t = (base + (float2(i, j) + 0.5) / 4.0) / n;
            sum += scene.sample(smp, uv.xy + t * uv.zw).rgb;
        }
    return float4(sum / 16.0, 1.0);
}
)MSL";

static id<MTLRenderPipelineState> g_pipe, g_bg_pipe;
static MTLPixelFormat g_pipe_fmt = MTLPixelFormatInvalid;
static id<MTLSamplerState> g_smp;
static id<MTLTexture> g_bg, g_dummy;
static id<MTLRenderCommandEncoder> g_enc;

static void build(id<MTLDevice> dev, MTLPixelFormat fmt) {
    if (g_pipe && g_pipe_fmt == fmt) return;
    static bool failed;
    if (failed) return;
    NSError *err = nil;
    std::string src = std::string(kMslHead) + kShade + kMslMain;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(src.c_str()) options:nil error:&err];
    if (!lib) { fprintf(stderr, "[plastic] shader: %s\n", err.localizedDescription.UTF8String); failed = true; return; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"v_main"];
    pd.fragmentFunction = [lib newFunctionWithName:@"f_main"];
    pd.colorAttachments[0].pixelFormat = fmt;
    pd.colorAttachments[0].blendingEnabled = YES;  // (premultiplied)
    pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    g_pipe = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!g_pipe) { fprintf(stderr, "[plastic] pipeline: %s\n", err.localizedDescription.UTF8String); failed = true; return; }
    g_pipe_fmt = fmt;
    MTLRenderPipelineDescriptor *bd = [MTLRenderPipelineDescriptor new];
    bd.vertexFunction = [lib newFunctionWithName:@"v_bg"];
    bd.fragmentFunction = [lib newFunctionWithName:@"f_bg"];
    bd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    g_bg_pipe = [dev newRenderPipelineStateWithDescriptor:bd error:&err];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    g_smp = [dev newSamplerStateWithDescriptor:sd];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64
                                                                                  height:32 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    g_bg = [dev newTextureWithDescriptor:td];
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    g_dummy = [dev newTextureWithDescriptor:td];
}

void plastic_frame_begin(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target, id<MTLTexture> scene,
                         const float scene_uv[4], ImVec2 mouse, float scale) {
    g_enc = nil;
    build(dev, target.pixelFormat);
    g_piece_ok = false;
    if (!g_pipe) return;
    frame_set((float)target.width, (float)target.height, mouse, scale);
    if (scene && g_bg_pipe) {  // the small copy of the game image (only while the menu is open)
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = g_bg;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:g_bg_pipe];
        [e setFragmentBytes:scene_uv length:16 atIndex:0];
        [e setFragmentTexture:scene atIndex:0];
        [e setFragmentSamplerState:g_smp atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        g_frame.has_bg = 1;
    }
}

void plastic_set_encoder(id<MTLRenderCommandEncoder> enc) {
    g_enc = g_pipe ? enc : nil;
    g_piece_ok = g_enc != nil;
}

// LARECOMP_UI_PERF=1 (test): the present command buffer's GPU time (the game image's scaling draw + the overlay),
// averaged over 240 frames, logged with the menu's state
void plastic_perf_note(id<MTLCommandBuffer> cb, bool menu_open) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_UI_PERF") != NULL;
    if (!on) return;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        static double sum;
        static int n, n_open;
        double ms = (done.GPUEndTime - done.GPUStartTime) * 1000;
        if (ms <= 0 || ms > 100) return;
        sum += ms, n++, n_open += menu_open;
        if (n == 240) {
            fprintf(stderr, "[plastic] perf: menu %s repeat %d: present GPU %.3f ms avg (240 frames)\n",
                    n_open == 240 ? "open" : n_open ? "mixed" : "closed", repeat_count(), sum / n);
            sum = 0, n = n_open = 0;
        }
    }];
}

static void draw_cb(const ImDrawList *, const ImDrawCmd *cmd) {
    if (!g_piece_ok || !g_enc || !cmd->UserCallbackData) return;
    PShape s = piece_px(cmd->UserCallbackData);
    float x0, y0, x1, y1;
    piece_clip(cmd, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) return;
    [g_enc setScissorRect:(MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}];
    [g_enc setRenderPipelineState:g_pipe];
    [g_enc setVertexBytes:&s length:sizeof s atIndex:0];
    [g_enc setVertexBytes:&g_frame length:sizeof g_frame atIndex:1];
    [g_enc setFragmentBytes:&s length:sizeof s atIndex:0];
    [g_enc setFragmentBytes:&g_frame length:sizeof g_frame atIndex:1];
    [g_enc setVertexTexture:g_frame.has_bg > 0.5f ? g_bg : g_dummy atIndex:0];
    [g_enc setVertexSamplerState:g_smp atIndex:0];
    for (int i = 0, n = repeat_count(); i < n; i++) [g_enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

#else  // ---- Vulkan (runtime/nvn/vulkan/plastic_vk.c)
#include "imgui_impl_vulkan.h"
extern "C" {
bool vk_plastic_init(const char *vs, const char *fs, const char *bg_fs);
void vk_plastic_bg(void *scene_view, const float uv[4]);
void vk_plastic_draw(VkCommandBuffer cb, const void *push, uint32_t size, int x, int y, uint32_t w, uint32_t h, float vw,
                     float vh, bool has_bg, int repeat);
bool vk_gpu_split_ns(uint64_t out[4]);
}

static const char kGlslHead[] = R"GL(#version 450
#define float2 vec2
#define float3 vec3
#define float4 vec4
)GL";
static const char kGlslPush[] = R"GL(
layout(push_constant) uniform P { vec4 rect; vec4 col; vec4 col2; vec4 p; vec4 q; vec4 f0; vec4 f1; } pc;
layout(set = 0, binding = 0) uniform sampler2D bg;
Shape piece() { return Shape(pc.rect, pc.col, pc.col2, pc.p, pc.q); }
)GL";
static const char kGlslVert[] = R"GL(
layout(location = 0) out vec2 o_px;
layout(location = 1) flat out float o_lum;
// how bright the game is around the menu: the small copy's edges (f1.z: there is one)
float scene_lum() {
    if (pc.f1.z < 0.5) return 1.0;
    float lum = 0.0;
    for (int i = 0; i < 8; i++) {
        float t = (float(i) + 0.5) / 8.0;
        vec3 a = textureLod(bg, vec2(0.04, t), 0.0).rgb, b = textureLod(bg, vec2(0.96, t), 0.0).rgb;
        vec3 c = textureLod(bg, vec2(t, 0.05), 0.0).rgb, d = textureLod(bg, vec2(t, 0.95), 0.0).rgb;
        lum += dot(a + b + c + d, vec3(0.2126, 0.7152, 0.0722)) / 32.0;
    }
    return lum;
}
void main() {
    Shape s = piece();
    vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    float m = piece_margin(s);
    vec2 px = mix(s.rect.xy - m, s.rect.zw + m, corner);
    gl_Position = vec4(px.x / pc.f0.x * 2.0 - 1.0, px.y / pc.f0.y * 2.0 - 1.0, 0.0, 1.0);
    o_px = px;
    o_lum = scene_lum();
}
)GL";
static const char kGlslFrag[] = R"GL(
layout(location = 0) in vec2 i_px;
layout(location = 1) flat in float i_lum;
layout(location = 0) out vec4 o;
void main() {
    vec4 c = shade(i_px, piece(), pc.f0.zw, pc.f1.x, pc.f1.y, i_lum);
    if (c.a <= 0.0) discard;
    o = c;
}
)GL";
static const char kGlslBg[] = R"GL(#version 450
layout(push_constant) uniform P { vec4 uv; } pc;
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(location = 0) out vec4 o;
// the small copy: each texel the average of 4 x 4 taps over its part of the game image
void main() {
    vec2 n = vec2(64.0, 32.0);
    vec2 base = floor(gl_FragCoord.xy);
    vec3 sum = vec3(0.0);
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            vec2 t = (base + (vec2(i, j) + 0.5) / 4.0) / n;
            sum += texture(scene, pc.uv.xy + t * pc.uv.zw).rgb;
        }
    o = vec4(sum / 16.0, 1.0);
}
)GL";

void plastic_frame_begin_vk(void *scene, const float scene_uv[4], float w, float h, ImVec2 mouse, float scale) {
    static int built = -1;
    if (built < 0) {
        std::string vs = std::string(kGlslHead) + kShade + kGlslPush + kGlslVert;
        std::string fs = std::string(kGlslHead) + kShade + kGlslPush + kGlslFrag;
        built = vk_plastic_init(vs.c_str(), fs.c_str(), kGlslBg);
    }
    g_piece_ok = built > 0;
    if (!g_piece_ok) return;
    frame_set(w, h, mouse, scale);
    if (scene) {  // the small copy of the game image (only while the menu is open)
        vk_plastic_bg(scene, scene_uv);
        g_frame.has_bg = 1;
    }
}

// LARECOMP_UI_PERF=1 (test): the window image's GPU time per frame (the game image's scaling draw + the overlay),
// averaged over 240 frames, logged with the menu's state
void plastic_perf_note_vk(bool menu_open) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_UI_PERF") != NULL;
    if (!on) return;
    static uint64_t prev[4];
    static int n, n_open;
    uint64_t sp[4];
    if (!vk_gpu_split_ns(sp)) return;
    if (!prev[3]) { memcpy(prev, sp, sizeof prev); return; }
    n_open += menu_open;
    if (++n < 240) return;
    double frames = (double)(sp[3] - prev[3]);
    if (frames > 0)
        fprintf(stderr, "[plastic] perf: menu %s repeat %d: window GPU %.3f ms avg (%.0f frames)\n",
                n_open == n ? "open" : n_open ? "mixed" : "closed", repeat_count(), (sp[1] - prev[1]) / 1e6 / frames, frames);
    memcpy(prev, sp, sizeof prev);
    n = n_open = 0;
}

static void draw_cb(const ImDrawList *, const ImDrawCmd *cmd) {
    if (!g_piece_ok || !cmd->UserCallbackData) return;
    ImGui_ImplVulkan_RenderState *rs = (ImGui_ImplVulkan_RenderState *)ImGui::GetPlatformIO().Renderer_RenderState;
    if (!rs) return;
    float x0, y0, x1, y1;
    piece_clip(cmd, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0) return;
    float push[28];
    PShape s = piece_px(cmd->UserCallbackData);
    memcpy(push, &s, sizeof s);
    memcpy(push + 20, &g_frame, sizeof g_frame);
    vk_plastic_draw(rs->CommandBuffer, push, sizeof push, (int)x0, (int)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0),
                    g_frame.vp[0], g_frame.vp[1], g_frame.has_bg > 0.5f, repeat_count());
}
#endif

void plastic_add(ImDrawList *dl, ImVec2 a, ImVec2 b, float radius, ImVec4 col, int kind, float depth, float gloss,
                 ImVec4 col2, float ring, float extra) {
    if (b.x <= a.x || b.y <= a.y) return;
    PShape s = {{a.x, a.y, b.x, b.y}, {col.x, col.y, col.z, col.w}, {col2.x, col2.y, col2.z, col2.w},
                {radius, depth, gloss, (float)kind}, {ring, extra, 0, 0}};
    dl->AddCallback(draw_cb, &s, sizeof s);
    dl->AddCallback(ImDrawCallback_ResetRenderState, NULL);
}

void plastic_pill(ImDrawList *dl, ImVec2 a, ImVec2 b, float radius, ImVec4 col, float depth, float gloss, float scale,
                  bool lit) {
    const PlasticTheme &t = plastic_theme();
    ImVec4 ring = lit ? t.ring_sel : t.ring;
    ring.w = col.w;  // (a faded piece: its rim too)
    float w = fminf(t.ring_w * scale, (b.y - a.y) * 0.12f);
    plastic_add(dl, a, b, radius, col, PK_PILL, depth, gloss * t.pill_gloss, ring, w, t.edge);
}

void plastic_panel(ImDrawList *dl, ImVec2 a, ImVec2 b, float scale) {
    const PlasticTheme &t = plastic_theme();
    float small = fminf(b.x - a.x, b.y - a.y) < 420 * scale ? 0.7f : 1.0f;  // (popups: a slimmer frame)
    float fw = 13 * scale * small, in = 7 * scale * small;
    dl->PushClipRectFullScreen();
    // (a soft sheen: the dark paper stays close to its own colour)
    plastic_add(dl, a, b, 26 * scale * small, t.panel, PK_SLAB, 14 * scale * small, 0.3f, t.panel2, 0, t.mottle * scale);
    plastic_add(dl, ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), 20 * scale * small, t.frame, PK_RING,
                7 * scale * small, 1.0f, t.frame_inner, fw);
    dl->PopClipRect();
}

void plastic_well(ImDrawList *dl, ImVec2 a, ImVec2 b, float scale) {
    dl->PushClipRectFullScreen();
    plastic_add(dl, a, b, 16 * scale, plastic_theme().inset, PK_INSET, 8 * scale, 0);
    dl->PopClipRect();
}

// the round button's piece and its icon: the X, or (reset) an arrow going round
static bool round_button(const char *id, float bx, float scale, bool reset) {
    const PlasticTheme &t = plastic_theme();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool pressed = ImGui::InvisibleButton(id, ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool hot = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && GImGui->NavCursorVisible);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    plastic_pill(dl, p, ImVec2(p.x + bx, p.y + bx), bx * 0.5f, hot ? t.row_sel : t.row, 6 * scale, 1.0f, scale, hot);
    ImU32 col = hot ? t.text_sel : t.text;
    float m = bx * 0.32f, th = (hot ? 3.0f : 2.6f) * scale * 1.5f, dy = -1 * scale;
    if (!reset) {
        dl->AddLine(ImVec2(p.x + m, p.y + m + dy), ImVec2(p.x + bx - m, p.y + bx - m + dy), col, th);
        dl->AddLine(ImVec2(p.x + bx - m, p.y + m + dy), ImVec2(p.x + m, p.y + bx - m + dy), col, th);
        return pressed;
    }
    // an open circle from the top, round anticlockwise to the top right, with the arrowhead at its start
    ImVec2 c(p.x + bx * 0.5f, p.y + bx * 0.5f + dy);
    float r = bx * 0.2f, a0 = -IM_PI * 0.5f, a1 = a0 - IM_PI * 1.55f;
    dl->PathArcTo(c, r, a0, a1, 24);
    dl->PathStroke(col, 0, th);
    float h = th * 1.7f;  // the arrowhead at the top, pointing right (the way the circle turns back to it)
    ImVec2 tip(c.x + h * 0.9f, c.y - r);
    dl->AddTriangleFilled(tip, ImVec2(c.x - h * 0.2f, c.y - r - h), ImVec2(c.x - h * 0.2f, c.y - r + h), col);
    return pressed;
}

bool plastic_close_button(float bx, float scale) { return round_button("##close", bx, scale, false); }

// slot 0: the corner itself; each slot further one button (and a gap) to the left
static bool corner_button(float bx, float scale, int slot, bool reset) {
    // in the window's top right corner, clear of the frame (its moulding ends 20 px in), and out of the layout: the
    // line it's on keeps its height
    ImGuiWindow *w = ImGui::GetCurrentWindow();
    ImGuiWindowTempData keep = w->DC;
    float m = 34 * scale;
    ImGui::SetCursorScreenPos(ImVec2(w->Pos.x + w->Size.x - m - bx - slot * (bx + 12 * scale), w->Pos.y + m));
    bool r = round_button(reset ? "##reset" : "##close", bx, scale, reset);
    w->DC.CursorPos = keep.CursorPos, w->DC.CursorPosPrevLine = keep.CursorPosPrevLine;
    w->DC.CursorMaxPos = keep.CursorMaxPos, w->DC.IdealMaxPos = keep.IdealMaxPos;
    w->DC.CurrLineSize = keep.CurrLineSize, w->DC.PrevLineSize = keep.PrevLineSize;
    w->DC.CurrLineTextBaseOffset = keep.CurrLineTextBaseOffset, w->DC.PrevLineTextBaseOffset = keep.PrevLineTextBaseOffset;
    w->DC.IsSameLine = keep.IsSameLine, w->DC.IsSetPos = keep.IsSetPos;
    return r;
}

bool plastic_close_corner(float bx, float scale) { return corner_button(bx, scale, 0, false); }

bool plastic_reset_corner(float bx, float scale) {
    bool r = corner_button(bx, scale, 1, true);
    if (ImGui::IsItemHovered() || (ImGui::IsItemFocused() && GImGui->NavCursorVisible)) ImGui::SetTooltip("%s", L("Reset to vanilla"));
    return r;
}

// ---- ImGui's own frames and windows as pieces (third_party/imgui: GImGuiLasFrameHook, called from RenderFrame and
// a window's background)
static bool g_widgets;
extern ImU32 GImGuiLasButtonText;
static bool frame_hook(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, ImU32 col, float rounding, int what, int flags) {
    if (!g_widgets) return false;
    ImGuiContext &g = *GImGui;
    const PlasticTheme &t = plastic_theme();
    float scale = g.Style.ScrollbarSize / 18.0f;  // (the style is scaled to the window: menu.mm)
    if (what == 1) {  // a window's background: top-level windows and popups get the panel; child windows stay clear
        if ((flags & ImGuiWindowFlags_ChildWindow) && !(flags & ImGuiWindowFlags_Popup)) return false;
        if (flags & ImGuiWindowFlags_Tooltip) return false;
        plastic_panel(dl, a, b, scale);
        return true;
    }
    auto is = [&](ImGuiCol c) { return col == ImGui::GetColorU32(c); };
    bool focus = g.NavId && g.LastItemData.ID == g.NavId && g.NavCursorVisible;
    float h = b.y - a.y, r = fminf(rounding > 0 ? rounding * 1.25f : 10 * scale, h * 0.5f);
    if (is(ImGuiCol_Button) || is(ImGuiCol_ButtonHovered) || is(ImGuiCol_ButtonActive)) {
        bool hot = !is(ImGuiCol_Button) || focus;
        ImVec4 c = hot ? t.row_hot : t.row;
        c.w *= ImGui::ColorConvertU32ToFloat4(col).w;  // (disabled: faded)
        if (hot) GImGuiLasButtonText = t.text_sel;  // (its label, drawn next: light on the dark piece)
        plastic_pill(dl, ImVec2(a.x + 1, a.y + 1), ImVec2(b.x - 1, b.y - 2 * scale), r, c, (hot ? 6 : 4.5f) * scale,
                     hot ? 1.0f : 0.7f, scale, hot);
        return true;
    }
    if (is(ImGuiCol_Header) || is(ImGuiCol_HeaderHovered) || is(ImGuiCol_HeaderActive)) {
        bool hot = !is(ImGuiCol_Header) || focus;
        plastic_pill(dl, a, b, r, hot ? t.row_mark : t.row, 3.5f * scale, 0.6f, scale, hot);  // (deep ochre: cream text stays)
        return true;
    }
    if (is(ImGuiCol_FrameBg) || is(ImGuiCol_FrameBgHovered) || is(ImGuiCol_FrameBgActive)) {
        ImVec4 c = t.inset;
        c.w *= ImGui::ColorConvertU32ToFloat4(col).w;
        plastic_add(dl, a, b, fminf(r, 12 * scale), c, PK_INSET, fminf(6 * scale, h * 0.3f), 0);
        if (focus || g.LastItemData.ID == g.ActiveId)  // (the text field being typed in / under the cursor)
            dl->AddRect(a, b, ImGui::ColorConvertFloat4ToU32(t.row_sel), fminf(r, 12 * scale), 0, 2.5f * scale);
        return true;
    }
    return false;
}
extern bool (*GImGuiLasFrameHook)(ImDrawList *, const ImVec2 &, const ImVec2 &, ImU32, float, int, int);
void plastic_widgets(bool on) {
    GImGuiLasFrameHook = frame_hook;
    g_widgets = on;
}
