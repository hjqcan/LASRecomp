// The snow on the ground on Vulkan (runtime/nvn/weather.h; renderer.m's snow_cover_encode is the Metal side, and the
// why): mid-scene, right before the game's first blended draw into its scene image - the new footprints into the trail
// map, the prep and half-resolution analysis, then the cover over the scene image. The shaders are renderer.m's sc_src
// in GLSL, the same math. Differences: Vulkan has no framebuffer fetch, so the scene image's drawn area is copied first
// and the passes read the copy; the scene stencil (the marks exec.c's draws write, bits 7 / 6 / 1-3) is read through
// a stencil view. The game's draws go through a negative-height viewport (exec.c's set_viewport: Metal's convention),
// so the scene image's rows are Metal's and the shaders keep renderer.m's pixel <-> NDC y (1 - uv.y * 2) - not the
// weather pass' (weather_vk.c: its own pass on the window image, y down).
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include "record.h"
#include "../weather.h"

#define SC_PRELUDE                                                                                                     \
    "#version 450\n"                                                                                                   \
    "layout(std140, set = 0, binding = 0) uniform SF { vec4 vp[4]; vec4 ivp[4]; vec4 screen, flags, zr, trail, tone; " \
    "vec4 shake4[2]; } w;\n"                                                                                           \
    "layout(set = 0, binding = 1) uniform sampler2D dt;\n"                                                             \
    "layout(set = 0, binding = 2) uniform usampler2D sv;\n"                                                            \
    "layout(set = 0, binding = 3) uniform sampler2D ct;\n"                                                             \
    "layout(set = 0, binding = 4) uniform sampler2D hf;\n"                                                             \
    "layout(set = 0, binding = 5) uniform sampler2D pp;\n"                                                             \
    "layout(set = 0, binding = 6) uniform sampler2D tr;\n"                                                             \
    "layout(std140, set = 0, binding = 7) uniform SP { vec4 pr[128]; } prints;\n"                                      \
    "#define saturate(x) clamp(x, 0.0, 1.0)\n"

