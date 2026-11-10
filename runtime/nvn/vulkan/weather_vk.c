// The weather on Vulkan (runtime/nvn/weather.h; renderer.m's weather_encode is the Metal side, and the why): one pass
// of its own on the window image right after the game's scene composite - the overcast / lightning fill, the rain
// streaks or snow flakes, the splashes, a storm's leaves and lightning strike, the snow Link kicks up. The scene depth
// is read as a texture. The shaders are renderer.m's wx_src in GLSL, the same math: the pass draws through a
// negative-height viewport as the game's draws do (exec.c's set_viewport: Metal's convention, NDC y up), so the window
// image's and the scene depth's rows are Metal's and a pixel's NDC y is 1 - uv.y * 2 on both. The clip transform (flip
// y, depth -1..1, the depth range, mirrored) is the game's own on both, as its translated shaders do.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "record.h"
#include "../weather.h"

#define WX_PRELUDE                                                                                                     \
    "#version 450\n"                                                                                                   \
    "layout(std140, set = 0, binding = 0) uniform WF { vec4 vp[4]; vec4 ivp[4]; vec4 origin, box, shape, color, "      \
    "screen, flags, fill, zr, cam, wind, leaf; uint count, splashes; int kind; } w;\n"                                 \
    "layout(set = 0, binding = 1) uniform sampler2D dt;\n"                                                             \
    "#define saturate(x) clamp(x, 0.0, 1.0)\n"                                                                         \
    "uint pcg(uint v) { uint s = v * 747796405u + 2891336453u; uint q = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u; " \
    "return (q >> 22u) ^ q; }\n"                                                                                       \
    "float h1(uint v) { return float(pcg(v)) * (1.0 / 4294967296.0); }\n"                                              \
    "float dread(uvec2 q) { return texelFetch(dt, ivec2(q), 0).r; }\n"

