// The Warp tab's maps (warp_maps.h). Every mark here is drawn from simple shapes: squares, arcs, lines.
#include "warp_maps.h"

#include <math.h>
#include <string.h>

#include <algorithm>

#ifndef IM_PI
#define IM_PI 3.14159265358979323846f
#endif

static ImU32 rgb(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
static ImU32 shade(ImU32 c, float f) {  // f > 0 lighter, < 0 darker
    int r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24;
    auto m = [f](int v) { return (int)std::max(0.0f, std::min(255.0f, f > 0 ? v + (255 - v) * f : v * (1 + f))); };
    return IM_COL32(m(r), m(g), m(b), a);
}
static float rnd(int a, int b, int k) {  // (a fixed scatter per square)
    float x = sinf(a * 127.1f + b * 311.7f + k * 74.7f) * 43758.5453f;
    return x - floorf(x);
}
static const ImU32 kInk = IM_COL32(16, 14, 22, 255);

// ---------- icons (24-unit boxes, as the sketches) ----------
void warp_icon(ImDrawList *dl, int k, ImVec2 c, float s, ImU32 col) {
    float u = s / 24.0f, th = std::max(1.2f, s / 11.0f);
    auto P = [&](float x, float y) { return ImVec2(c.x + (x - 12) * u, c.y + (y - 12) * u); };
    switch (k) {
    case WI_DOOR:
        dl->PathLineTo(P(6, 21)), dl->PathLineTo(P(6, 10)), dl->PathArcTo(P(12, 10), 6 * u, IM_PI, 2 * IM_PI), dl->PathLineTo(P(18, 21));
        dl->PathStroke(col, 0, th), dl->AddLine(P(3.5f, 21), P(20.5f, 21), col, th);
        break;
    case WI_CAVE:
        dl->PathArcTo(P(12, 20), 9.5f * u, IM_PI, 2 * IM_PI), dl->PathStroke(col, ImDrawFlags_Closed, th);
        dl->PathArcTo(P(12, 20), 3.2f * u, IM_PI, 2 * IM_PI), dl->PathStroke(col, 0, th);
        break;
    case WI_DUNGEON:
        dl->PathLineTo(P(4, 21)), dl->PathLineTo(P(4, 9.5f)), dl->PathLineTo(P(12, 4)), dl->PathLineTo(P(20, 9.5f)), dl->PathLineTo(P(20, 21));
        dl->PathStroke(col, ImDrawFlags_Closed, th);
        dl->PathLineTo(P(9.5f, 21)), dl->PathArcTo(P(12, 16), 2.5f * u, IM_PI, 2 * IM_PI), dl->PathLineTo(P(14.5f, 21)), dl->PathStroke(col, 0, th);
        break;
    case WI_STAIRS:
        for (ImVec2 p : {P(3.5f, 20.5f), P(8, 20.5f), P(8, 16), P(12.5f, 16), P(12.5f, 11.5f), P(17, 11.5f), P(17, 7), P(20.5f, 7)}) dl->PathLineTo(p);
        dl->PathStroke(col, 0, th);
        break;
    case WI_LADDER:
        dl->AddLine(P(8, 3), P(8, 21), col, th), dl->AddLine(P(16, 3), P(16, 21), col, th);
        for (float y : {7.5f, 12.0f, 16.5f}) dl->AddLine(P(8, y), P(16, y), col, th);
        break;
    case WI_DROP:
        dl->AddLine(P(12, 3), P(12, 14), col, th), dl->AddLine(P(7.5f, 9.5f), P(12, 14), col, th), dl->AddLine(P(16.5f, 9.5f), P(12, 14), col, th);
        dl->AddLine(P(5, 20.5f), P(19, 20.5f), col, th);
        break;
    case WI_ENTRANCE:
        dl->AddLine(P(12, 21), P(12, 9), col, th), dl->AddLine(P(7.5f, 13.5f), P(12, 9), col, th), dl->AddLine(P(16.5f, 13.5f), P(12, 9), col, th);
        dl->AddLine(P(4, 3.5f), P(20, 3.5f), col, th);
        break;
    case WI_WARP:
        for (int i = 0; i <= 40; i++) { float t = i / 40.0f * 3.2f * IM_PI, r = (1.0f + 2.6f * t) * u; dl->PathLineTo(ImVec2(c.x + cosf(t) * r, c.y + sinf(t) * r)); }
        dl->PathStroke(col, 0, th);
        break;
    case WI_PHONE:
        dl->AddRect(P(7, 4), P(17, 20), col, 2 * u, 0, th), dl->AddLine(P(10, 16.5f), P(14, 16.5f), col, th);
        dl->AddRect(P(9.5f, 6.5f), P(14.5f, 12.5f), col, u, 0, th * 0.7f);
        break;
    case WI_FAIRY:
        for (ImVec2 p : {P(12, 2.5f), P(13.9f, 10.1f), P(21.5f, 12), P(13.9f, 13.9f), P(12, 21.5f), P(10.1f, 13.9f), P(2.5f, 12), P(10.1f, 10.1f)}) dl->PathLineTo(p);
        dl->PathStroke(col, ImDrawFlags_Closed, th);
        break;
    case WI_NOTE:
        dl->AddCircle(P(6.4f, 18.5f), 2.6f * u, col, 0, th), dl->AddCircle(P(16.4f, 16.1f), 2.6f * u, col, 0, th);
        dl->AddLine(P(9, 18.5f), P(9, 5.5f), col, th), dl->AddLine(P(19, 16.1f), P(19, 3.3f), col, th), dl->AddLine(P(9, 5.5f), P(19, 3.3f), col, th);
        break;
    case WI_PIN:
        dl->PathArcTo(P(12, 10), 6.5f * u, 0.8f * IM_PI, 2.2f * IM_PI), dl->PathLineTo(P(12, 21.5f)), dl->PathStroke(col, ImDrawFlags_Closed, th);
        dl->AddCircle(P(12, 10), 2.2f * u, col, 0, th);
        break;
    case WI_BOAT:
        for (ImVec2 p : {P(2.5f, 15.5f), P(21.5f, 15.5f), P(18.3f, 20.5f), P(5.7f, 20.5f)}) dl->PathLineTo(p);
        dl->PathStroke(col, ImDrawFlags_Closed, th);
        dl->AddLine(P(12, 3), P(12, 15.5f), col, th), dl->AddTriangle(P(12, 4), P(18.5f, 12.5f), P(12, 12.5f), col, th);
        break;
    default:  // WI_ROOM
        dl->AddRect(P(4, 4), P(20, 20), col, 0, 0, th);
        break;
    }
}
static ImU32 icon_col(int k) {
    static const ImU32 c[WI_N] = {rgb(242, 211, 138), rgb(216, 193, 163), rgb(240, 163, 94), rgb(201, 184, 240), rgb(201, 184, 240),
                                  rgb(159, 212, 234), rgb(185, 156, 242), rgb(159, 224, 181), rgb(245, 168, 212), rgb(159, 212, 234),
                                  rgb(230, 225, 236), rgb(189, 182, 201), rgb(242, 211, 138), rgb(159, 212, 234)};
    return k >= 0 && k < WI_N ? c[k] : c[WI_ROOM];
}

// ---------- the island ----------
enum { T_ROCK, T_MOUNTAIN, T_HILLS, T_SWAMP, T_WOODS, T_GRAVE, T_CASTLE, T_RIVER, T_VILLAGE, T_RUINS, T_PRAIRIE, T_BEACH, T_DESERT, T_WATER };
static int terrain_of(const char *region) {
    static const struct { const char *n; int t; } k[] = {
        {"Turtle Rock", T_ROCK}, {"Tal Tal Mountain Range", T_MOUNTAIN}, {"Tal Tal Heights", T_HILLS}, {"Goponga Swamp", T_SWAMP},
        {"Mysterious Woods", T_WOODS}, {"Cemetery", T_GRAVE}, {"Kanalet Castle", T_CASTLE}, {"Face Shrine & Rapids", T_RIVER},
        {"Mabe Village", T_VILLAGE}, {"Southern Face Shrine", T_RUINS}, {"Ukuku Prairie", T_PRAIRIE}, {"Toronbo Shores", T_BEACH},
        {"Animal Village", T_VILLAGE}, {"Yarna Desert", T_DESERT}, {"Martha's Bay", T_WATER}};
    for (auto &e : k)
        if (region && !strcmp(region, e.n)) return e.t;
    return T_PRAIRIE;
}
static ImU32 terrain_col(int t) {
    static const ImU32 c[] = {rgb(93, 75, 74), rgb(108, 95, 83), rgb(119, 114, 78), rgb(77, 90, 58), rgb(47, 90, 61), rgb(84, 80, 104), rgb(86, 95, 112),
                              rgb(58, 106, 105), rgb(134, 104, 63), rgb(106, 104, 83), rgb(92, 125, 65), rgb(168, 145, 90), rgb(176, 141, 85), rgb(46, 91, 122)};
    return c[t];
}
static void terrain(ImDrawList *dl, int t, ImVec2 o, float s, int r, int c) {
    ImU32 base = terrain_col(t), lt = shade(base, 0.22f), dk = shade(base, -0.3f);
    float th = std::max(1.0f, s / 22);
    auto R = [&](int k) { return rnd(r, c, k); };
    dl->PushClipRect(o, ImVec2(o.x + s, o.y + s), true);
    switch (t) {
    case T_WOODS:
        for (int k = 0; k < 3; k++) {
            ImVec2 p(o.x + s * (0.2f + 0.3f * k + R(k) * 0.1f), o.y + s * (0.3f + R(k + 3) * 0.45f));
            float rr = s * (0.16f + R(k + 6) * 0.06f);
            dl->AddCircleFilled(ImVec2(p.x, p.y + rr * 0.25f), rr, dk), dl->AddCircleFilled(ImVec2(p.x - rr * 0.2f, p.y - rr * 0.15f), rr * 0.7f, lt);
        }
        break;
    case T_MOUNTAIN: case T_ROCK:
        for (int k = 0; k < 2; k++) {
            float bx = o.x + s * (0.08f + k * 0.42f + R(k) * 0.08f), w = s * 0.5f, h = s * (0.4f + R(k + 2) * 0.25f), by = o.y + s * 0.85f;
            dl->AddTriangleFilled(ImVec2(bx, by), ImVec2(bx + w / 2, by - h), ImVec2(bx + w, by), dk);
            dl->AddLine(ImVec2(bx + w * 0.3f, by - h * 0.6f), ImVec2(bx + w / 2, by - h), lt, th), dl->AddLine(ImVec2(bx + w / 2, by - h), ImVec2(bx + w * 0.62f, by - h * 0.75f), lt, th);
        }
        break;
    case T_HILLS:
        for (int k = 0; k < 2; k++) {
            dl->PathArcTo(ImVec2(o.x + s * (0.3f + k * 0.35f + R(k) * 0.1f), o.y + s * (0.45f + k * 0.3f)), s * 0.2f, IM_PI * 1.1f, IM_PI * 1.9f);
            dl->PathStroke(lt, 0, th);
        }
        break;
    case T_WATER: case T_RIVER:
        for (int k = 0; k < 3; k++) {
            float y = o.y + s * (0.25f + k * 0.25f), x = o.x + s * (0.1f + R(k) * 0.3f);
            dl->AddBezierQuadratic(ImVec2(x, y), ImVec2(x + s * 0.12f, y - s * 0.08f), ImVec2(x + s * 0.24f, y), lt, th);
            dl->AddBezierQuadratic(ImVec2(x + s * 0.24f, y), ImVec2(x + s * 0.36f, y + s * 0.08f), ImVec2(x + s * 0.48f, y), lt, th);
        }
        break;
    case T_BEACH: case T_DESERT: {
        for (int k = 0; k < 6; k++) dl->AddCircleFilled(ImVec2(o.x + s * R(k), o.y + s * R(k + 7)), s * 0.03f, dk);
        float y = o.y + s * (0.55f + R(20) * 0.2f);
        dl->AddBezierQuadratic(ImVec2(o.x + s * 0.1f, y), ImVec2(o.x + s * 0.5f, y - s * 0.18f), ImVec2(o.x + s * 0.9f, y), t == T_BEACH ? rgb(207, 227, 234) : lt, th);
        break;
    }
    case T_PRAIRIE:
        for (int k = 0; k < 5; k++) {
            float gx = o.x + s * (0.1f + R(k) * 0.8f), gy = o.y + s * (0.2f + R(k + 5) * 0.7f), q = s * 0.06f;
            dl->AddLine(ImVec2(gx - q, gy - q), ImVec2(gx, gy), lt, th), dl->AddLine(ImVec2(gx, gy), ImVec2(gx + q, gy - q * 1.3f), lt, th);
        }
        break;
    case T_SWAMP:
        for (int k = 0; k < 3; k++) dl->AddEllipseFilled(ImVec2(o.x + s * (0.2f + R(k) * 0.6f), o.y + s * (0.25f + R(k + 3) * 0.55f)), ImVec2(s * 0.14f, s * 0.07f), rgb(46, 74, 85));
        break;
    case T_VILLAGE:
        dl->AddLine(ImVec2(o.x, o.y + s * 0.62f), ImVec2(o.x + s, o.y + s * 0.62f), lt, th);
        if (R(1) < 0.6f) {
            float hx = o.x + s * (0.2f + R(2) * 0.35f), hy = o.y + s * 0.5f, w = s * 0.3f;
            dl->AddRectFilled(ImVec2(hx, hy - w * 0.55f), ImVec2(hx + w, hy), dk);
            dl->AddTriangleFilled(ImVec2(hx - w * 0.12f, hy - w * 0.5f), ImVec2(hx + w / 2, hy - w), ImVec2(hx + w * 1.12f, hy - w * 0.5f), rgb(148, 72, 57));
        }
        break;
    case T_GRAVE:
        for (int k = 0; k < 2; k++) {
            float gx = o.x + s * (0.22f + k * 0.38f + R(k) * 0.08f), gy = o.y + s * (0.35f + R(k + 2) * 0.3f), w = s * 0.16f;
            dl->AddRectFilled(ImVec2(gx, gy + w * 0.5f), ImVec2(gx + w, gy + w * 1.4f), shade(base, 0.3f));
            dl->AddCircleFilled(ImVec2(gx + w / 2, gy + w * 0.5f), w / 2, shade(base, 0.3f));
        }
        break;
    case T_CASTLE:
        for (int k = 1; k < 4; k++) {
            float y = o.y + s * k / 4;
            dl->AddLine(ImVec2(o.x, y), ImVec2(o.x + s, y), dk, th);
            for (int j = 0; j < 3; j++) { float x = o.x + s * (j / 3.0f + (k % 2) * 0.16f); dl->AddLine(ImVec2(x, y - s / 4), ImVec2(x, y), dk, th); }
        }
        break;
    case T_RUINS:
        for (int k = 0; k < 3; k++) { float px = o.x + s * (0.15f + k * 0.27f), h = s * (0.2f + R(k) * 0.35f); dl->AddRectFilled(ImVec2(px, o.y + s * 0.8f - h), ImVec2(px + s * 0.12f, o.y + s * 0.8f), shade(base, 0.25f)); }
        break;
    }
    dl->PopClipRect();
}
void warp_draw_island(ImDrawList *dl, ImVec2 a, ImVec2 b, const WarpIsland &is, int cur_r, int cur_c, int region) {
    float t = floorf(std::min((b.x - a.x) / 16, (b.y - a.y) / 16));
    if (t < 2) return;
    ImVec2 o(floorf(a.x + (b.x - a.x - t * 16) / 2), floorf(a.y + (b.y - a.y - t * 16) / 2));
    if (cur_r >= 0 && region < 0) region = is.sq[cur_r][cur_c].region;
    dl->AddRectFilled(ImVec2(o.x - 3, o.y - 3), ImVec2(o.x + t * 16 + 3, o.y + t * 16 + 3), kInk, 6);
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16; c++) {
            const WarpIslandSquare &q = is.sq[r][c];
            int tt = terrain_of(q.region >= 0 ? is.regions[q.region] : NULL);
            ImVec2 p(o.x + c * t, o.y + r * t);
            dl->AddRectFilled(p, ImVec2(p.x + t, p.y + t), terrain_col(tt));
            if (t > 12) terrain(dl, tt, p, t, r, c);
            if (region >= 0 && q.region != region) dl->AddRectFilled(p, ImVec2(p.x + t, p.y + t), IM_COL32(10, 9, 14, 100));
        }
    // region borders (the lit one in gold)
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16; c++) {
            int g = is.sq[r][c].region;
            ImVec2 p(o.x + c * t, o.y + r * t);
            auto edge = [&](int g2, ImVec2 p1, ImVec2 p2) {
                if (g2 == g) return;
                bool hot = region >= 0 && (g == region || g2 == region);
                dl->AddLine(p1, p2, hot ? IM_COL32(237, 194, 84, 220) : IM_COL32(12, 10, 16, 170), hot ? 2.0f : 1.5f);
            };
            if (c < 15) edge(is.sq[r][c + 1].region, ImVec2(p.x + t, p.y), ImVec2(p.x + t, p.y + t));
            if (r < 15) edge(is.sq[r + 1][c].region, ImVec2(p.x, p.y + t), ImVec2(p.x + t, p.y + t));
        }
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16; c++) {
            const WarpIslandSquare &q = is.sq[r][c];
            if (!q.n) continue;
            ImVec2 m(o.x + c * t + t / 2, o.y + r * t + t / 2);
            if (t > 16) {
                dl->AddCircleFilled(m, t * 0.35f, IM_COL32(16, 14, 22, 184));
                warp_icon(dl, q.icons[0], m, t * 0.56f, icon_col(q.icons[0]));
            } else {
                dl->AddCircleFilled(m, std::max(1.6f, t * 0.12f), rgb(244, 236, 216));
            }
        }
    if (cur_r >= 0) {
        ImVec2 p(o.x + cur_c * t, o.y + cur_r * t);
        dl->AddRect(ImVec2(p.x - 2, p.y - 2), ImVec2(p.x + t + 2, p.y + t + 2), IM_COL32(237, 194, 84, 90), 5, 0, 7);
        dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(p.x + t + 1, p.y + t + 1), rgb(237, 194, 84), 4, 0, 3);
    }
}