static const char *sc_common =
    "vec3 world_at(vec2 uv, float d) {\n"
    "  vec2 m = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"  // (the scene's rows: Metal's, above)
    "  if (w.flags.x > 0.5) m.y = -m.y;\n"
    "  float z = (d - w.zr.x) / w.zr.y;\n"
    "  if (w.flags.y > 0.5) z = z * 2.0 - 1.0;\n"
    "  vec4 q = vec4(m, z, 1.0);\n"
    "  vec4 wp = vec4(dot(w.ivp[0], q), dot(w.ivp[1], q), dot(w.ivp[2], q), dot(w.ivp[3], q));\n"
    "  return wp.xyz / wp.w;\n"
    "}\n"
    "float sc_eye(vec2 uv, float d) {\n"
    "  vec2 m = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);\n"
    "  if (w.flags.x > 0.5) m.y = -m.y;\n"
    "  float z = (d - w.zr.x) / w.zr.y;\n"
    "  if (w.flags.y > 0.5) z = z * 2.0 - 1.0;\n"
    "  return 1.0 / dot(w.ivp[3], vec4(m, z, 1.0));\n"
    "}\n"
    "float hash2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
    "float vnoise(vec2 p) {\n"
    "  vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);\n"
    "  return mix(mix(hash2(i), hash2(i + vec2(1.0, 0.0)), u.x), mix(hash2(i + vec2(0.0, 1.0)), hash2(i + vec2(1.0, 1.0)), u.x), u.y);\n"
    "}\n"
    "float dread(ivec2 q) { return texelFetch(dt, q, 0).r; }\n"
    "uint sread(ivec2 q) { return texelFetch(sv, q, 0).r; }\n"
    "float shake(uint i) { return w.shake4[i >> 2u][i & 3u]; }\n"
    "vec4 sc_at(ivec2 qq) {\n"
    "  ivec2 g = ivec2(round(vec2(qq) * w.screen.xy / w.screen.zw / w.trail.z));\n"
    "  return texelFetch(pp, clamp(g, ivec2(0), textureSize(pp, 0) - 1), 0);\n"
    "}\n"
    "float sc_flat(ivec2 q, ivec2 mx, int step) {\n"
    "  float yl = 1e9, yp = 1e9;\n"
    "  int jl = 0;\n"
    "  for (int j = 1; j <= 4 && yl > 1e8; j++) {\n"
    "    vec4 a = sc_at(clamp(q + ivec2(0, step * j), ivec2(0), mx));\n"
    "    float y = a.w < 0.5 ? a.y : 1e9;\n"
    "    if (y < 1e8 && abs(y - yp) < 0.03) { yl = min(y, yp); jl = j; }\n"
    "    yp = y;\n"
    "  }\n"
    "  if (yl > 1e8) return 1e9;\n"
    "  for (int j = 2; j <= 4; j += 2) {\n"
    "    vec4 a = sc_at(clamp(q + ivec2(0, step * jl * j), ivec2(0), mx));\n"
    "    float y = a.w < 1.5 ? a.y : 1e9;\n"
    "    if (abs(y - yl) > 0.07) return 1e9;\n"
    "  }\n"
    "  return yl;\n"
    "}\n"
    "float sc_print(vec3 p) {\n"
    "  float N = w.zr.w, P = w.zr.z;\n"
    "  vec2 a = p.xz / P * N - 0.5, i0 = floor(a), f = a - i0;\n"
    "  float s = 0.0;\n"
    "  for (int k = 0; k < 4; k++) {\n"
    "    vec2 A = i0 + vec2(float(k & 1), float(k >> 1));\n"
    "    vec2 cell = floor(A / N), t = A - cell * N;\n"
    "    vec4 v = texelFetch(tr, ivec2(t), 0);\n"
    "    float wt = ((k & 1) != 0 ? f.x : 1.0 - f.x) * ((k >> 1) != 0 ? f.y : 1.0 - f.y);\n"
    "    if (v.x > 0.0 && all(equal(v.zw, cell)) && abs(v.y - p.y) < 0.6) s += wt * exp(-max(w.flags.w - v.x, 0.0) / w.trail.x);\n"
    "  }\n"
    "  return s;\n"
    "}\n";

// a new footprint into the trail map: 4 copies of its quad (the map wraps); its shape a sole and a heel
static const char *stamp_vs =
    "layout(location = 0) out vec2 o_u;\n"
    "layout(location = 1) out vec3 o_wp;\n"
    "layout(location = 2) out float o_t;\n"
    "void main() {\n"
    "  uint vid = uint(gl_VertexIndex), iid = uint(gl_InstanceIndex);\n"
    "  vec4 sa = prints.pr[(iid >> 2u) * 2u], sb = prints.pr[(iid >> 2u) * 2u + 1u];\n"
    "  uint c = iid & 3u;\n"
    "  vec2 corner = vec2((vid & 1u) != 0u ? 1.0 : -1.0, (vid & 2u) != 0u ? 1.0 : -1.0) * 1.2;\n"
    "  vec2 fwd = vec2(sin(sb.x), cos(sb.x)), rt = vec2(fwd.y, -fwd.x);\n"
    "  vec2 wxz = sa.xz + corner.x * rt * sb.w * 0.5 + corner.y * fwd * sb.z * 0.5;\n"
    "  float N = w.zr.w, P = w.zr.z;\n"
    "  vec2 ctr = (sa.xz / P - floor(sa.xz / P)) * N;\n"
    "  vec2 sh = vec2((c & 1u) != 0u ? (ctr.x > 0.5 * N ? -N : N) : 0.0, (c & 2u) != 0u ? (ctr.y > 0.5 * N ? -N : N) : 0.0);\n"
    "  vec2 tx = (wxz / P - floor(sa.xz / P)) * N + sh;\n"
    "  gl_Position = vec4(tx.x / N * 2.0 - 1.0, tx.y / N * 2.0 - 1.0, 0.0, 1.0);\n"  // (Vulkan: row tx.y)
    "  o_u = corner; o_wp = vec3(wxz.x, sa.y, wxz.y); o_t = sa.w;\n"
    "}\n";