// the scene geometry: drops, the volume, what's under a point (renderer.m's comments say what each does)
static const char *wx_common =
    "vec4 game_clip(vec3 p) {\n"
    "  vec4 q = vec4(p, 1.0);\n"
    "  return vec4(dot(w.vp[0], q), dot(w.vp[1], q), dot(w.vp[2], q), dot(w.vp[3], q));\n"
    "}\n"
    "vec4 clip_of(vec3 p) {\n"
    "  vec4 c = game_clip(p);\n"
    "  if (w.flags.x > 0.5) c.y = -c.y;\n"
    "  if (w.flags.y > 0.5) c.z = (c.z + c.w) * 0.5;\n"
    "  c.z = w.zr.x * c.w + w.zr.y * c.z;\n"
    "  if (w.flags.z > 0.5) c.x = -c.x;\n"
    "  return c;\n"
    "}\n"
    "vec3 world_at(vec2 uv, float d) {\n"
    "  vec2 m = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"
    "  if (w.flags.x > 0.5) m.y = -m.y;\n"
    "  float z = (d - w.zr.x) / w.zr.y;\n"
    "  if (w.flags.y > 0.5) z = z * 2.0 - 1.0;\n"
    "  vec4 q = vec4(m, z, 1.0);\n"
    "  vec4 wp = vec4(dot(w.ivp[0], q), dot(w.ivp[1], q), dot(w.ivp[2], q), dot(w.ivp[3], q));\n"
    "  return wp.xyz / wp.w;\n"
    "}\n"
    "vec3 wx_drop(uint iid, float tt, out float speed, out float age) {\n"
    "  vec3 sz = vec3(w.box.x, w.box.y, w.box.x);\n"
    "  vec3 r = vec3(h1(iid * 3u + 1u), h1(iid * 3u + 2u), h1(iid * 3u + 3u));\n"
    "  speed = w.shape.z * (0.8 + 0.4 * h1(iid * 7u + 5u));\n"
    "  vec3 lo = w.origin.xyz - 0.5 * sz;\n"
    "  float back = w.box.z - tt;\n"
    "  vec3 f = fract((r * sz + vec3(w.wind.x - w.wind.z * back, -speed * tt, w.wind.y - w.wind.w * back) - lo) / sz);\n"
    "  age = (1.0 - f.y) * sz.y / speed;\n"
    "  vec3 p = lo + f * sz;\n"
    "  if (abs(w.flags.w - 2.0) < 0.5) {\n"
    "    float ph = r.x * 40.0 + r.z * 17.0;\n"
    "    p.x += w.shape.w * sin(tt * 1.3 + ph);\n"
    "    p.z += w.shape.w * cos(tt * 1.1 + ph * 1.7);\n"
    "  }\n"
    "  return p;\n"
    "}\n"
    "bool wx_surface(vec3 p, inout vec3 s) {\n"
    "  float y = w.origin.w, dy = 1e9;\n"
    "  vec2 uv = vec2(0.0);\n"
    "  for (int k = 0; k < 4 && dy > 0.05; k++) {\n"
    "    vec4 c = game_clip(vec3(p.x, y, p.z));\n"
    "    if (c.w <= 0.0) return false;\n"
    "    vec2 m = c.xy / c.w;\n"
    "    if (any(greaterThan(abs(m), vec2(1.0)))) return false;\n"
    "    if (w.flags.x > 0.5) m.y = -m.y;\n"
    "    uv = vec2(m.x * 0.5 + 0.5, 0.5 - m.y * 0.5);\n"
    "    float d = dread(uvec2(uv * (w.screen.zw - 1.0)));\n"
    "    if (d >= 0.99999) return false;\n"
    "    s = world_at(uv, d);\n"
    "    dy = abs(s.y - y);\n"
    "    y = s.y;\n"
    "  }\n"
    "  if (dy > 0.5 || distance(s.xz, p.xz) > 1.0) return false;\n"
    "  vec2 e = 2.0 / w.screen.zw;\n"
    "  uvec2 q = uvec2(uv * (w.screen.zw - 1.0));\n"
    "  vec3 a = world_at(uv + vec2(e.x, 0.0), dread(min(q + uvec2(2u, 0u), uvec2(w.screen.zw - 1.0))));\n"
    "  vec3 b = world_at(uv + vec2(0.0, e.y), dread(min(q + uvec2(0u, 2u), uvec2(w.screen.zw - 1.0))));\n"
    "  vec3 n = cross(a - s, b - s);\n"
    "  return abs(n.y) > 0.6 * length(n);\n"
    "}\n"
    "bool wx_landed(uint iid, vec3 p, float speed, float age, out float since, inout vec3 s) {\n"
    "  float sp, ag;\n"
    "  since = 0.0;\n"
    "  if (wx_surface(p, s) && p.y < s.y) since = (s.y - p.y) / speed;\n"
    "  else {\n"
    "    if (age > w.cam.w) return false;\n"
    "    vec3 q = wx_drop(iid, w.box.z - age - 1e-3, sp, ag);\n"
    "    if (!wx_surface(q, s) || q.y >= s.y) return false;\n"
    "    since = age + 1e-3 + (s.y - q.y) / speed;\n"
    "  }\n"
    "  vec3 q = wx_drop(iid, w.box.z - since, sp, ag), s0 = s;\n"
    "  if (!wx_surface(q, s)) s = vec3(q.x, s0.y, q.z);\n"
    "  return true;\n"
    "}\n"
    "float wx_fade(vec3 p) {\n"
    "  vec2 e = 0.5 * w.box.x - abs(p.xz - w.origin.xz);\n"
    "  return saturate((w.origin.y + 0.5 * w.box.y - p.y) / 1.5) * saturate(min(e.x, e.y) / 2.0);\n"
    "}\n"
    "vec4 wx_flake(uint iid, vec3 p, vec2 corner) {\n"
    "  vec4 c = clip_of(p), ref = clip_of(w.origin.xyz);\n"
    "  float size = w.shape.y * (0.6 + 0.8 * h1(iid * 11u + 3u)) * w.screen.y / 1080.0 * clamp(ref.w / c.w, 0.5, 2.0);\n"
    "  c.xy += corner * size / w.screen.xy * c.w;\n"
    "  return c;\n"
    "}\n"
    "bool wx_hidden(vec4 pos) {\n"
    "  vec2 p = pos.xy;\n"
    "  if (w.flags.z > 0.5) p.x = w.screen.x - p.x;\n"
    "  uvec2 q = uvec2(clamp(p * w.screen.zw / w.screen.xy, vec2(0.0), w.screen.zw - 1.0));\n"
    "  return pos.z > dread(q) + 1e-6;\n"
    "}\n"
    "vec2 corner_of(uint vid) { return vec2((vid & 1u) != 0u ? 1.0 : -1.0, (vid & 2u) != 0u ? 1.0 : -1.0); }\n"
    // a lightning strike (binding 3) and the snow Link kicked up (binding 4)
    "layout(std140, set = 0, binding = 3) uniform SK { vec4 at; vec4 tm; vec4 pts[40]; } k;\n"
    "struct PF { vec4 p; vec4 v; vec4 g; };\n"
    "layout(std140, set = 0, binding = 4) uniform PFS { PF pf[161]; };\n"
    "vec3 sk_ground() {\n"
    "  vec3 s = vec3(0.0);\n"
    "  if (wx_surface(k.at.xyz, s) && abs(s.y - k.at.y) < 3.0) return vec3(k.at.x, s.y, k.at.z);\n"
    "  return k.at.xyz;\n"
    "}\n";