// ---------- dungeon rooms ----------
static ImU32 floor_col(float e) { return e > 0.4f ? rgb(78, 71, 96) : e < -0.3f ? rgb(46, 42, 58) : rgb(62, 56, 76); }
static void feature(ImDrawList *dl, const WarpRoomFeat &f, ImVec2 p, float tw) {
    float s = tw * 0.8f, th = std::max(1.0f, tw * 0.1f);
    ImU32 ink = IM_COL32(10, 8, 14, 190);
    switch (f.kind) {
    case WF_BLOCK: case WF_CBLOCK: case WF_LOCKBLOCK: {
        ImU32 c = f.kind == WF_CBLOCK ? rgb(79, 159, 174) : rgb(138, 129, 156);
        dl->AddRectFilled(ImVec2(p.x - s / 2, p.y - s / 2), ImVec2(p.x + s / 2, p.y + s / 2), c), dl->AddRect(ImVec2(p.x - s / 2, p.y - s / 2), ImVec2(p.x + s / 2, p.y + s / 2), ink, 0, 0, th);
        if (f.kind == WF_LOCKBLOCK) dl->AddCircleFilled(p, s * 0.16f, rgb(237, 194, 84));
        break;
    }
    case WF_POT: dl->AddCircleFilled(p, s * 0.38f, rgb(180, 138, 94)), dl->AddCircle(p, s * 0.38f, ink, 0, th); break;
    case WF_CHEST:
        dl->AddRectFilled(ImVec2(p.x - s * 0.45f, p.y - s * 0.32f), ImVec2(p.x + s * 0.45f, p.y + s * 0.32f), rgb(217, 164, 65));
        dl->AddLine(ImVec2(p.x - s * 0.45f, p.y - s * 0.08f), ImVec2(p.x + s * 0.45f, p.y - s * 0.08f), rgb(107, 74, 20), th);
        break;
    case WF_STAIRS: warp_icon(dl, WI_STAIRS, p, tw * 1.1f, rgb(201, 184, 240)); break;
    case WF_HOLE: dl->AddEllipseFilled(p, ImVec2(s * 0.6f, s * 0.45f), rgb(9, 8, 12)); break;
    case WF_SPIKES:
        for (int i = -1; i <= 1; i += 2)
            for (int j = -1; j <= 1; j += 2) {
                ImVec2 q(p.x + i * s * 0.2f, p.y + j * s * 0.2f);
                dl->AddTriangleFilled(ImVec2(q.x - s * 0.15f, q.y + s * 0.12f), ImVec2(q.x, q.y - s * 0.15f), ImVec2(q.x + s * 0.15f, q.y + s * 0.12f), rgb(169, 162, 182));
            }
        break;
    case WF_BELT:
        for (float d : {-0.3f, 0.05f}) {
            dl->PathLineTo(ImVec2(p.x + s * d, p.y - s * 0.25f)), dl->PathLineTo(ImVec2(p.x + s * (d + 0.35f), p.y)), dl->PathLineTo(ImVec2(p.x + s * d, p.y + s * 0.25f));
            dl->PathStroke(rgb(130, 122, 150), 0, th);
        }
        break;
    case WF_SWITCH: dl->AddQuadFilled(ImVec2(p.x, p.y - s * 0.4f), ImVec2(p.x + s * 0.32f, p.y), ImVec2(p.x, p.y + s * 0.4f), ImVec2(p.x - s * 0.32f, p.y), rgb(127, 208, 224)); break;
    case WF_STATUE:
        dl->AddRectFilled(ImVec2(p.x - s * 0.3f, p.y - s * 0.1f), ImVec2(p.x + s * 0.3f, p.y + s * 0.4f), rgb(165, 156, 184));
        dl->AddCircleFilled(ImVec2(p.x, p.y - s * 0.1f), s * 0.3f, rgb(165, 156, 184));
        break;
    case WF_TRAP:
        dl->AddLine(ImVec2(p.x - s * 0.3f, p.y - s * 0.3f), ImVec2(p.x + s * 0.3f, p.y + s * 0.3f), rgb(217, 130, 107), th * 1.4f);
        dl->AddLine(ImVec2(p.x + s * 0.3f, p.y - s * 0.3f), ImVec2(p.x - s * 0.3f, p.y + s * 0.3f), rgb(217, 130, 107), th * 1.4f);
        break;
    case WF_INSTRUMENT: dl->AddCircleFilled(p, tw * 0.7f, IM_COL32(237, 194, 84, 60)), warp_icon(dl, WI_NOTE, p, tw * 1.2f, rgb(237, 194, 84)); break;
    default: {  // doors: a bar across the doorway, coloured by kind
        bool across_x = f.x < 0.6f || f.x > 9.4f;  // (on a west / east edge: the bar runs along z)
        ImVec2 h = across_x ? ImVec2(tw * 0.25f, tw * 0.8f) : ImVec2(tw * 0.8f, tw * 0.25f);
        ImU32 c = f.kind == WF_DOOR_LOCK ? rgb(214, 160, 52) : f.kind == WF_DOOR_BOSS ? rgb(196, 64, 52) : f.kind == WF_DOOR_CRACKED ? rgb(120, 112, 136)
                : f.kind == WF_DOOR_ONEWAY ? rgb(159, 212, 234) : rgb(98, 90, 112);
        dl->AddRectFilled(ImVec2(p.x - h.x, p.y - h.y), ImVec2(p.x + h.x, p.y + h.y), c);
        if (f.kind == WF_DOOR_LOCK) dl->AddCircleFilled(p, tw * 0.14f, rgb(255, 236, 170));
        break;
    }
    }
}
// one cell's tiles at o (tw per tile); detail: landmarks too
static void cell(ImDrawList *dl, const WarpRoom &r, ImVec2 o, float tw, bool detail) {
    for (int i = 0; i < 80; i++) {
        ImVec2 p(o.x + (i % 10) * tw, o.y + (i / 10) * tw), q(p.x + tw + 0.5f, p.y + tw + 0.5f);
        ImU32 c = r.tile[i] == WT_WALL ? rgb(26, 23, 34) : r.tile[i] == WT_WATER ? rgb(52, 92, 128) : r.tile[i] == WT_DEEP ? rgb(18, 30, 52) : floor_col(r.elev[i]);
        dl->AddRectFilled(p, q, c);
    }
    if (detail && tw > 6) {  // a faint tile grid, and a light edge where floor meets wall
        for (int i = 0; i < 80; i++) {
            if (r.tile[i] == WT_WALL) continue;
            int x = i % 10, y = i / 10;
            ImVec2 p(o.x + x * tw, o.y + y * tw);
            dl->AddRect(p, ImVec2(p.x + tw, p.y + tw), IM_COL32(255, 255, 255, 10));
            ImU32 e = IM_COL32(120, 108, 150, 200);
            if (y > 0 && r.tile[i - 10] == WT_WALL) dl->AddLine(p, ImVec2(p.x + tw, p.y), e, 1.5f);
            if (y < 7 && r.tile[i + 10] == WT_WALL) dl->AddLine(ImVec2(p.x, p.y + tw), ImVec2(p.x + tw, p.y + tw), e, 1.5f);
            if (x > 0 && r.tile[i - 1] == WT_WALL) dl->AddLine(p, ImVec2(p.x, p.y + tw), e, 1.5f);
            if (x < 9 && r.tile[i + 1] == WT_WALL) dl->AddLine(ImVec2(p.x + tw, p.y), ImVec2(p.x + tw, p.y + tw), e, 1.5f);
        }
    }
    for (auto &f : r.feats) {
        bool small = f.kind == WF_CHEST || f.kind == WF_STAIRS || f.kind == WF_INSTRUMENT || f.kind >= WF_DOOR_LOCK;
        if (!detail && (!small || f.kind == WF_DOOR_SHUTTER)) continue;
        if (!detail && tw < 2.5f) continue;
        feature(dl, f, ImVec2(o.x + f.x * tw, o.y + f.z * tw), tw);
    }
}
static void unit_box(const WarpLevel &L, int room, int *c0, int *r0, int *c1, int *r1) {
    int u = L.rooms[room].unit;
    *c0 = *r0 = 1 << 20, *c1 = *r1 = -1;
    for (auto &r : L.rooms)
        if (!r.side && r.unit == u) *c0 = std::min(*c0, r.col), *c1 = std::max(*c1, r.col), *r0 = std::min(*r0, r.row), *r1 = std::max(*r1, r.row);
}
static void pin(ImDrawList *dl, ImVec2 p, float r, bool hot) {
    dl->AddCircleFilled(p, r, hot ? rgb(255, 255, 255) : rgb(237, 194, 84));
    dl->AddCircle(p, r, kInk, 0, 1.5f);
    if (hot) dl->AddCircle(p, r * 1.7f, rgb(237, 194, 84), 0, 2.0f);
}