static const char *stamp_fs =
    "layout(location = 0) in vec2 i_u;\n"
    "layout(location = 1) in vec3 i_wp;\n"
    "layout(location = 2) in float i_t;\n"
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  vec2 a = (i_u - vec2(0.0, 0.3)) / vec2(0.95, 0.7), b = (i_u - vec2(0.0, -0.6)) / vec2(0.75, 0.42);\n"
    "  if (dot(a, a) > 1.0 && dot(b, b) > 1.0) discard;\n"
    "  o = vec4(i_t, i_wp.y, floor(i_wp.xz / w.zr.z));\n"
    "}\n";
static const char *full_vs =
    "void main() {\n"
    "  gl_Position = vec4(gl_VertexIndex == 1 ? 3.0 : -1.0, gl_VertexIndex == 2 ? 3.0 : -1.0, 0.0, 1.0);\n"
    "}\n";

// per analysis cell: its world point and what is there (w: 0 the level's model, 1 a character / grass / face tree, 2 sky)
static const char *prep_fs =
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  ivec2 q = clamp(ivec2(floor(gl_FragCoord.xy) * w.trail.z * w.screen.zw / w.screen.xy), ivec2(0), ivec2(w.screen.zw) - 1);\n"
    "  float d = dread(q);\n"
    "  if (d >= 0.99999) { o = vec4(0.0, 1e9, 0.0, 2.0); return; }\n"
    "  uint m = sread(q);\n"
    "  o = vec4(world_at((vec2(q) + 0.5) / w.screen.zw, d), ((m & 0xC0u) != 0u || ((m >> 1) & 7u) == 7u) ? 1.0 : 0.0);\n"
    "}\n";