// vertex shaders (each: main), then fragment shaders. VO: uv, a; the mark adds the ground point g
static const char *wx_vs =
    "layout(std430, set = 0, binding = 2) buffer PR { float probe[]; };\n"
    "layout(location = 0) out vec2 o_uv;\n"
    "layout(location = 1) out float o_a;\n"
    "void main() {\n"
    "  uint vid = uint(gl_VertexIndex), iid = uint(gl_InstanceIndex);\n"
    "#ifndef NO_PROBE\n"
    "  if (iid == 0u && vid == 0u) {\n"
    "    const vec2 pu[5] = vec2[5](vec2(0.5, 0.5), vec2(0.3, 0.35), vec2(0.7, 0.35), vec2(0.3, 0.65), vec2(0.7, 0.65));\n"
    "    for (uint j = 0u; j < 5u; j++) probe[j] = dread(uvec2(pu[j] * (w.screen.zw - 1.0)));\n"
    "  }\n"
    "#endif\n"
    "  float speed, age;\n"
    "  vec3 p = wx_drop(iid, w.box.z, speed, age), s = vec3(0.0);\n"
    "  bool test = w.flags.w > 2.5;\n"
    "  o_uv = vec2(0.0); o_a = 0.0;\n"
    "  if (!test && wx_surface(p, s) && p.y < s.y) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; }\n"
    "  float a = w.color.a * wx_fade(p);\n"
    "  float px = w.screen.y / 1080.0;\n"
    "  vec2 corner = corner_of(vid);\n"
    "  if (abs(w.flags.w - 2.0) < 0.5) {\n"
    "    float ws = length(w.wind.zw);\n"
    "    if (ws < 3.0) { gl_Position = wx_flake(iid, p, corner); o_uv = corner; o_a = a; return; }\n"
    "    vec4 fc = wx_flake(iid, p, vec2(1.0, 0.0)), f0 = clip_of(p);\n"
    "    float hw = abs(fc.x / fc.w - f0.x / f0.w) * 0.5 * w.screen.x;\n"
    "    vec3 tail = p - vec3(w.wind.z, -speed, w.wind.w) * 0.035 * saturate((ws - 3.0) / 5.0);\n"
    "    vec4 ch = clip_of(p), ct = clip_of(tail);\n"
    "    vec2 d = ch.xy / ch.w * w.screen.xy - ct.xy / ct.w * w.screen.xy;\n"
    "    d = length(d) > 1e-3 ? normalize(d) : vec2(0.0, 1.0);\n"
    "    vec2 n = vec2(-d.y, d.x) * hw;\n"
    "    vec4 c = corner.y > 0.0 ? ch : ct;\n"
    "    c.xy += (corner.x * n + corner.y * d * hw) / w.screen.xy * c.w;\n"
    "    gl_Position = c; o_uv = vec2(corner.x, corner.y * 0.6); o_a = a;\n"
    "    return;\n"
    "  }\n"
    "  vec3 dir = normalize(vec3(w.wind.z, -speed, w.wind.w));\n"
    "  vec3 tail = p - dir * w.shape.x * (0.7 + 0.6 * h1(iid * 13u + 7u));\n"
    "  if (test) { p.y = w.origin.w; tail = p + vec3(0.0, 6.0, 0.0); }\n"
    "  vec4 ch = clip_of(p), ct = clip_of(tail);\n"
    "  vec2 sh = ch.xy / ch.w * w.screen.xy, st = ct.xy / ct.w * w.screen.xy;\n"
    "  vec2 d = sh - st;\n"
    "  d = length(d) > 1e-3 ? normalize(d) : vec2(0.0, 1.0);\n"
    "  vec2 n = vec2(-d.y, d.x) * w.shape.y * px;\n"
    "  vec4 c = corner.y > 0.0 ? ch : ct;\n"
    "  c.xy += corner.x * n / w.screen.xy * c.w;\n"
    "  gl_Position = c; o_uv = corner; o_a = corner.y > 0.0 ? a : 0.0;\n"
    "}\n";
static const char *wx_fs =
    "layout(location = 0) in vec2 i_uv;\n"
    "layout(location = 1) in float i_a;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  if (wx_hidden(gl_FragCoord)) discard;\n"
    "  float a;\n"
    "  if (abs(w.flags.w - 2.0) < 0.5) { float r = length(i_uv); a = i_a * (1.0 - smoothstep(0.5, 1.0, r)); }\n"
    "  else a = i_a * (1.0 - i_uv.x * i_uv.x);\n"
    "  o = vec4(w.color.rgb, a);\n"
    "}\n";
static const char *splash_vs =
    "layout(location = 0) out vec2 o_uv;\n"
    "layout(location = 1) out float o_a;\n"
    "void main() {\n"
    "  uint vid = uint(gl_VertexIndex);\n"
    "  uint i = uint(gl_InstanceIndex) * max(w.count / max(w.splashes, 1u), 1u);\n"
    "  float speed, age, since;\n"
    "  vec3 p = wx_drop(i, w.box.z, speed, age), s = vec3(0.0);\n"
    "  o_uv = vec2(0.0); o_a = 0.0;\n"
    "  if (!wx_landed(i, p, speed, age, since, s) || since > w.cam.w) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; }\n"
    "  float kk = since / w.cam.w;\n"
    "  vec2 corner = corner_of(vid);\n"
    "  o_uv = corner;\n"
    "  if (abs(w.flags.w - 2.0) < 0.5) {\n"
    "    gl_Position = wx_flake(i, s + normalize(w.cam.xyz - s) * 0.3, corner); o_a = w.color.a * (1.0 - kk);\n"
    "    return;\n"
    "  }\n"
    "  float rad = w.shape.x * 0.12 * (0.3 + kk);\n"
    "  gl_Position = clip_of(s + vec3(corner.x, 0.0, corner.y) * rad);\n"
    "  o_a = w.color.a * 1.4 * (1.0 - kk);\n"
    "}\n";