void warp_draw_floor(ImDrawList *dl, ImVec2 a, ImVec2 b, const WarpLevel &L, int sel, const WarpDot *dots, int ndots) {
    int cols = L.c1 - L.c0 + 1, rows = L.r1 - L.r0 + 1, nside = 0;
    for (auto &r : L.rooms) nside += r.side;
    float strip = nside ? 3.4f : 0;  // (side passages: a strip under the floor, in tile rows)
    float tw = std::min((b.x - a.x - 8) / (cols * 10.0f), (b.y - a.y - 8) / (rows * 8.0f + strip));
    if (tw <= 0.5f) return;
    ImVec2 o(floorf(a.x + (b.x - a.x - cols * 10 * tw) / 2), floorf(a.y + 4));
    for (auto &r : L.rooms)
        if (!r.side) cell(dl, r, ImVec2(o.x + (r.col - L.c0) * 10 * tw, o.y + (r.row - L.r0) * 8 * tw), tw, false);
    // side passages: small strips (seen from the side), in the order of the level's files
    std::vector<int> side;
    for (int i = 0; i < (int)L.rooms.size(); i++)
        if (L.rooms[i].side) side.push_back(i);
    float sy = o.y + rows * 8 * tw + tw * 1.2f, sh = tw * 1.8f, gap = tw;
    float sw = side.empty() ? 0 : std::min(10 * tw, (b.x - a.x - 8 - gap * (side.size() - 1)) / side.size());
    float sx = floorf(a.x + (b.x - a.x - (side.size() * sw + (side.size() - 1) * gap)) / 2);
    for (size_t k = 0; k < side.size(); k++) {
        ImVec2 p(sx + k * (sw + gap), sy);
        dl->AddRectFilled(p, ImVec2(p.x + sw, p.y + sh), rgb(62, 56, 76)), dl->AddRectFilled(ImVec2(p.x, p.y + sh * 0.8f), ImVec2(p.x + sw, p.y + sh), rgb(26, 23, 34));
        if (side[k] == sel) dl->AddRect(ImVec2(p.x - 2, p.y - 2), ImVec2(p.x + sw + 2, p.y + sh + 2), rgb(237, 194, 84), 3, 0, 2.5f);
    }
    for (int i = 0; i < ndots; i++) {
        const WarpRoom &r = L.rooms[dots[i].room];
        ImVec2 p;
        if (r.side) {
            size_t k = std::find(side.begin(), side.end(), dots[i].room) - side.begin();
            if (k >= side.size()) continue;
            p = ImVec2(sx + k * (sw + gap) + sw / 2, sy + sh * 0.45f);
        } else {
            p = ImVec2(o.x + ((r.col - L.c0) * 10 + dots[i].x) * tw, o.y + ((r.row - L.r0) * 8 + dots[i].z) * tw);
        }
        dl->AddCircleFilled(p, std::max(2.2f, tw * (dots[i].hot ? 0.8f : 0.45f)), dots[i].hot ? rgb(255, 255, 255) : rgb(237, 194, 84));
    }
    if (sel >= 0 && !L.rooms[sel].side) {
        int c0, r0, c1, r1;
        unit_box(L, sel, &c0, &r0, &c1, &r1);
        ImVec2 p(o.x + (c0 - L.c0) * 10 * tw, o.y + (r0 - L.r0) * 8 * tw), q(o.x + (c1 - L.c0 + 1) * 10 * tw, o.y + (r1 - L.r0 + 1) * 8 * tw);
        dl->AddRect(ImVec2(p.x - 2, p.y - 2), ImVec2(q.x + 2, q.y + 2), IM_COL32(237, 194, 84, 90), 5, 0, 7);
        dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(q.x + 1, q.y + 1), rgb(237, 194, 84), 4, 0, 3);
    }
}