// the half-resolution analysis (renderer.m's sc_half_fs: what each step is for)
static const char *half_fs =
    "layout(location = 0) out vec4 o;\n"
    "const vec2 sc_hex[6] = vec2[6](vec2(1.0, 0.0), vec2(0.5, 0.866), vec2(-0.5, 0.866), vec2(-1.0, 0.0), vec2(-0.5, -0.866), vec2(0.5, -0.866));\n"
    "const vec2 sc_ring[8] = vec2[8](vec2(0.924, 0.383), vec2(0.383, 0.924), vec2(-0.383, 0.924), vec2(-0.924, 0.383),\n"
    "                                vec2(-0.924, -0.383), vec2(-0.383, -0.924), vec2(0.383, -0.924), vec2(0.924, -0.383));\n"
    "void main() {\n"
    "  ivec2 mx = ivec2(w.screen.zw) - 1;\n"
    "  ivec2 q = clamp(ivec2(floor(gl_FragCoord.xy) * w.trail.z * w.screen.zw / w.screen.xy), ivec2(0), mx);\n"
    "  vec2 e = 1.0 / w.screen.zw;\n"
    "  float d = dread(q);\n"
    "  vec4 none = vec4(0.0, 1.0, 0.0, d < 0.99999 ? sc_eye((vec2(q) + 0.5) * e, d) : 1e6);\n"
    "  if (d >= 0.99999 || (sread(q) & 0xC0u) != 0u) { o = none; return; }\n"
    "  float rad = max(2.0, round(6.0 * w.screen.w / 1080.0));\n"
    "  vec3 p = world_at((vec2(q) + 0.5) * e, d), v[6];\n"
    "  float lim = 3.0 * length(world_at((vec2(q) + vec2(0.5 + rad, 0.5)) * e, d) - p);\n"
    "  bool ok[6];\n"
    "  int nok = 0;\n"
    "  for (int i = 0; i < 6; i++) {\n"
    "    ivec2 of = ivec2(round(sc_hex[i] * rad));\n"
    "    ivec2 qq = clamp(q + of, ivec2(0), mx);\n"
    "    float dk = dread(qq);\n"
    "    v[i] = world_at((vec2(qq) + 0.5) * e, dk) - p;\n"
    "    ok[i] = dk < 0.99999 && length(v[i]) < lim && any(notEqual(of, ivec2(0)));\n"
    "    nok += ok[i] ? 1 : 0;\n"
    "  }\n"
    "  vec3 ns = vec3(0.0);\n"
    "  float nl = 0.0;\n"
    "  for (int i = 0; i < 6; i++) {\n"
    "    int j = i == 5 ? 0 : i + 1;\n"
    "    if (!ok[i] || !ok[j]) continue;\n"
    "    vec3 c = cross(v[i], v[j]);\n"
    "    ns += c; nl += length(c);\n"
    "  }\n"
    "  if (nok < 2 || nl <= 0.0 || length(ns) < 1e-9) { o = none; return; }\n"
    "  vec3 n = normalize(ns);\n"
    "  float flat_ = length(ns) / nl;\n"
    "  vec4 cq = vec4(-w.ivp[0].z + w.ivp[0].w, -w.ivp[1].z + w.ivp[1].w, -w.ivp[2].z + w.ivp[2].w, -w.ivp[3].z + w.ivp[3].w);\n"
    "  if (dot(n, cq.xyz / cq.w - p) < 0.0) n = -n;\n"
    "  float rough = saturate((0.9 - flat_) / 0.6);\n"
    "  float up = smoothstep(0.35, 0.85, n.y) * (1.0 - 0.45 * rough) * mix(0.6, 1.0, smoothstep(2.0, 4.5, float(nok)));\n"
    "  int down = int(rad * 1.5) * (w.flags.x > 0.5 ? -1 : 1);\n"  // (the depth's rows run up the screen when flipped)
    "  if (up < 0.999) {\n"
    "    float dh = p.y - sc_flat(q, mx, down);\n"
    "    up = max(up, smoothstep(-0.06, -0.02, dh) * (1.0 - smoothstep(0.05, 0.15, dh)));\n"
    "  }\n"
    "  if (up < 0.999) {\n"
    "    float dt2 = sc_flat(q, mx, -down) - p.y;\n"
    "    up = max(up, smoothstep(-0.04, 0.0, dt2) * (1.0 - smoothstep(0.06, 0.12, dt2)));\n"
    "  }\n"
    "  vec3 c = texelFetch(ct, clamp(ivec2(vec2(q) * w.screen.xy / w.screen.zw), ivec2(0), ivec2(w.screen.xy) - 1), 0).rgb;\n"
    "  float mc = max(c.r, max(c.g, c.b));\n"
    "  float cover = w.flags.z;\n"
    "  vec2 pr = vec2(p.x * 0.8 + p.z * 0.6, p.z * 0.8 - p.x * 0.6);\n"
    "  float nz = vnoise(pr * 0.4) * 0.65 + vnoise(pr * 1.3) * 0.35;\n"
    "  float patch_ = smoothstep(0.0, 1.0, saturate((cover * 1.45 - nz) / 0.25));\n"
    "  float dust = 0.35 * smoothstep(0.0, 0.4, cover) * (0.6 + 0.4 * vnoise(pr * 2.5));\n"
    "  float k = up * max(patch_ * (0.45 + 0.55 * smoothstep(0.0, 0.85, cover)), dust);\n"
    "  if (k <= 0.0) { o = none; return; }\n"
    "  float above = 0.0, rise = 0.0;\n"
    "  int valid = 0;\n"
    "  for (int i = 0; i < 8; i++) {\n"
    "    vec4 a = sc_at(clamp(q + ivec2(round(sc_ring[i] * rad * 8.0)), ivec2(0), mx));\n"
    "    if (a.w > 0.5) continue;\n"
    "    valid++;\n"
    "    vec3 vq = a.xyz - p;\n"
    "    above += smoothstep(0.2, 0.4, dot(vq, n));\n"
    "    rise += smoothstep(0.5, 0.9, vq.y);\n"
    "  }\n"
    "  if (valid >= 3) {\n"
    "    float hem = above / float(valid);\n"
    "    k *= 1.0 - max(smoothstep(0.45, 0.62, hem) * (1.0 - smoothstep(0.03, 0.05, mc)), smoothstep(0.75, 0.9, rise / float(valid)));\n"
    "  }\n"
    "  if (k <= 0.0) { o = none; return; }\n"
    "  float side[3];\n"
    "  ivec2 dirs[3] = ivec2[3](ivec2(-1, 0), ivec2(1, 0), ivec2(0, w.flags.x > 0.5 ? 1 : -1));\n"  // (behind: up the screen)
    "  for (int s = 0; s < 3; s++) {\n"
    "    side[s] = 0.0;\n"
    "    for (int j = 3; j <= 15; j += 4) {\n"
    "      vec4 a = sc_at(clamp(q + dirs[s] * int(rad * float(j)), ivec2(0), mx));\n"
    "      if (a.w > 0.5) continue;\n"
    "      side[s] = max(side[s], smoothstep(0.4, 0.7, a.y - p.y));\n"
    "    }\n"
    "  }\n"
    "  k *= 1.0 - min(side[0], min(side[1], side[2]));\n"
    "  if (k <= 0.0) { o = none; return; }\n"
    "  o = vec4(k, n.y, rough, sc_eye((vec2(q) + 0.5) * e, d));\n"
    "}\n";