static const char *splash_fs =
    "layout(location = 0) in vec2 i_uv;\n"
    "layout(location = 1) in float i_a;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  float r = length(i_uv);\n"
    "  if (abs(w.flags.w - 2.0) < 0.5) {\n"
    "    if (wx_hidden(gl_FragCoord)) discard;\n"
    "    o = vec4(w.color.rgb, i_a * (1.0 - smoothstep(0.5, 1.0, r)));\n"
    "    return;\n"
    "  }\n"
    "  float ring = smoothstep(0.45, 0.75, r) * (1.0 - smoothstep(0.8, 1.0, r));\n"
    "  o = vec4(w.color.rgb, i_a * ring);\n"
    "}\n";
static const char *fill_vs =
    "void main() {\n"
    "  uint vid = uint(gl_VertexIndex);\n"
    "  gl_Position = vec4(vid == 1u ? 3.0 : -1.0, vid == 2u ? 3.0 : -1.0, 0.0, 1.0);\n"
    "}\n";
static const char *fill_fs =
    "layout(location = 0) out vec4 o;\n"
    "void main() { o = w.fill; }\n";
static const char *puff_vs =
    "layout(location = 0) out vec2 o_uv;\n"
    "layout(location = 1) out float o_a;\n"
    "void main() {\n"
    "  uint vid = uint(gl_VertexIndex), iid = uint(gl_InstanceIndex);\n"
    "  PF q = pf[iid + 1u];\n"
    "  float age = pf[0].p.x - q.p.w, life = q.g.y;\n"
    "  o_uv = vec2(0.0); o_a = 0.0;\n"
    "  if (q.p.w <= 0.0 || age < 0.0 || age > life) { gl_Position = vec4(0.0, 0.0, -2.0, 1.0); return; }\n"
    "  vec3 p = q.p.xyz + q.v.xyz * age + vec3(0.0, -q.g.z, 0.0) * age * age;\n"
    "  p.y = max(p.y, q.g.x - 0.05);\n"
    "  p += normalize(w.cam.xyz - p) * 0.3;\n"
    "  vec2 corner = corner_of(vid);\n"
    "  vec4 c = clip_of(p);\n"
    "  float size = q.v.w * (0.7 + 0.8 * age / life) * w.screen.y / 1080.0;\n"
    "  c.xy += corner * size / w.screen.xy * c.w;\n"
    "  gl_Position = c; o_uv = corner; o_a = 0.9 * (1.0 - age / life);\n"
    "}\n";
static const char *puff_fs =
    "layout(location = 0) in vec2 i_uv;\n"
    "layout(location = 1) in float i_a;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  if (wx_hidden(gl_FragCoord)) discard;\n"
    "  float r = length(i_uv);\n"
    "  vec3 c = mix(vec3(0.62, 0.68, 0.8), w.color.rgb, saturate(1.2 - r * r));\n"
    "  o = vec4(c, i_a * (1.0 - smoothstep(0.55, 1.0, r)));\n"
    "}\n";
static const char *leaf_vs =
    "layout(location = 0) out vec2 o_uv;\n"
    "layout(location = 1) out float o_a;\n"
    "void main() {\n"
    "  uint vid = uint(gl_VertexIndex), iid = uint(gl_InstanceIndex);\n"
    "  float fall = 0.7 + 0.4 * h1(iid * 5u + 1u), hgt = 2.5 + 1.5 * h1(iid * 5u + 2u), rest = 1.5;\n"
    "  float life = hgt / fall + rest, tt = w.box.z + h1(iid * 5u + 3u) * life;\n"
    "  float cyc = floor(tt / life), a = tt - cyc * life;\n"
    "  uint seed = iid * 7919u + uint(cyc) * 104729u;\n"
    "  float fly = min(a, hgt / fall);\n"
    "  float gust = 1.0 + 0.6 * sin(w.box.z * 0.9 + h1(seed) * 6.28) + 0.4 * sin(w.box.z * 2.3 + h1(seed + 1u) * 6.28);\n"
    "  vec2 drift = w.wind.zw * 0.55 * gust * fly;\n"
    "  float sw = sin(fly * 3.1 + h1(seed + 2u) * 6.28) * 0.5;\n"
    "  vec2 wd = length(w.wind.zw) > 0.01 ? normalize(w.wind.zw) : vec2(1.0, 0.0);\n"
    "  vec3 sz = vec3(w.box.x, 0.0, w.box.x);\n"
    "  vec3 lo = w.origin.xyz - 0.5 * sz;\n"
    "  vec2 xz = vec2(h1(seed + 3u), h1(seed + 4u)) * w.box.x + drift + vec2(-wd.y, wd.x) * sw;\n"
    "  xz = lo.xz + fract((xz - lo.xz) / w.box.x) * w.box.x;\n"
    "  vec3 p = vec3(xz.x, w.origin.w + hgt - fall * fly, xz.y);\n"
    "  float ang = h1(seed + 5u) * 6.28 + fly * (3.0 + 4.0 * h1(seed + 6u)) * (h1(seed + 7u) > 0.5 ? 1.0 : -1.0);\n"
    "  float flip = a < hgt / fall ? cos(fly * (4.0 + 3.0 * h1(seed + 8u))) : 1.0;\n"
    "  vec2 corner = corner_of(vid);\n"
    "  vec2 cr = vec2(corner.x * max(abs(flip), 0.15), corner.y);\n"
    "  cr = vec2(cr.x * cos(ang) - cr.y * sin(ang), cr.x * sin(ang) + cr.y * cos(ang));\n"
    "  vec4 c = clip_of(p), ref = clip_of(w.origin.xyz);\n"
    "  float size = 30.0 * (0.8 + 0.4 * h1(seed + 9u)) * w.screen.y / 1080.0 * clamp(ref.w / c.w, 0.5, 2.0);\n"
    "  c.xy += cr * size / w.screen.xy * c.w;\n"
    "  gl_Position = c; o_uv = corner * 0.5 + 0.5;\n"
    "  o_a = (1.0 - smoothstep(life - rest, life, a)) * smoothstep(0.0, 0.4, a) * wx_fade(p);\n"
    "}\n";
