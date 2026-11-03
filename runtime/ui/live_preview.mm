// The live preview (see live_preview.h): the game behind a panel at the bottom with one setting on it.
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner)

extern "C" {
#include "../menu_sfx.h"
}
#include "i18n.h"
#include "live_preview.h"
#include "option_rows.h"
#include "plastic.h"

static struct {
    bool active, scanning, found;
    std::string label;
    int focus_row;
    int orig, cur, n, def;  // (the value when it opened, now; -1: not read yet)
    bool wrap, off;
    int step, set;  // this frame's change for preview_row: a step, or a value (-1: none)
    std::string value, desc;  // (in the menus' language)
    std::string next_desc, next_note;  // (preview_text: for the next row)
    bool confirm;  // the Yes / No to save is up (sel: 0 Yes, 1 No)
    int sel;
} P;

bool preview_active(void) { return P.active; }
bool preview_scanning(void) { return P.scanning; }

void preview_open(const char *label, int focus_row) {
    P = {};
    P.active = true, P.label = label, P.focus_row = focus_row;
    P.orig = P.cur = -1, P.set = -1;
}

void preview_reset(void) { P.active = false; }

void preview_text(const char *desc, const char *note) {
    if (!P.scanning) return;
    P.next_desc = desc ? desc : "", P.next_note = note ? note : "";
}

bool preview_row(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap,
                 bool off) {
    std::string short_desc = P.next_desc, note = P.next_note;
    P.next_desc.clear(), P.next_note.clear();
    if (!P.active || P.label != label || n <= 0) return false;
    P.found = true;
    int cur = *v >= 0 && *v < n ? *v : 0;
    if (P.orig < 0) P.orig = cur;
    int nv = cur;
    if (!off) {
        if (P.set >= 0) nv = P.set < n ? P.set : cur;
        else if (P.step) {
            nv = cur + P.step;
            nv = wrap ? (nv + n) % n : nv < 0 ? 0 : nv >= n ? n - 1 : nv;
        }
    }
    P.step = 0, P.set = -1;
    bool changed = nv != cur;
    if (changed) *v = nv;
    P.cur = nv, P.n = n, P.def = def, P.wrap = wrap, P.off = off;
    P.value = L_line(names[nv]);
    if (!note.empty() && !changed) P.value += " (" + note + ")";  // (changed: the note is last frame's)
    P.desc = !short_desc.empty() ? L(short_desc.c_str()) : desc ? L(desc) : "";
    return changed;
}

static bool pressed(std::initializer_list<ImGuiKey> keys, bool repeat) {
    for (ImGuiKey k : keys)
        if (ImGui::IsKeyPressed(k, repeat)) return true;
    return false;
}

static bool in_rect(ImVec2 m, ImVec2 a, ImVec2 b) { return m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y; }

// text with a soft shadow (it sits on the game); bold: drawn twice, a hair apart
static void text(ImDrawList *dl, ImFont *f, float fs, ImVec2 p, ImU32 col, const char *s, bool bold = false,
                 float wrap = 0) {
    float o = fmaxf(fs * 0.05f, 1.0f);
    dl->AddText(f, fs, ImVec2(p.x + o, p.y + o), IM_COL32(0, 0, 0, 120), s, NULL, wrap);
    dl->AddText(f, fs, p, col, s, NULL, wrap);
    if (bold) dl->AddText(f, fs, ImVec2(p.x + o * 0.6f, p.y), col, s, NULL, wrap);
}

// a chevron at x (its point: left or right), centred on cy
static void chevron(ImDrawList *dl, float x, float cy, float h, bool left, ImU32 col, float th) {
    float w = h * 0.5f;
    ImVec2 pts[3] = {ImVec2(x + (left ? w : 0), cy - h), ImVec2(x + (left ? 0 : w), cy), ImVec2(x + (left ? w : 0), cy + h)};
    dl->AddPolyline(pts, 3, col, 0, th);
}