// the cover at full resolution (renderer.m's sc_fs); dst: the scene image's copy
static const char *cover_fs =
    "layout(location = 0) out vec4 o;\n"
    "void main() {\n"
    "  vec4 dst = texelFetch(ct, ivec2(gl_FragCoord.xy), 0);\n"
    "  vec2 uv = gl_FragCoord.xy / w.screen.xy;\n"
    "  ivec2 mx = ivec2(w.screen.zw) - 1;\n"
    "  ivec2 q = clamp(ivec2(uv * w.screen.zw), ivec2(0), mx);\n"
    "  uint sm = sread(q);\n"
    "  if (abs(w.trail.y - 2.0) < 0.5) { o = vec4((sm & 0x80u) != 0u ? 1.0 : 0.0, (sm & 1u) != 0u ? 1.0 : 0.0, (sm & 0x40u) != 0u ? 1.0 : 0.0, 1.0); return; }\n"
    "  if ((sm & 0x80u) != 0u) { o = dst; return; }\n"
    "  float d = dread(q);\n"
    "  if (d >= 0.99999) { o = dst; return; }\n"
    "  vec3 c = dst.rgb;\n"
    "  float mc = max(c.r, max(c.g, c.b));\n"
    "  if ((sm & 0x40u) != 0u) {\n"
    "    vec3 p = world_at((vec2(q) + 0.5) / w.screen.zw, d);\n"
    "    vec2 pr = vec2(p.x * 0.8 + p.z * 0.6, p.z * 0.8 - p.x * 0.6);\n"
    "    float cover = w.flags.z;\n"
    "    float nz = vnoise(pr * 0.7) * 0.55 + vnoise(pr * 2.6) * 0.45;\n"
    "    float patch_ = smoothstep(0.62 - 0.22 * cover, 0.78 - 0.22 * cover, nz);\n"
    "    float tip = smoothstep(0.03, 0.25, mc / w.tone.w);\n"
    "    float k = (0.06 + 0.3 * patch_) * tip * smoothstep(0.1, 0.8, cover);\n"
    "    if (w.trail.y > 0.5) { o = mix(dst, vec4(1.0, 0.0, 0.0, 1.0), k); return; }\n"
    "    vec3 col = w.tone.rgb * (0.55 + 0.45 * saturate(mc / w.tone.w));\n"
    "    o = vec4(mix(c, col, k), dst.a);\n"
    "    return;\n"
    "  }\n"
    "  if (mc < 0.018) { o = dst; return; }\n"
    "  vec2 e = 1.0 / w.screen.zw;\n"
    "  float ez = sc_eye((vec2(q) + 0.5) * e, d);\n"
    "  vec2 hp = gl_FragCoord.xy / w.trail.z - 0.5;\n"
    "  ivec2 h0 = ivec2(floor(hp)), hm = textureSize(hf, 0) - 1;\n"
    "  vec2 f = hp - vec2(h0);\n"
    "  vec4 acc = vec4(0.0);\n"
    "  float wsum = 0.0, best = 1e9;\n"
    "  vec4 near = vec4(0.0, 1.0, 0.0, 0.0);\n"
    "  for (int i = 0; i < 4; i++) {\n"
    "    ivec2 of = ivec2(i & 1, i >> 1);\n"
    "    vec4 t = texelFetch(hf, clamp(h0 + of, ivec2(0), hm), 0);\n"
    "    float dz = abs(t.w - ez) / ez;\n"
    "    float wt = (of.x != 0 ? f.x : 1.0 - f.x) * (of.y != 0 ? f.y : 1.0 - f.y) * (1.0 - smoothstep(0.005, 0.02, dz));\n"
    "    acc += t * wt; wsum += wt;\n"
    "    if (dz < best) { best = dz; near = t; }\n"
    "  }\n"
    "  vec4 hv = wsum > 1e-4 ? acc / wsum : near;\n"
    "  float k = hv.x * smoothstep(0.018, 0.03, mc);\n"
    "  uint ts = (sm >> 1) & 7u;\n"
    "  if (ts != 0u) k *= shake(ts);\n"
    "  if (k <= 0.0) { o = dst; return; }\n"
    "  float cover = w.flags.z, rough = hv.z;\n"
    "  vec3 p = world_at((vec2(q) + 0.5) * e, d);\n"
    "  vec2 pr = vec2(p.x * 0.8 + p.z * 0.6, p.z * 0.8 - p.x * 0.6);\n"
    "  float fp = sc_print(p);\n"
    "  if (w.trail.y > 0.5) { o = mix(dst, vec4(1.0, 0.0, 0.0, 1.0), k); return; }\n"
    "  k *= 1.0 - saturate(fp) * (0.9 - 0.45 * cover);\n"
    "  float lit = mix(saturate(mc / w.tone.w), 0.8, rough * 0.7);\n"
    "  vec3 col = w.tone.rgb * (0.45 + 0.55 * lit) * (0.85 + 0.15 * hv.y) * (0.96 + 0.06 * vnoise(pr * 3.0));\n"
    "  col *= vec3(1.0 - 0.5 * saturate(fp), 1.0 - 0.42 * saturate(fp), 1.0 - 0.28 * saturate(fp));\n"
    "  o = vec4(mix(c, col, k), dst.a);\n"
    "}\n";