static const char *leaf_fs =
    "layout(location = 0) in vec2 i_uv;\n"
    "layout(location = 1) in float i_a;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  if (wx_hidden(gl_FragCoord)) discard;\n"
    "  vec2 q = i_uv * 2.0 - 1.0;\n"
    "  float y = q.y * 1.1 + 0.05;\n"
    "  float hw = 0.62 * sqrt(saturate(1.0 - y * y)) * (1.0 - 0.45 * saturate(y));\n"
    "  float edge = hw - abs(q.x);\n"
    "  float px = fwidth(q.x) * 1.5;\n"
    "  float a = smoothstep(-px, px, edge);\n"
    "  a = max(a, (1.0 - smoothstep(0.03, 0.03 + px, abs(q.x))) * step(-1.0, q.y) * step(q.y, -0.85));\n"
    "  if (a <= 0.0) discard;\n"
    "  vec3 col = w.leaf.r < 0.0 ? vec3(0.30, 0.55, 0.16) : w.leaf.rgb;\n"
    "  float side = q.x > 0.0 ? 1.0 : 0.72;\n"
    "  float rib = 1.0 - 0.35 * (1.0 - smoothstep(0.0, 0.05 + px, abs(q.x))) * step(q.y, 0.85);\n"
    "  float veins = 1.0 - 0.12 * smoothstep(0.85, 1.0, sin((q.y - abs(q.x) * 1.2) * 18.0)) * step(0.06, abs(q.x));\n"
    "  float rim = mix(0.8, 1.0, smoothstep(0.0, 0.18, edge));\n"
    "  o = vec4(col * side * rib * veins * rim, a * i_a);\n"
    "}\n";
static const char *mark_vs =
    "layout(location = 0) out vec2 o_uv;\n"
    "layout(location = 1) out float o_a;\n"
    "layout(location = 2) out vec3 o_g;\n"
    "void main() {\n"
    "  o_g = sk_ground();\n"
    "  vec2 corner = corner_of(uint(gl_VertexIndex));\n"
    "  gl_Position = clip_of(o_g + vec3(corner.x, 0.0, corner.y) * 2.2);\n"
    "  o_uv = corner; o_a = 1.0;\n"
    "}\n";
static const char *mark_fs =
    "layout(location = 0) in vec2 i_uv;\n"
    "layout(location = 1) in float i_a;\n"
    "layout(location = 2) in vec3 i_g;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  vec2 p = gl_FragCoord.xy;\n"
    "  if (w.flags.z > 0.5) p.x = w.screen.x - p.x;\n"
    "  uvec2 q = uvec2(clamp(p * w.screen.zw / w.screen.xy, vec2(0.0), w.screen.zw - 1.0));\n"
    "  float d = dread(q);\n"
    "  if (d >= 0.99999) discard;\n"
    "  vec3 s = world_at(p / w.screen.xy, d);\n"
    "  float dy = s.y - i_g.y;\n"
    "  if (dy > 0.4 || dy < -1.2) discard;\n"
    "  vec2 v = s.xz - i_g.xz;\n"
    "  float r = length(v), an = atan(v.y, v.x), now = k.at.w;\n"
    "  vec3 rgb = vec3(0.0); float a = 0.0, keep = 1.0 - smoothstep(0.15, 0.4, dy) * 1.0;\n"
    "  if (now < k.tm.y) {\n"
    "    float ph = saturate((now - k.tm.x) / max(k.tm.y - k.tm.x, 1e-3));\n"
    "    float fl = 0.6 + 0.4 * h1(uint(now * 40.0) + 7u);\n"
    "    float g = (1.0 - smoothstep(0.0, 0.25 + 0.65 * ph, r)) * (0.25 + 0.75 * ph) * fl;\n"
    "    float arc = pow(saturate(sin(an * 5.0 + h1(uint(now * 12.0)) * 40.0)), 24.0) * (1.0 - smoothstep(0.2, 0.6 + ph, r));\n"
    "    rgb = vec3(0.55, 0.75, 1.0) * (g * 1.5 + arc * 1.0 * fl);\n"
    "  } else {\n"
    "    float age = now - k.tm.y;\n"
    "    float ab = (an + 3.1416) * 2.0, af = fract(ab);\n"
    "    uint ai = uint(ab) % 13u, aj = (ai + 1u) % 13u;\n"
    "    float jag = 0.8 + 0.4 * mix(h1(ai + uint(k.tm.z)), h1(aj + uint(k.tm.z)), smoothstep(0.0, 1.0, af));\n"
    "    float rr = r / (1.45 * jag);\n"
    "    a = (1.0 - smoothstep(0.35, 1.0, rr)) * 0.85 * (1.0 - smoothstep(5.0, 12.0, age));\n"
    "    rgb = vec3(0.03, 0.025, 0.02) * a;\n"
    "    rgb += vec3(1.0, 0.5, 0.15) * (1.0 - smoothstep(0.0, 0.45, rr)) * exp(-age * 2.0) * (0.7 + 0.3 * h1(uint(now * 20.0)));\n"
    "    rgb += vec3(0.75, 0.85, 1.0) * (1.0 - smoothstep(0.0, 1.6, r)) * exp(-age * 9.0);\n"
    "  }\n"
    "  o = vec4(rgb, a) * keep;\n"
    "}\n";