void warp_draw_room(ImDrawList *dl, ImVec2 a, ImVec2 b, const WarpLevel &L, int room, const WarpDot *dots, int ndots) {
    if (room < 0) return;
    const WarpRoom &rm = L.rooms[room];
    if (rm.side) {  // a passage seen from the side: floor, ceiling, a ladder at each end
        float tw = std::min((b.x - a.x) / 22, (b.y - a.y) / 6), w = 20 * tw, h = 4 * tw;
        ImVec2 p(a.x + (b.x - a.x - w) / 2, a.y + (b.y - a.y - h) / 2);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), rgb(62, 56, 76));
        dl->AddRectFilled(ImVec2(p.x, p.y + h - tw), ImVec2(p.x + w, p.y + h), rgb(26, 23, 34)), dl->AddRectFilled(p, ImVec2(p.x + w, p.y + tw * 0.5f), rgb(26, 23, 34));
        warp_icon(dl, WI_LADDER, ImVec2(p.x + tw * 1.6f, p.y + h / 2), tw * 2.2f, rgb(201, 184, 240));
        warp_icon(dl, WI_LADDER, ImVec2(p.x + w - tw * 1.6f, p.y + h / 2), tw * 2.2f, rgb(201, 184, 240));
        for (int i = 0; i < ndots; i++)
            if (dots[i].room == room) pin(dl, ImVec2(p.x + w / 2, p.y + h / 2), tw * 0.6f, dots[i].hot);
        return;
    }
    int c0, r0, c1, r1;
    unit_box(L, room, &c0, &r0, &c1, &r1);
    int cols = c1 - c0 + 1, rows = r1 - r0 + 1;
    float tw = std::min((b.x - a.x - 8) / (cols * 10.0f), (b.y - a.y - 8) / (rows * 8.0f));
    ImVec2 o(floorf(a.x + (b.x - a.x - cols * 10 * tw) / 2), floorf(a.y + (b.y - a.y - rows * 8 * tw) / 2));
    for (auto &r : L.rooms)
        if (!r.side && r.unit == rm.unit) cell(dl, r, ImVec2(o.x + (r.col - c0) * 10 * tw, o.y + (r.row - r0) * 8 * tw), tw, true);
    for (int i = 0; i < ndots; i++) {
        const WarpRoom &r = L.rooms[dots[i].room];
        if (r.side || r.unit != rm.unit) continue;
        pin(dl, ImVec2(o.x + ((r.col - c0) * 10 + dots[i].x) * tw, o.y + ((r.row - r0) * 8 + dots[i].z) * tw), std::max(4.0f, tw * (dots[i].hot ? 0.5f : 0.3f)), dots[i].hot);
    }
}