enum { S_STAMP, S_PREP, S_HALF, S_COVER, NPIPE };
static const char *const *const kSrc[NPIPE][2] = {{&stamp_vs, &stamp_fs}, {&full_vs, &prep_fs}, {&full_vs, &half_fs}, {&full_vs, &cover_fs}};
static VkShaderModule g_mod[NPIPE][2];
static VkPipeline g_pipe[NPIPE];
static VkFormat g_cover_fmt;
static VkDescriptorSetLayout g_set_layout;
static VkPipelineLayout g_layout;
static int g_ok = -1;

// the images of their own: the trail map (1024 x 1024, world-anchored), the prep and half-resolution analysis, the
// scene image's copy (its format; at least the drawn area)
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; VkFormat fmt; uint32_t w, h; } Img;
static Img g_trail, g_prep, g_half, g_copy;

static bool img_make(Img *m, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    if (m->img && m->fmt == fmt && m->w == w && m->h == h) return true;
    if (m->img) vk_defer_view(m->view), vk_defer_image(m->img, m->mem), memset(m, 0, sizeof *m);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &m->img, &m->mem, NULL) != VK_SUCCESS) { m->img = VK_NULL_HANDLE; return false; }
    vk_image_init_layout(vk_cmd(), m->img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &m->view) != VK_SUCCESS) {
        vk_defer_image(m->img, m->mem);
        memset(m, 0, sizeof *m);
        return false;
    }
    m->fmt = fmt, m->w = w, m->h = h;
    return true;
}