static const char *spark_vs =
    "layout(location = 0) out vec2 o_uv;\n"
    "layout(location = 1) out float o_a;\n"
    "void main() {\n"
    "  uint vid = uint(gl_VertexIndex), iid = uint(gl_InstanceIndex);\n"
    "  o_uv = vec2(0.0); o_a = 0.0; gl_Position = vec4(0.0, 0.0, -2.0, 1.0);\n"
    "  vec3 g = sk_ground(), p;\n"
    "  float now = k.at.w, size;\n"
    "  uint sd = uint(k.tm.z) + iid * 977u;\n"
    "  if (iid < 16u) {\n"
    "    if (now >= k.tm.y) return;\n"
    "    float c = (now - k.tm.x) * 9.0 + h1(sd);\n"
    "    uint n = uint(c);\n"
    "    float f = fract(c), an = h1(sd + n * 31u) * 6.2832, r = 0.15 + 0.8 * h1(sd + n * 17u);\n"
    "    p = g + vec3(cos(an) * r, 0.05 + 0.3 * f * h1(sd + n * 5u), sin(an) * r);\n"
    "    o_a = (1.0 - f) * saturate((now - k.tm.x) * 3.0);\n"
    "    size = 3.0;\n"
    "  } else {\n"
    "    float age = now - k.tm.y, life = 0.4 + 0.5 * h1(sd + 1u);\n"
    "    if (age < 0.0 || age > life) return;\n"
    "    float an = h1(sd + 2u) * 6.2832, sp = 2.0 + 4.0 * h1(sd + 3u);\n"
    "    vec3 v = vec3(cos(an) * sp, 2.0 + 4.0 * h1(sd + 4u), sin(an) * sp);\n"
    "    p = g + v * age + vec3(0.0, -7.0, 0.0) * age * age;\n"
    "    p.y = max(p.y, g.y + 0.03);\n"
    "    o_a = 1.0 - age / life;\n"
    "    size = 4.0;\n"
    "  }\n"
    "  p += normalize(w.cam.xyz - p) * 0.3;\n"
    "  vec2 corner = corner_of(vid);\n"
    "  vec4 c = clip_of(p);\n"
    "  c.xy += corner * size * w.screen.y / 1080.0 / w.screen.xy * c.w;\n"
    "  gl_Position = c; o_uv = corner;\n"
    "}\n";
static const char *spark_fs =
    "layout(location = 0) in vec2 i_uv;\n"
    "layout(location = 1) in float i_a;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  float r = length(i_uv), e = (1.0 - smoothstep(0.2, 1.0, r)) * i_a;\n"
    "  o = vec4(mix(vec3(1.0, 0.6, 0.25), vec3(1.0, 0.95, 0.85), saturate(1.0 - r * 1.5)) * e, 0.0);\n"
    "}\n";