bool preview_draw(float scale, ImFont *font, ImFont *font_big, void (*page)(void)) {
    if (!P.active) return false;
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 ds = io.DisplaySize, m = io.MousePos;
    bool click = ImGui::IsMouseClicked(0);
    bool close = false, keep = true;
    static const char *test_confirm = getenv("LARECOMP_PREVIEW_CONFIRM");  // (test: a step, then the Yes / No)
    static bool test_done;

    // input (the panel's hit boxes are last frame's: the layout hardly moves)
    static ImVec2 hit[6][2];  // ‹, ›, Return, Reset to default, Yes, No
    bool left = pressed({ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft, ImGuiKey_LeftArrow}, true);
    bool right = pressed({ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight, ImGuiKey_RightArrow}, true);
    bool a = pressed({ImGuiKey_GamepadFaceDown, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space}, false);
    bool b = pressed({ImGuiKey_GamepadFaceRight, ImGuiKey_Escape}, false);  // (its sound: menu_sfx_frame)
    int clicked = -1;
    if (click)
        for (int i = 0; i < 6; i++)
            if (in_rect(m, hit[i][0], hit[i][1])) clicked = i;
    if (test_confirm && !test_done && P.orig >= 0) test_done = true, P.step = 1, P.confirm = true, P.sel = 0;
    if (P.confirm) {
        if (left || right) P.sel ^= 1, menu_sfx_play(MENU_SFX_CURSOR);
        if (clicked == 4 || clicked == 5) P.sel = clicked - 4, a = true;
        if (a) close = true, keep = P.sel == 0, menu_sfx_play(MENU_SFX_DECIDE);
        else if (b) P.confirm = false;
    } else {
        if (clicked == 0) left = true;
        if (clicked == 1) right = true;
        if (!P.off && (left || right)) {
            P.step = left ? -1 : 1;
            menu_sfx_play(MENU_SFX_CURSOR);
        } else if ((left || right) && P.off) menu_sfx_play(MENU_SFX_BEEP);
        if (clicked == 3 && !P.off && P.cur != P.def) P.set = P.def, menu_sfx_play(MENU_SFX_DECIDE);
        if (a) close = true, menu_sfx_play(MENU_SFX_DECIDE);  // (A keeps it)
        else if (b || clicked == 2) {
            if (clicked == 2) menu_sfx_play(MENU_SFX_CANCEL);
            if (P.cur == P.orig) close = true;
            else P.confirm = true, P.sel = 0;
        }
    }
    if (close && !keep) P.set = P.orig;
    // left / right are the preview's (the menu's navigation would take them too)
    for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
        ImGui::SetKeyOwner(k, ImGui::GetID("##preview_lr"));

    // the page, its rows only read: preview_row gets this frame's change and the row's value and description. In a
    // window out of sight (opt_begin's panes are child windows), with nothing to select.
    {
        static std::vector<OptLine> sink;
        sink.clear();
        ImGui::SetNextWindowPos(ImVec2(-20000, -20000));
        ImGui::SetNextWindowSize(ImVec2(ds.x * 0.8f, ds.y * 0.8f));
        ImGui::Begin("##preview_scan", NULL,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
        P.scanning = true, P.found = false;
        opt_collect_begin(&sink);
        page();
        opt_collect_end();
        P.scanning = false;
        ImGui::End();
        if (!P.found) close = true;  // (the row is gone: back to the page)
    }
    if (close) {
        P.active = false;
        if (P.focus_row >= 0) opt_focus_row(P.focus_row);
        return true;
    }

    // the panel: the menus' framed panel along the bottom, see-through so the game shows - the setting's line, its
    // description, a groove, Return / Reset to default (or the Yes / No to save) as the menus' buttons
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ds);
    ImGui::Begin("##preview", NULL,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const PlasticTheme &pt = plastic_theme();
    ImGui::PushFont(font);
    float fs = ImGui::GetFontSize();
    ImGui::PopFont();
    ImFont *bf = font_big ? font_big : font;
    float big = fs * 1.2f, small = fs * 0.9f, btn = fs * 0.95f, bh = btn * 1.75f;
    float edge = 18 * scale;  // (the panel's margin to the window, as the menu window's)
    float pad = fmaxf(48 * scale, 20.0f);  // (inside the frame)
    float px0 = fmaxf(ds.x * 0.05f, edge), px1 = ds.x - px0;
    float mx = px0 + pad, w = px1 - pad - mx;
    float desc_h = P.desc.empty() ? 0 : font->CalcTextSizeA(small, FLT_MAX, w, P.desc.c_str()).y;
    float bottom = ds.y - edge;
    float y_btn = bottom - pad * 0.7f - bh, y_rule = y_btn - fs * 0.6f, y_desc = y_rule - fs * 0.6f - desc_h;
    float y_line = y_desc - fs * 0.3f - big;
    float top = y_line - pad * 0.6f;
    {   // (the paper at the menu's colours, lighter so the game shows through; its frame solid)
        ImVec4 p1 = pt.panel, p2 = pt.panel2;
        p1.w = p2.w = 0.62f;
        float r = 20 * scale, in = 6 * scale;
        dl->PushClipRectFullScreen();
        plastic_add(dl, ImVec2(px0, top), ImVec2(px1, bottom), r + in, p1, PK_SLAB, 10 * scale, 0.3f, p2, 0, pt.mottle * scale);
        plastic_add(dl, ImVec2(px0 + in, top + in), ImVec2(px1 - in, bottom - in), r, pt.frame, PK_RING, 6 * scale, 1.0f,
                    pt.frame_inner, 10 * scale);
        dl->PopClipRect();
    }
    ImU32 dim = pt.text_dim, off = IM_COL32(150, 138, 116, 200);
    float th = fmaxf(2.6f * scale, 1.5f);
    // the name, the value between ‹ ›
    text(dl, bf, big, ImVec2(mx, y_line), pt.title, i18n_upper(L(P.label.c_str())).c_str());
    {
        float vw = bf->CalcTextSizeA(big, FLT_MAX, 0, P.value.c_str()).x;
        float ah = big * 0.22f, aw = ah * 0.5f, gap = big * 0.55f, cy = y_line + big * 0.55f;
        float rx = mx + w - aw, vx = rx - gap - vw, lx = vx - gap - aw;
        bool can_l = !P.off && (P.wrap || P.cur > 0), can_r = !P.off && (P.wrap || P.cur < P.n - 1);
        float hb = big * 0.5f;  // (the arrows' hit boxes: bigger than they look)
        hit[0][0] = ImVec2(lx - hb, cy - hb), hit[0][1] = ImVec2(lx + aw + hb * 0.6f, cy + hb);
        hit[1][0] = ImVec2(rx - hb * 0.6f, cy - hb), hit[1][1] = ImVec2(rx + aw + hb, cy + hb);
        bool hl = !P.confirm && in_rect(m, hit[0][0], hit[0][1]), hr = !P.confirm && in_rect(m, hit[1][0], hit[1][1]);
        ImU32 hot = ImGui::ColorConvertFloat4ToU32(pt.row_sel);
        chevron(dl, lx, cy, ah, true, !can_l ? off : hl ? hot : pt.arrow, th);
        chevron(dl, rx, cy, ah, false, !can_r ? off : hr ? hot : pt.arrow, th);
        text(dl, bf, big, ImVec2(vx, y_line), P.off ? off : P.cur != P.def ? pt.value_changed : pt.value, P.value.c_str(), true);
    }
    if (!P.desc.empty()) text(dl, font, small, ImVec2(mx, y_desc), dim, P.desc.c_str(), false, w);
    {   // a groove, as under the menus' lists
        dl->AddLine(ImVec2(mx, y_rule), ImVec2(mx + w, y_rule), IM_COL32(0, 0, 0, 110), 1.5f * scale);
        dl->AddLine(ImVec2(mx, y_rule + 1.5f * scale), ImVec2(mx + w, y_rule + 1.5f * scale),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(pt.ring.x, pt.ring.y, pt.ring.z, 0.45f)), 1.2f * scale);
    }
    // a button: a plastic pill (lit: chosen with the controller; the mouse over it lights it too)
    auto button = [&](int i, const char *label, float x, bool right_aligned, bool lit, bool enabled, bool arrow) {
        std::string t = i18n_upper(L(label));
        float tw = font->CalcTextSizeA(btn, FLT_MAX, 0, t.c_str()).x, ax = arrow ? btn * 0.85f : 0;
        float bw = tw + ax + btn * 1.6f, x0 = right_aligned ? x - bw : x;
        hit[i][0] = ImVec2(x0, y_btn), hit[i][1] = ImVec2(x0 + bw, y_btn + bh);
        bool sel = enabled && (lit || in_rect(m, hit[i][0], hit[i][1]));
        ImVec4 col = sel ? pt.row_sel : pt.button;
        if (!enabled) col.w *= 0.5f;
        plastic_pill(dl, hit[i][0], hit[i][1], bh * 0.5f, col, (sel ? 7 : 5) * scale, 1.0f, scale, sel);
        ImU32 c = !enabled ? off : sel ? pt.text_sel : pt.text_button;
        float tx = x0 + btn * 0.8f, ty = y_btn + (bh - btn) * 0.5f - 1 * scale;
        if (arrow) {  // ← before Return
            float cy = ty + btn * 0.55f, ah = btn * 0.24f;
            dl->AddLine(ImVec2(tx, cy), ImVec2(tx + btn * 0.55f, cy), c, th);
            ImVec2 pts[3] = {ImVec2(tx + ah, cy - ah), ImVec2(tx, cy), ImVec2(tx + ah, cy + ah)};
            dl->AddPolyline(pts, 3, c, 0, th);
        }
        dl->AddText(font, btn, ImVec2(tx + ax, ty), c, t.c_str());
        return x0 + bw;
    };
    for (int i = 2; i < 6; i++) hit[i][0] = hit[i][1] = ImVec2(-1, -1);
    if (P.confirm) {
        std::string q = L("Save changes?");
        text(dl, font, btn * 1.05f, ImVec2(mx, y_btn + (bh - btn * 1.05f) * 0.5f), pt.text, q.c_str(), true);
        float x = mx + font->CalcTextSizeA(btn * 1.05f, FLT_MAX, 0, q.c_str()).x + fs * 1.2f;
        x = button(4, LN("Yes"), x, false, P.sel == 0, true, false) + fs * 0.6f;
        button(5, LN("No"), x, false, P.sel == 1, true, false);
    } else {
        button(2, LN("Return"), mx, false, false, true, true);
        button(3, LN("Reset to default"), mx + w, true, false, !P.off && P.cur != P.def, false);
    }
    ImGui::End();
    return false;
}