static VkShaderModule compile(int i, int stage) {
    const char *body = *kSrc[i][stage == STAGE_VERTEX ? 0 : 1];
    size_t n = strlen(SC_PRELUDE) + strlen(sc_common) + strlen(body) + 8;
    char *src = malloc(n);
    snprintf(src, n, "%s%s%s", SC_PRELUDE, sc_common, body);
    char name[32];
    snprintf(name, sizeof name, "snow%d.%s", i, stage == STAGE_VERTEX ? "vert" : "frag");
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    free(src);
    return m;
}

static VkPipeline make_pipe(int i, VkFormat fmt) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_mod[i][0], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g_mod[i][1], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = i == S_STAMP ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
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
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) {
        LOG("[snow] vk: pipeline %d failed\n", i);
        return VK_NULL_HANDLE;
    }
    return p;
}

// Once: shaders, the descriptor layout (0 the frame, 1 the scene depth, 2 its stencil, 3 the scene image's copy, 4 the
// half-resolution analysis, 5 the prep, 6 the trail map, 7 the new prints), the fixed-format pipelines, the trail map
static bool init(void) {
    if (g_ok >= 0) return g_ok;
    g_ok = 0;
    for (int i = 0; i < NPIPE; i++) {
        g_mod[i][0] = compile(i, STAGE_VERTEX);
        g_mod[i][1] = compile(i, STAGE_FRAGMENT);
        if (!g_mod[i][0] || !g_mod[i][1]) { LOG("[snow] vk: shaders for pass %d failed\n", i); return false; }
    }
    VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding b[8];
    for (uint32_t i = 0; i < 8; i++)
        b[i] = (VkDescriptorSetLayoutBinding){i, i == 0 || i == 7 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                              1, vf, NULL};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 8, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &g_set_layout));
    VkPipelineLayoutCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_set_layout};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pci, NULL, &g_layout));
    g_pipe[S_STAMP] = make_pipe(S_STAMP, VK_FORMAT_R32G32B32A32_SFLOAT);
    g_pipe[S_PREP] = make_pipe(S_PREP, VK_FORMAT_R32G32B32A32_SFLOAT);
    g_pipe[S_HALF] = make_pipe(S_HALF, VK_FORMAT_R16G16B16A16_SFLOAT);
    if (!g_pipe[S_STAMP] || !g_pipe[S_PREP] || !g_pipe[S_HALF]) return false;
    if (!img_make(&g_trail, VK_FORMAT_R32G32B32A32_SFLOAT, 1024, 1024, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return false;
    g_ok = 1;
    return true;
}

static void begin(VkCommandBuffer cb, VkImageView view, uint32_t w, uint32_t h, bool clear) {
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    if (g_vk.vids) {
        VkVertexInputBindingDescription2EXT vb = {.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
        VkVertexInputAttributeDescription2EXT va = {.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
        rec_CmdSetVertexInputEXT(cb, 0, &vb, 0, &va);
    }
}

// The passes, in the frame's command buffer outside any rendering (exec.c ended the game's). color: the scene image
// (its view for rendering into, its image to copy, fmt, the drawn area cw x ch), depth / stencil: the scene depth's
// sampled views. sf: weather_snow_cover's frame, screen / flags / zr filled by exec.c
bool vk_snow_pass(VkImage color_img, VkImageView color, VkFormat fmt, uint32_t cw, uint32_t ch, VkImageView depth,
                  VkImageView stencil, SnowFrame *sf) {
    if (!init()) return false;
    if (g_cover_fmt != fmt || !g_pipe[S_COVER]) {  // (by the scene image's format: kept, a frame in flight may use one)
        static struct { VkFormat fmt; VkPipeline p; } made[4];
        int i = 0;
        while (i < 4 && made[i].p && made[i].fmt != fmt) i++;
        if (i == 4) return false;
        if (!made[i].p) made[i].p = make_pipe(S_COVER, fmt), made[i].fmt = fmt;
        g_pipe[S_COVER] = made[i].p;
        g_cover_fmt = fmt;
        if (!g_pipe[S_COVER]) return false;
    }
    // the analysis' grid: a texel per 2x2 pixels at 1080p (renderer.m). LARECOMP_SNOW_HALF=<n> (test): n x n
    static int fixed_s = -1;
    if (fixed_s < 0) fixed_s = getenv("LARECOMP_SNOW_HALF") ? atoi(getenv("LARECOMP_SNOW_HALF")) : 0;
    int hs = fixed_s > 0 ? fixed_s : (int)fmaxf(2, roundf(2.0f * ch / 1080));
    sf->trail[2] = (float)hs;
    uint32_t hw = (cw + hs - 1) / hs, hh = (ch + hs - 1) / hs;
    uint32_t pw = g_half.w > hw ? g_half.w : hw, ph = g_half.h > hh ? g_half.h : hh;  // (half: grows only, as renderer.m)
    uint32_t xw = g_copy.w > cw ? g_copy.w : cw, xh = g_copy.h > ch ? g_copy.h : ch;
    if (!img_make(&g_half, VK_FORMAT_R16G16B16A16_SFLOAT, pw, ph, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        !img_make(&g_prep, VK_FORMAT_R32G32B32A32_SFLOAT, hw, hh, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
        !img_make(&g_copy, fmt, g_copy.fmt == fmt ? xw : cw, g_copy.fmt == fmt ? xh : ch, VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // (the scene's opaque draws: colour, depth and the stencil marks)
    // the scene image's drawn area, for reading (no framebuffer fetch here)
    VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {cw, ch, 1}};
    rec_CmdCopyImage(cb, color_img, VK_IMAGE_LAYOUT_GENERAL, g_copy.img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    SnowPrint pr[64];
    int np = weather_snow_prints(pr, 64);
    VkSpan fs = vk_transient_copy(sf, sizeof *sf, 256);
    VkSpan ps = vk_transient(sizeof pr, 256);
    memcpy(ps.ptr, pr, sizeof pr);
    VkDescriptorSet set = vk_alloc_set(g_set_layout);
    VkSampler smp = vk_default_sampler(false);
    VkDescriptorBufferInfo bf = {fs.buf, fs.off, sizeof *sf}, bp = {ps.buf, ps.off, sizeof pr};
    VkDescriptorImageInfo im[6] = {{smp, depth, VK_IMAGE_LAYOUT_GENERAL},      {smp, stencil, VK_IMAGE_LAYOUT_GENERAL},
                                   {smp, g_copy.view, VK_IMAGE_LAYOUT_GENERAL}, {smp, g_half.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {smp, g_prep.view, VK_IMAGE_LAYOUT_GENERAL}, {smp, g_trail.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[8];
    for (uint32_t i = 0; i < 8; i++) {
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1};
        if (i == 0 || i == 7) wr[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, wr[i].pBufferInfo = i ? &bp : &bf;
        else wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[i].pImageInfo = &im[i - 1];
    }
    rec_UpdateDescriptorSets(8, wr);
    // new footprints into the trail map (cleared the first time)
    static bool trail_clear = true;
    if (np || trail_clear) {
        begin(cb, g_trail.view, 1024, 1024, trail_clear);
        trail_clear = false;
        if (np) {
            rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe[S_STAMP]);
            rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &set, 0, NULL);
            rec_CmdDraw(cb, 4, (uint32_t)np * 4, 0, 0);
        }
        rec_CmdEndRendering(cb);
    }
    vk_full_barrier(cb);  // (the copy, the prints)
    begin(cb, g_prep.view, hw, hh, false);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe[S_PREP]);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &set, 0, NULL);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    begin(cb, g_half.view, hw, hh, false);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe[S_HALF]);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &set, 0, NULL);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    begin(cb, color, cw, ch, false);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe[S_COVER]);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &set, 0, NULL);
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    return true;
}