static const char *bolt_vs =
    "layout(location = 0) out vec2 o_uv;\n"
    "layout(location = 1) out float o_a;\n"
    "void main() {\n"
    "  uint vid = uint(gl_VertexIndex), iid = uint(gl_InstanceIndex);\n"
    "  o_uv = vec2(0.0); o_a = 0.0; gl_Position = vec4(0.0, 0.0, -2.0, 1.0);\n"
    "  float age = k.at.w - k.tm.y;\n"
    "  if (age < 0.0 || age > 0.6 || float(iid + 1u) >= k.tm.w) return;\n"
    "  vec4 p0 = k.pts[iid], p1 = k.pts[iid + 1u];\n"
    "  if (p0.w <= 0.0 || p1.w <= 0.0) return;\n"
    "  vec3 g = sk_ground();\n"
    "  vec4 c0 = clip_of(g + p0.xyz), c1 = clip_of(g + p1.xyz);\n"
    "  if (c0.w <= 0.0 || c1.w <= 0.0) return;\n"
    "  vec2 s0 = c0.xy / c0.w * w.screen.xy, s1 = c1.xy / c1.w * w.screen.xy;\n"
    "  vec2 dir = normalize(s1 - s0 + vec2(1e-4, 0.0)), nrm = vec2(-dir.y, dir.x);\n"
    "  bool end = (vid & 1u) != 0u;\n"
    "  vec4 c = end ? c1 : c0;\n"
    "  float wd = (end ? p1.w : p0.w) * 6.0 * w.screen.y / 1080.0;\n"
    "  float side = (vid & 2u) != 0u ? 1.0 : -1.0;\n"
    "  c.xy += (nrm * side * wd + dir * (end ? wd : -wd) * 0.5) / w.screen.xy * c.w;\n"
    "  gl_Position = c; o_uv = vec2(side, 0.0);\n"
    "  o_a = age < 0.07 ? 1.0 : age < 0.12 ? 0.25 : age < 0.2 ? 1.0 : exp(-(age - 0.2) * 9.0);\n"
    "}\n";
static const char *bolt_fs =
    "layout(location = 0) in vec2 i_uv;\n"
    "layout(location = 1) in float i_a;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  float x = abs(i_uv.x);\n"
    "  float core = 1.0 - smoothstep(0.18, 0.33, x), glow = exp(-x * x * 9.0);\n"
    "  o = vec4((vec3(1.0) * core + vec3(0.45, 0.6, 1.0) * glow * 0.8) * i_a, 0.0);\n"
    "}\n";

enum { P_DROPS, P_SPLASH, P_FILL, P_PUFF, P_LEAF, P_MARK, P_SPARK, P_BOLT, NPIPE };
static const char *const *const kSrc[NPIPE][2] = {{&wx_vs, &wx_fs}, {&splash_vs, &splash_fs}, {&fill_vs, &fill_fs},
                                                  {&puff_vs, &puff_fs}, {&leaf_vs, &leaf_fs}, {&mark_vs, &mark_fs},
                                                  {&spark_vs, &spark_fs}, {&bolt_vs, &bolt_fs}};
static VkShaderModule g_mod[NPIPE][2];
static VkPipeline g_pipe[NPIPE];
static VkFormat g_pipe_fmt;
static VkDescriptorSetLayout g_set_layout;
static VkPipelineLayout g_layout;
static VkBuffer g_probe;
static VmaAllocation g_probe_mem;
static float *g_probe_ptr;
static int g_ok = -1;

static VkShaderModule compile(int i, int stage, bool no_probe) {
    const char *body = *kSrc[i][stage == STAGE_VERTEX ? 0 : 1];
    size_t n = strlen(WX_PRELUDE) + strlen(wx_common) + strlen(body) + 64;
    char *src = malloc(n);
    snprintf(src, n, "%s%s%s%s", WX_PRELUDE, no_probe ? "#define NO_PROBE\n" : "", wx_common, body);
    char name[32];
    snprintf(name, sizeof name, "weather%d.%s", i, stage == STAGE_VERTEX ? "vert" : "frag");
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    free(src);
    return m;
}

// Once: shaders, the descriptor layout (0 the frame, 1 the scene depth, 2 the probes, 3 a strike, 4 the kicked-up
// snow), the probe buffer (host-visible: the CPU reads the last frame's values)
static bool init(void) {
    if (g_ok >= 0) return g_ok;
    g_ok = 0;
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(g_vk.phys, &f);
    bool no_probe = !f.vertexPipelineStoresAndAtomics;  // (no probes: weather.c keeps its last ground height)
    for (int i = 0; i < NPIPE; i++) {
        g_mod[i][0] = compile(i, STAGE_VERTEX, no_probe);
        g_mod[i][1] = compile(i, STAGE_FRAGMENT, no_probe);
        if (!g_mod[i][0] || !g_mod[i][1]) { LOG("[weather] vk: shaders for pass %d failed\n", i); return false; }
    }
    VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[5] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vf, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, vf, NULL},
                                         {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vf, NULL},
                                         {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vf, NULL},
                                         {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vf, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &g_set_layout));
    VkPipelineLayoutCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_set_layout};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pci, NULL, &g_layout));
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &g_probe, &g_probe_mem, &info) != VK_SUCCESS) return false;
    g_probe_ptr = info.pMappedData;
    memset(g_probe_ptr, 0, 64);
    g_ok = 1;
    return true;
}

// by the window image's format: particles etc. blend over the picture (alpha kept); the fill is rgb + picture * a,
// the lightning (mark, sparks, bolt) premultiplied
static bool pipelines(VkFormat fmt) {
    if (g_pipe_fmt == fmt && g_pipe[0]) return true;
    for (int i = 0; i < NPIPE; i++) if (g_pipe[i]) vkDestroyPipeline(g_vk.dev, g_pipe[i], NULL), g_pipe[i] = VK_NULL_HANDLE;
    g_pipe_fmt = fmt;
    for (int i = 0; i < NPIPE; i++) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_mod[i][0], .pName = "main"},
            {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g_mod[i][1], .pName = "main"}};
        VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                     .topology = i == P_FILL ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
        VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineColorBlendAttachmentState cba = {.blendEnable = VK_TRUE, .colorWriteMask = 0xF,
                                                   .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                   .colorBlendOp = VK_BLEND_OP_ADD, .alphaBlendOp = VK_BLEND_OP_ADD};
        cba.srcColorBlendFactor = i == P_FILL || i >= P_MARK ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = i == P_FILL ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                                  .pAttachments = &cba};
        VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
        VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                               .dynamicStateCount = g_vk.vids ? 3u : 2u, .pDynamicStates = dyn};
        VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                            .pColorAttachmentFormats = &fmt};
        VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                           .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                           .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                           .pColorBlendState = &cb, .pDynamicState = &dy, .layout = g_layout};
        if (vk_create_graphics_pipeline(&pi, &g_pipe[i]) != VK_SUCCESS) {
            LOG("[weather] vk: pipeline %d failed\n", i);
            g_pipe_fmt = VK_FORMAT_UNDEFINED;
            return false;
        }
    }
    return true;
}

// The scene depth at the probe points as the last frame's particle draw wrote it (NULL: none yet)
bool vk_weather_probes(float out[WEATHER_PROBES]) {
    if (!init() || !g_probe_ptr) return false;
    vmaInvalidateAllocation(g_vk.vma, g_probe_mem, 0, VK_WHOLE_SIZE);
    memcpy(out, g_probe_ptr, WEATHER_PROBES * sizeof(float));
    return true;
}

// The pass, in the frame's command buffer outside any rendering (exec.c ended the game's): color: the window image
// (fmt, the area w x h drawn), depth: the scene depth's sampled view. parts: LARECOMP_WEATHER_PARTS' mask (renderer.m)
void vk_weather_pass(VkImageView color, VkFormat fmt, uint32_t w, uint32_t h, VkImageView depth, const WeatherFrame *wf,
                     int parts, bool dbg) {
    if (!init() || !pipelines(fmt)) return;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // (the composite's colour and the scene depth, written just before)
    StrikeFrame sk;
    bool strike = wf->kind == WEATHER_STORM && !dbg && (parts & 32) && weather_strike(&sk);
    if (!strike) memset(&sk, 0, sizeof sk);
    bool puffs = (wf->kind == WEATHER_SNOW || wf->kind == WEATHER_BLIZZARD) && !dbg && (parts & 16);
    VkSpan pf = vk_transient(sizeof(SnowPuff) * (SNOW_PUFFS + 1), 256);
    memset(pf.ptr, 0, sizeof(SnowPuff));
    if (puffs) {
        SnowPuff *p = pf.ptr;
        weather_snow_puffs(p + 1);
        p[0].p[0] = weather_snow_now();
    } else memset(pf.ptr, 0, sizeof(SnowPuff) * (SNOW_PUFFS + 1));
    VkSpan fs = vk_transient_copy(wf, sizeof *wf, 256), ks = vk_transient_copy(&sk, sizeof sk, 256);
    VkDescriptorSet set = vk_alloc_set(g_set_layout);
    VkDescriptorBufferInfo bf = {fs.buf, fs.off, sizeof *wf}, bp = {g_probe, 0, 64}, bk = {ks.buf, ks.off, sizeof sk},
                           bpf = {pf.buf, pf.off, sizeof(SnowPuff) * (SNOW_PUFFS + 1)};
    VkDescriptorImageInfo id = {vk_default_sampler(false), depth, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[5] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bf},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &id},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bp},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bk},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bpf}};
    rec_UpdateDescriptorSets(5, wr);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = color,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    VkViewport vp = {0, (float)h, (float)w, -(float)h, 0, 1};  // (NDC y up: the game's own, the header says why)
    VkRect2D sc = {{0, 0}, {w, h}};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    if (g_vk.vids) {
        VkVertexInputBindingDescription2EXT vb = {.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
        VkVertexInputAttributeDescription2EXT va = {.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
        rec_CmdSetVertexInputEXT(cb, 0, &vb, 0, &va);
    }
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &set, 0, NULL);
#define DRAW(p, nv, ni) (rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe[p]), rec_CmdDraw(cb, nv, ni, 0, 0))
    if ((parts & 1) && (wf->fill[3] < 1 || wf->fill[0] > 0)) DRAW(P_FILL, 3, 1);  // (overcast, lightning)
    DRAW(P_DROPS, 4, (parts & 2) ? wf->count : 1);  // (one drop at least: its first vertex writes the probes)
    if (wf->splashes && (parts & 4)) DRAW(P_SPLASH, 4, wf->splashes);
    if (wf->leaf[3] > 0 && !dbg && (parts & 8)) DRAW(P_LEAF, 4, (uint32_t)wf->leaf[3]);
    if (strike) DRAW(P_MARK, 4, 1), DRAW(P_SPARK, 4, 48), DRAW(P_BOLT, 4, STRIKE_PTS - 1);
    if (puffs) DRAW(P_PUFF, 4, SNOW_PUFFS);
#undef DRAW
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
}
