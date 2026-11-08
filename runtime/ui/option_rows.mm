// Two-pane settings pages (see option_rows.h): rows on the left, the selected / hovered row's description on the right.
#include <ctype.h>
#include <float.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"  // (SetKeyOwner, NavCursorVisible)

extern "C" {
#include "../cosmetic_colour.h"
#include "../input_bind.h"
#include "../menu_sfx.h"
#include "../platform.h"
}
#include "i18n.h"
#include "live_preview.h"
#include "option_rows.h"
#include "osk.h"
#include "plastic.h"

static float g_scale = 1;
static std::string g_desc_hover, g_desc_focus, g_desc_shown;  // this frame's hovered / selected row, and the last shown
static std::string g_desc_first;  // the page's first row (shown until a row is selected or hovered)
static bool g_hov_set, g_foc_set;  // (a row was hovered / selected this frame, even one without a description)
static std::string g_left_hover, g_left_focus;  // (opt_split: the left pane's, kept for opt_desc_text)
static bool g_left_hov_set, g_left_foc_set;
static bool g_lr_row, g_focused_lr;  // (the row being drawn steps with left / right; the selected one does)
static float g_row_h = 1.9f;  // (a row's height, in font sizes: opt_compact makes it lower)
static int g_half;  // (opt_pair_next: 1 the next row is a line's left half, 2 its right half)
static float g_desc_w, g_desc_max, g_desc_max_last;  // (opt_list_begin: the description's width; the page's tallest)
static bool g_listed;  // (this page has opt_list_begin's list on the right)
static std::string g_page;  // (the page drawn last: its last shown description isn't carried to another)
// Phones: a page without a right pane is one column of cards (the Android port's 3b4d0504) - the name, its description
// under it, the value with larger arrows on the right - instead of rows with the description in a pane beside them
static bool g_cards;
// Phones: opt_list_begin's list is cards too, filling its pane - no description box under it
static bool g_list_cards;

static bool g_next_disabled;
static std::vector<OptLine> *g_collect;  // (opt_collect_begin: rows are only read, not drawn)
static bool g_row_button;  // (the next row is an action: a button)
void opt_disable_next(void) { g_next_disabled = true; }
static bool g_next_mark, g_next_warn;
void opt_mark_next(void) { g_next_mark = true; }
void opt_warn_next(void) { g_next_warn = true; }
void opt_button_next(void) { g_row_button = true; }
static bool g_next_own;
void opt_own_next(void) { g_next_own = true; }
static int g_indent;
static bool g_focus_req, g_take_first;  // (opt_focus_first: requested / this page's first row takes it)
void opt_focus_first(bool on) { g_focus_req = on; }
static int g_focus_row = -1;  // (opt_focus_row: this page's row with that index takes the cursor)
void opt_focus_row(int i) { g_focus_row = i; }
bool opt_focus_first_left(void) {
    bool left = g_focus_req || g_take_first;
    g_focus_req = g_take_first = false;
    g_focus_row = -1;
    return left;
}
// this frame's rows in order (focus checks): labels, the first enabled one, the focused one
static std::vector<std::string> g_rows;
static std::vector<int> g_row_pane;  // (each row's pane: 0 the left one, 1 opt_split's right one)
static int g_row_first = -1, g_row_focus = -1, g_pane;
int opt_row_count(void) { return (int)g_rows.size(); }
const char *opt_row_label(int i) { return i >= 0 && i < (int)g_rows.size() ? g_rows[i].c_str() : ""; }
int opt_first_row(void) { return g_row_first; }
int opt_focused_row(void) { return g_row_focus; }
void opt_frame_reset(void) { g_rows.clear(), g_row_pane.clear(), g_row_first = g_row_focus = -1; }
// Up / Down between option rows go by the rows' order, not ImGui's spatial pick: that skipped indented sub-rows
// (Rumble -> Controller light past Intensity), and past the last row went up to the tab bar
bool opt_nav_step(int dir) {
    int from = g_row_focus, n = (int)g_row_pane.size();  // (the frame before's rows)
    if (from < 0 || from >= n) return false;  // (the cursor isn't on an option row)
    int to = from + dir;
    while (to >= 0 && to < n && g_row_pane[to] != g_row_pane[from]) to += dir;  // (the same pane)
    if (to < 0 || to >= n) {
        if (dir < 0) return false;  // Up from the top row: out of the page (ImGui: the tab bar / controls above)
        to = from;                  // Down from the last row: stays
    }
    g_focus_row = to;
    return true;
}
static std::unordered_map<ImGuiID, int> g_ids;  // this page's rows so far, by id (two rows with one label)
static bool g_held;  // the last row's left / right has been held a while (steppers go faster)
static bool g_click_silent;  // this frame's click did nothing (a finger on a ◀ value ▶ row's body): no decide sound
void opt_indent(int step) { g_indent = g_indent + step < 0 ? 0 : g_indent + step; }

void opt_begin(const char *id, float scale, bool split) {
    g_scale = scale;
    g_take_first = g_focus_req;
    g_focus_req = false;
    if (g_take_first) ImGui::NavMoveRequestCancel();  // (this frame's Down goes to the first row instead)
    g_indent = 0;
    g_desc_hover.clear(), g_desc_focus.clear();
    g_hov_set = g_foc_set = g_left_hov_set = g_left_foc_set = false;
    g_row_h = 1.9f, g_listed = false;
    g_desc_max_last = g_desc_max, g_desc_max = 0;
    if (g_page != id) g_page = id, g_desc_shown.clear(), g_desc_max_last = 0, g_desc_w = 0;
    g_cards = PLAT_TOUCH && !split;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float pane_w = g_cards ? avail.x : avail.x * 0.48f;
    {   // the rows' pane: a recessed well
        ImVec2 a = ImGui::GetCursorScreenPos();
        float m = 6 * scale;
        plastic_well(ImGui::GetWindowDrawList(), ImVec2(a.x - m, a.y - m), ImVec2(a.x + pane_w + m, a.y + avail.y + m), scale);
    }
    ImGui::PushID(id);
    ImGui::BeginChild("##rows", ImVec2(pane_w, avail.y), ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, (g_cards ? 10 : 4) * scale));  // (rows close)
    g_desc_first.clear();
    g_ids.clear();
    g_pane = 0;
}

void opt_section(const char *title) {
    if (g_collect) return;
    std::string t = i18n_upper(L(title));
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1) ImGui::Dummy(ImVec2(0, 10 * g_scale));
    ImGui::TextColored(plastic_col(plastic_theme().section), "%s", t.c_str());
}

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A / click), -1 / +1 (left / right) or 0.
// arrows: ◀ value ▶ (a choice of several, or a stepper): left / right step it, and the arrows click too
// chevron: a ">" on the right (opt_link)
// The label, value and description are English (i18n.h): shown in the menus' language. The label stays the row's id.
static bool g_value_shown;  // (the next row's value is already in the menus' language: a number put in L(fmt), a name)
static bool g_label_shown;  // (opt_shown_next: the label too)
void opt_shown_next(void) { g_value_shown = g_label_shown = true; }
static float g_value_x, g_value_cy;
void opt_last_value_pos(float *x, float *cy) { *x = g_value_x, *cy = g_value_cy; }
static int row(const char *label, const std::string &value_en, bool changed, const char *desc_en, bool upper = true,
               bool arrows = false, bool chevron = false) {
    const std::string &value = g_value_shown ? value_en : L_line(value_en);
    const char *desc = L(desc_en);
    const char *label_shown = g_label_shown ? label : L(label);
    // (phones: a row with a description is a card wherever it is - a two-pane page's left rows too)
    bool lr = arrows || g_lr_row, cards = g_cards || g_list_cards || (PLAT_TOUCH && desc && *desc);
    g_lr_row = false;
    g_value_shown = g_label_shown = false;
    if (g_collect) {
        if (!g_next_disabled && !g_next_own && !value.empty()) g_collect->push_back({label_shown, value, changed, g_indent});
        g_next_disabled = g_next_mark = g_next_warn = g_row_button = g_next_own = false;
        g_half = 0;
        return 0;
    }
    // a label a row above already has (Save Editor: Bombs, Magic Powder under Equipment and Ammo) gets an id of its
    // own: two rows with one id share the controller's cursor - ImGui kept the later one's place, so Up / Down from
    // the first went on from the second (a row far below, or the edge of the view)
    int dup = g_ids[ImGui::GetID(label)]++;
    ImGui::PushID(label);
    if (dup) ImGui::PushID(dup);
    int half = g_half;
    g_half = half == 1 ? 2 : 0;
    if (half == 2) ImGui::SameLine();
    else if (g_indent) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + g_indent * 28 * g_scale);  // (an option of the row above)
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFontSize() * g_row_h;
    if (half == 1) w = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    // a card: the description under the name, wrapped clear of the value (its arrows / chevron included); a value
    // wider than half the card goes on a line of its own under the description, and the description takes the width
    const float dscale = 0.8f, cfs = ImGui::GetFontSize();
    float card_pad = 24 * g_scale, desc_w = 0, desc_h = 0;
    bool value_below = false;
    if (cards) {
        float vw = value.empty() ? 0 : ImGui::CalcTextSize(value.c_str()).x;
        if (arrows && vw > 0) vw += 2 * (cfs * 0.6f + cfs * 0.55f);
        if (chevron) vw += cfs * 0.75f;
        float room = w - 2 * card_pad - 16 * g_scale - 14 * g_scale;  // (the value's own right pad)
        value_below = vw > 0 && vw + cfs > room * 0.5f && desc && *desc;
        desc_w = value_below || vw <= 0 ? room : room - vw - cfs;
        if (desc && *desc) desc_h = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize() * dscale, FLT_MAX, desc_w, desc).y;
        h = ImGui::GetFontSize() * 1.3f + desc_h + 26 * g_scale + (value_below ? cfs * 1.4f : 0);
    }
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the row's own border shows the selection)
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool off = g_next_disabled;  // (greyed out: still selectable, so its description shows, but it can't change)
    if (g_take_first && !off) {  // (opt_focus_first: the first row that isn't greyed out)
        g_take_first = false;
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        // the page from its top when the row is in its first screen (its section title shows), else the row at the top
        if (ImGui::GetCursorPosY() < ImGui::GetWindowHeight()) ImGui::SetScrollY(0);
        else ImGui::SetScrollHereY(0);
    }
    if (g_focus_row >= 0 && g_focus_row == (int)g_rows.size()) {  // (opt_focus_row / opt_nav_step)
        g_focus_row = -1;
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
    }
    if (!off && g_row_first < 0) g_row_first = (int)g_rows.size();
    if (ImGui::IsItemFocused()) g_row_focus = (int)g_rows.size();
    g_rows.push_back(dup ? std::string(label) + " #" + std::to_string(dup + 1) : std::string(label));
    g_row_pane.push_back(g_pane);
    bool mark = g_next_mark, warn = g_next_warn;
    g_next_disabled = g_next_mark = g_next_warn = g_next_own = false;
    bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    // (phones: a finger on a ◀ value ▶ row is only on its arrows - the body neither lights up nor steps; the
    // controller still focuses and steps the row)
    bool tap_steps = !(PLAT_TOUCH && arrows && ImGui::GetIO().MouseSource == ImGuiMouseSource_TouchScreen);
    float arrow_lx = 0, arrow_rx = 0, arrow_aw = 0, arrow_hit = 0;
    if (arrows && !value.empty()) {  // (the arrows' places, as drawn below)
        float fs0 = ImGui::GetFontSize(), aw = fs0 * (cards ? 0.6f : 0.42f), gap = fs0 * 0.55f;
        float right0 = p.x + w - 16 * g_scale - 14 * g_scale;
        arrow_rx = right0 - aw, arrow_lx = arrow_rx - gap - ImGui::CalcTextSize(value.c_str()).x - gap - aw, arrow_aw = aw;
        arrow_hit = tap_steps ? gap : fs0 * 1.2f;  // (a finger: wider targets)
    }
    int arrow_under = 0;  // (the arrow the mouse is over: -1 / +1)
    if (hov && arrows && !value.empty()) {
        float mx = ImGui::GetIO().MousePos.x;
        if (mx >= arrow_lx - arrow_hit && mx <= arrow_lx + arrow_aw + arrow_hit) arrow_under = -1;
        else if (mx >= arrow_rx - arrow_hit && mx <= arrow_rx + arrow_aw + arrow_hit) arrow_under = 1;
    }
    if (!tap_steps && !arrow_under) hov = false;  // (the finger on the body: not a highlight)
    // (nor a press: no sound - menu_sfx_frame plays on the press frame, before the button reports the click)
    if (!tap_steps && !arrow_under && ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) g_click_silent = true;
    int step = clicked && !off && tap_steps ? 2 : 0;
    if (clicked && off) menu_sfx_play(MENU_SFX_BEEP);  // (greyed out: can't change)
    g_held = false;
    if (foc && !off) {
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft) ||
            ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight) ||
            ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            step = 1;
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickLeft,
                           ImGuiKey_GamepadLStickRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            if (ImGui::GetKeyData(k)->DownDuration > 1.0f) g_held = true;
        // left / right belong to the row (navigation would otherwise try to move sideways)
        for (ImGuiKey k : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow, ImGuiKey_RightArrow})
            ImGui::SetKeyOwner(k, ImGui::GetItemID());
    }
    if (foc) g_desc_focus = desc ? desc : "", g_foc_set = true, g_focused_lr = lr && !off;
    if (hov) g_desc_hover = desc ? desc : "", g_hov_set = true;
    if (g_desc_first.empty() && desc) g_desc_first = desc;
    if (desc && *desc && g_desc_w > 0) {  // (opt_list_begin's room for the description: the page's tallest)
        float fs13 = ImGui::GetFontSize() * 1.3f;
        float dh = ImGui::GetFont()->CalcTextSizeA(fs13, FLT_MAX, g_desc_w, desc).y;
        if (dh > g_desc_max) g_desc_max = dh;
    }

    // the row: a plastic pill in its rim; selected (cursor or mouse), it stands up in the selection colour (text_sel).
    // Actions are buttons (text_button), warnings red ones (a red row too: a duplicate binding).
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const PlasticTheme &pt = plastic_theme();
    float pad = 16 * g_scale, fs = ImGui::GetFontSize();
    bool sel = hov || foc, button = g_row_button;
    g_row_button = false;
    ImVec4 col = sel ? pt.row_sel : mark ? pt.row_mark : warn ? pt.danger : button ? pt.button : pt.row;
    if (off) col = sel ? pt.row_mark : ImVec4(col.x, col.y, col.z, col.w * 0.5f);  // (greyed out, the cursor on it: ochre)
    float dep = (sel ? 7 : button ? 6 : 3.5f) * g_scale;
    plastic_pill(dl, ImVec2(p.x + 3 * g_scale, p.y + 1 * g_scale), ImVec2(p.x + w - 8 * g_scale, p.y + h - 3 * g_scale),
                 cards ? fminf(h * 0.5f, fs * 0.9f) : h * 0.5f, col, dep, sel || button ? 1.0f : 0.6f, g_scale, sel && !off);
    float lpad = cards ? card_pad : h * 0.66f;  // (the label clear of the pill's rounded end)
    std::string t = upper && !cards ? i18n_upper(label_shown) : std::string(label_shown);
    float ty = p.y + (h - fs) * 0.5f - 1 * g_scale;
    if (value_below) ty = p.y + h - fs * 1.4f - 12 * g_scale;  // (the value's own line, at the card's foot)
    bool light = !off && (sel || button);
    ImU32 on_col = sel ? pt.text_sel : pt.text_button;  // (text on the selection / on a button)
    ImU32 label_col = off ? pt.text_dim : light ? on_col : pt.text;
    if (cards) {  // (the label at the card's top, its description under it; a row's label: below, clear of the value)
        float top = p.y + 12 * g_scale;
        dl->AddText(ImVec2(p.x + lpad, top), label_col, t.c_str());
        if (desc_h > 0)
            dl->AddText(ImGui::GetFont(), fs * dscale, ImVec2(p.x + lpad, top + fs * 1.2f),
                        (label_col & 0x00FFFFFFu) | ((off ? 0x90u : 0xB8u) << 24), desc, NULL, desc_w);
    }
    float right = p.x + w - pad - 14 * g_scale;  // (the value's right edge)
    ImU32 ac = off ? pt.text_dim : light ? on_col : pt.arrow;  // (arrows, chevron)
    if (arrows && !value.empty()) {  // ◀ value ▶, drawn (the fonts have no arrow glyphs); a click on one steps
        float aw = fs * (cards ? 0.6f : 0.42f), gap = fs * 0.55f, cy = ty + fs * 0.52f;
        float rx = right - aw, lx = rx - gap - ImGui::CalcTextSize(value.c_str()).x - gap - aw;
        dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + aw, cy - aw * 0.75f), ImVec2(lx + aw, cy + aw * 0.75f), ac);
        dl->AddTriangleFilled(ImVec2(rx + aw, cy), ImVec2(rx, cy - aw * 0.75f), ImVec2(rx, cy + aw * 0.75f), ac);
        if (clicked && !off && arrow_under) step = arrow_under;
        right = rx - gap;
    }
    if (chevron) {  // ">", drawn (as the arrows above)
        float cw = fs * 0.2f, cy = ty + fs * 0.52f;
        const ImVec2 pts[] = {ImVec2(right - cw, cy - cw * 1.1f), ImVec2(right, cy), ImVec2(right - cw, cy + cw * 1.1f)};
        dl->AddPolyline(pts, 3, ac, 0, 2.6f * g_scale);
        right -= cw + fs * 0.55f;
        if (step == 1) step = 2, menu_sfx_play(MENU_SFX_DECIDE);  // (Right opens it too)
    }
    g_value_x = 0;
    if (!value.empty()) {
        float vw = ImGui::CalcTextSize(value.c_str()).x, vx = right - vw;
        ImU32 vc = off ? pt.text_dim : light ? on_col : changed ? pt.value_changed : pt.value;
        dl->AddText(ImVec2(vx, ty), vc, value.c_str());
        right = vx;
        g_value_x = vx, g_value_cy = ty + fs * 0.52f;
    }
    if (!cards) {  // the label, clear of the value: a long one (German, French) a little smaller, then cut short with "..."
        float room = right - (p.x + lpad) - fs * (value.empty() && !chevron && !arrows ? 0.2f : 0.7f);
        float lw = ImGui::CalcTextSize(t.c_str()).x, lfs = fs;
        if (lw > room && room > 0) lfs = fmaxf(fs * 0.78f, fs * room / lw);
        float lty = ty + (fs - lfs) * 0.55f;
        if (lw * lfs / fs <= room || room <= 0) {
            dl->AddText(ImGui::GetFont(), lfs, ImVec2(p.x + lpad, lty), label_col, t.c_str());
        } else {
            const char *end = t.c_str() + t.size();
            float ew = ImGui::GetFont()->CalcTextSizeA(lfs, FLT_MAX, 0, "...").x;
            while (end > t.c_str() && ImGui::GetFont()->CalcTextSizeA(lfs, FLT_MAX, 0, t.c_str(), end).x + ew > room) {
                do end--; while (end > t.c_str() && (*end & 0xC0) == 0x80);  // (a whole UTF-8 character)
            }
            std::string cut = std::string(t.c_str(), end) + "...";
            static std::unordered_map<std::string, bool> cut_logged;  // (test: names too long for their row, once each)
            bool &logged = cut_logged[t];
            if (!logged && getenv("LARECOMP_I18N_MISSING")) logged = true, fprintf(stderr, "[i18n] cut: \"%s\"\n", t.c_str());
            dl->AddText(ImGui::GetFont(), lfs, ImVec2(p.x + lpad, lty), label_col, cut.c_str());
        }
    }
    if (dup) ImGui::PopID();
    ImGui::PopID();
    return step;
}

// a value stepped with left / right: the cursor sound (A / click: menu_sfx_frame's decide)
static void step_sfx(int step) {
    if (step == 1 || step == -1) menu_sfx_play(MENU_SFX_CURSOR);
}

bool opt_toggle(const char *label, bool *v, bool def, const char *desc) {
    g_lr_row = true;  // (left / right flip it)
    int step = row(label, *v ? LN("On") : LN("Off"), *v != def, desc);
    if (!step) return false;  // (any step flips it)
    step_sfx(step);
    *v = !*v;
    return true;
}

bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap) {
    return opt_preview(label, v, names, n, def, desc, wrap, false);
}

// (opt_choice, with preview: A / a click on the row opens the live preview instead of stepping)
bool opt_preview(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap,
                 bool preview) {
    bool scan = preview && preview_scanning(), off = g_next_disabled;
    int cur = *v >= 0 && *v < n ? *v : 0;
    // (preview: a chevron - A / a click / Right opens it, the value doesn't step here)
    int step = row(label, names[cur], cur != def, desc, true, !preview, preview);
    if (scan) return preview_row(label, v, names, n, def, desc, wrap, off);  // (row: only read, as the others)
    if (preview) {
        if (step == 2 && !g_collect) preview_open(label, opt_item_nav_focused() ? (int)g_rows.size() - 1 : -1);
        return false;
    }
    if (!step) return false;
    int nv = cur + (step == 2 ? 1 : step);
    nv = wrap && step == 2 ? nv % n : nv < 0 ? 0 : nv >= n ? n - 1 : nv;
    if (nv == cur) return false;
    step_sfx(step);
    *v = nv;
    return true;
}

bool opt_slider(const char *label, float *v, float lo, float hi, float step, float def, const char *fmt, const char *desc) {
    char buf[64];
    snprintf(buf, sizeof buf, L(fmt), *v);
    g_value_shown = true;
    int s = row(label, buf, fabsf(*v - def) > step * 0.01f, desc, true, true);
    if (!s) return false;
    float eps = step * 0.01f, n = roundf(*v / step) * step + (s == 2 ? step : s * step);
    if (s == 2 && n > hi + eps) n = lo;  // (A / click past the top wraps to the bottom; left / right stop at the ends)
    n = n < lo ? lo : n > hi ? hi : n;
    if (fabsf(n - *v) > eps) step_sfx(s);
    *v = n;
    return true;
}

bool opt_int(const char *label, int *v, int lo, int hi, int step, int def, const char *fmt, const char *desc,
             const char *lo_name) {
    char buf[96];
    if (lo_name && *v == lo) snprintf(buf, sizeof buf, "%s", L(lo_name));
    else snprintf(buf, sizeof buf, L(fmt), *v);
    g_value_shown = true;
    int s = row(label, buf, *v != def, desc, true, true);
    if (!s) return false;
    int d = s == 2 ? step : s * step * (g_held && hi - lo > 40 * step ? 10 : 1);
    int n = *v + d;
    if (s == 2 && n > hi) n = lo;  // (A / click past the top wraps; left / right stop at the ends)
    n = n < lo ? lo : n > hi ? hi : n;
    if (n == *v) return false;
    step_sfx(s);
    *v = n;
    return true;
}

bool opt_button(const char *label, const char *desc) {
    g_row_button = true;
    return row(label, "", false, desc) == 2;
}

bool opt_radio(const char *label, bool selected, const char *desc) {
    bool off = g_next_disabled;
    bool chosen = row(label, "", false, desc) == 2;
    if (g_collect) return false;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float r = ImGui::GetFontSize() * 0.32f, cx = b.x - 16 * g_scale - r, cy = (a.y + b.y) * 0.5f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const PlasticTheme &pt = plastic_theme();
    bool lit = ImGui::IsItemHovered() || opt_item_nav_focused();
    ImU32 col = off ? pt.text_dim : lit ? pt.text_sel : pt.arrow;
    dl->AddCircle(ImVec2(cx, cy), r, col, 0, 2.2f * g_scale);
    if (selected) dl->AddCircleFilled(ImVec2(cx, cy), r * 0.55f, lit ? pt.text_sel : ImGui::ColorConvertFloat4ToU32(pt.row_sel));
    return chosen && !selected;
}

bool opt_action(const char *label, const char *value, const char *desc, bool upper, bool changed) {
    g_row_button = g_row_button || ((!value || !*value) && upper);  // (a plain action: a button)
    return row(label, value ? value : "", changed, desc, upper) == 2;
}

bool opt_colour(const char *label, CosColour *c, int slot, const char *desc) {
    int prev = c->pick;
    const char *names[N_COL];
    for (int i = 0; i < N_COL; i++) names[i] = g_col_names[i];
    (void)slot;
    bool changed = opt_choice(label, &c->pick, names, N_COL, COL_DEFAULT, desc);
    if (g_collect) return false;
    if (changed && c->pick == COL_CUSTOM && prev != COL_CUSTOM)  // (Custom starts from the colour it was on)
        c->rgb = prev > COL_DEFAULT && prev < COL_RANDOM ? g_col_rgb[prev] : (c->rgb ? c->rgb : 0xF2F0EA);
    if (c->pick != COL_CUSTOM) return changed;
    ImGui::PushID(label);
    char hex[16];
    snprintf(hex, sizeof hex, "#%06X", (unsigned)(c->rgb & 0xFFFFFF));
    g_value_shown = true;
    bool open = opt_action("Custom color", hex, "Pick any color by hue, saturation and brightness.", true, true);
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    {   // a swatch of it, left of the value
        float fs = ImGui::GetFontSize(), sw = fs * 1.1f, vw = ImGui::CalcTextSize(hex).x;
        float x1 = mx.x - 30 * g_scale - vw - fs * 0.5f, y0 = (mn.y + mx.y - sw) * 0.5f;
        ImU32 col = IM_COL32(c->rgb >> 16 & 255, c->rgb >> 8 & 255, c->rgb & 255, 255);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x1 - sw, y0), ImVec2(x1, y0 + sw), col, 4 * g_scale);
        ImGui::GetWindowDrawList()->AddRect(ImVec2(x1 - sw, y0), ImVec2(x1, y0 + sw), plastic_theme().text, 4 * g_scale,
                                            0, 1.5f * g_scale);
    }
    static int test_open = -1;  // LARECOMP_OPEN_PICKER=1 (test): the first Custom row's picker opens by itself
    if (test_open < 0) test_open = getenv("LARECOMP_OPEN_PICKER") != NULL;
    if (test_open) test_open = 0, open = true;
    if (open) ImGui::OpenPopup("##picker");
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30 * g_scale, 30 * g_scale));  // (inside the panel's frame)
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 9 * g_scale);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::SetNextWindowPos(ImVec2(mx.x + 12 * g_scale, mn.y), ImGuiCond_Appearing);  // (beside the row, over the description)
    if (ImGui::BeginPopup("##picker")) {
        float f[3] = {(c->rgb >> 16 & 255) / 255.0f, (c->rgb >> 8 & 255) / 255.0f, (c->rgb & 255) / 255.0f};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
        if (ImGui::ColorPicker3("##colour", f, ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview |
                                                   ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_NoAlpha)) {
            c->rgb = (uint32_t)lroundf(f[0] * 255) << 16 | (uint32_t)lroundf(f[1] * 255) << 8 | (uint32_t)lroundf(f[2] * 255);
            changed = true;
        }
        if (ImGui::Button(L("Done"), ImVec2(ImGui::GetFontSize() * 11, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    return changed;
}

bool opt_link(const char *label, const char *desc, const char *value) {
    return row(label, value ? value : "", false, desc, true, false, true) == 2;
}

static ImGuiID g_text_editing;  // the text row turned into a field (one at a time)
bool opt_text_editing(const char *label) { return g_text_editing && g_text_editing == ImGui::GetID(label); }

bool opt_text(const char *label, char *buf, size_t cap, const char *hint, const char *desc) {
    ImGuiID &editing = g_text_editing;
    static int focus;        // frames left to give it the keyboard focus
    ImGuiID id = ImGui::GetID(label);
    if (editing != id || g_collect) {
        g_value_shown = true;  // (what was typed, as typed)
        if (row(label, buf[0] ? buf : (hint ? L(hint) : ""), false, desc) == 2) editing = id, focus = 2;
        return false;
    }
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1);
#if PLAT_TOUCH
    // (a keyboard of ours or the system's prompt took the field: no more focus requests - the field would come back
    //  to life under them, and the system keyboard with it)
    if (osk_is_open()) focus = 0;
#endif
    if (focus > 0) ImGui::SetKeyboardFocusHere(), focus--;
    bool done = ui_input_text("##text", buf, cap, ImGuiInputTextFlags_EnterReturnsTrue, L(hint), L(label));
    if (ImGui::IsItemFocused()) g_desc_focus = desc ? L(desc) : "";
    if (done || ui_input_text_cancelled() || (focus == 0 && !ImGui::IsItemActive() && !osk_is_open())) editing = 0;
#if PLAT_TOUCH
    if (editing == 0 && ImGui::IsItemActive()) ImGui::ClearActiveID();  // (done under the prompt: the system keyboard would stay)
#endif
    ImGui::PopID();
    return done;
}

// the left pane's end: its child window, and the description the right pane shows
static void left_end(void) {
    ImGui::PopStyleVar();
    if (const char *t = getenv("LARECOMP_OPT_SCROLL")) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(t));  // (test)
    ImGui::EndChild();
    // the description: the hovered row (mouse) wins, else the selected one (controller), else the last shown
    if (!g_desc_hover.empty()) g_desc_shown = g_desc_hover;
    else if (!g_desc_focus.empty()) g_desc_shown = g_desc_focus;
    else if (g_desc_shown.empty()) g_desc_shown = g_desc_first;
}

// The right stick scrolls the right pane while it only shows something to read (a description, the Randomizer's
// summary) and is longer than the pane; a pane with rows to pick (this frame) keeps the stick still. Speed grows with
// the tilt, past a small dead zone. Called at the pane's end, inside its child window.
static bool any_nav_items(ImGuiWindow *w) {
    if (w->DC.NavLayersActiveMaskNext & 1) return true;
    for (ImGuiWindow *c : w->DC.ChildWindows)
        if (c->Active && any_nav_items(c)) return true;
    return false;
}
static void stick_scroll(void) {
    ImGuiWindow *w = ImGui::GetCurrentWindow();
    if (w->ScrollMax.y <= 0 || any_nav_items(w)) return;
    float v = ImGui::GetKeyData(ImGuiKey_GamepadRStickDown)->AnalogValue - ImGui::GetKeyData(ImGuiKey_GamepadRStickUp)->AnalogValue;
    const float dead = 0.18f;
    float a = fabsf(v);
    if (a <= dead) return;
    float t = fminf((a - dead) / (1 - dead), 1.0f);
    float speed = ImGui::GetFontSize() * (2.0f + 26.0f * t * t);  // (pixels a second: a slow crawl to a quick run)
    float dy = speed * ImGui::GetIO().DeltaTime * (v > 0 ? 1 : -1);
    ImGui::SetScrollY(w, ImClamp(w->Scroll.y + dy, 0.0f, w->ScrollMax.y));
}

// the right pane's child window, in a well beside the left one
static void right_begin(const char *id, ImGuiWindowFlags flags) {
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    float gap = 24 * g_scale, m = 6 * g_scale;  // (the right pane: a well, as the rows')
    plastic_well(ImGui::GetWindowDrawList(), ImVec2(p.x + gap - m, p.y - m), ImVec2(p.x + avail.x + m, p.y + avail.y + m), g_scale);
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * g_scale, 6 * g_scale));
    ImGui::BeginChild(id, ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, flags);
    ImGui::PopStyleVar();
}

void opt_desc_text(void) {
    if (PLAT_TOUCH) return;  // (phones: each card has its own)
    // after opt_list_begin's rows: a row of the right pane hovered wins, then the left's hovered, the right's selected,
    // the left's selected (other pages: the left's, as left_end picked)
    if (g_listed) {
        if (g_hov_set) g_desc_shown = g_desc_hover;
        else if (g_left_hov_set) g_desc_shown = g_left_hover;
        else if (g_foc_set) g_desc_shown = g_desc_focus;
        else if (g_left_foc_set) g_desc_shown = g_left_focus;
        else if (g_desc_shown == g_desc_first) g_desc_shown.clear();  // (nothing chosen yet: none)
    }
    ImGui::SetWindowFontScale(1.3f);  // (the description reads at a glance)
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_desc_shown.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1.0f);
}

void opt_end(void) {
    left_end();
    if (g_cards) { ImGui::PopID(); return; }  // (the descriptions are in the cards)
    right_begin("##desc", ImGuiWindowFlags_NoNav);
    opt_desc_text();
    stick_scroll();
    ImGui::EndChild();
    ImGui::PopID();
}

void opt_split(void) {
    IM_ASSERT(!g_cards && "opt_begin(id, scale, true) for a page with opt_split");
    left_end();
    g_left_hover = g_desc_hover, g_left_focus = g_desc_focus, g_left_hov_set = g_hov_set, g_left_foc_set = g_foc_set;
    g_desc_hover.clear(), g_desc_focus.clear();
    g_hov_set = g_foc_set = false;
    right_begin("##right", ImGuiWindowFlags_None);
    g_pane = 1;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 4 * g_scale));  // (as the left)
    g_indent = 0;
}

void opt_split_end(void) {
    ImGui::PopStyleVar();
    stick_scroll();
    ImGui::EndChild();
    ImGui::PopID();
}

void opt_compact(void) { g_row_h = 1.4f; }
void opt_pair_next(void) { g_half = 1; }

void opt_list_begin(void) {
    // room under the list for the page's longest description (as last frame measured it; 2 lines at least): a longer
    // list scrolls
    // The width the description wraps at, measured as if the pane had a scrollbar whether or not it has one now: a
    // width that followed the scrollbar re-wrapped the text, which moved the list, which toggled the scrollbar - the
    // rows and the text flashed every frame (Bushsanity's three lines)
    ImGuiWindow *win = ImGui::GetCurrentWindow();
    g_desc_w = ImGui::GetContentRegionAvail().x - (win->ScrollbarY ? 0 : ImGui::GetStyle().ScrollbarSize), g_listed = true;
    float line = ImGui::GetFontSize() * 1.3f, avail = ImGui::GetContentRegionAvail().y;
    // (+ opt_list_end's groove and the spacing around it, so the pane itself never needs to scroll)
    float desc = (g_desc_max_last > line * 2 ? g_desc_max_last : line * 2) + 14 * g_scale +
                 2 * ImGui::GetStyle().ItemSpacing.y + 4 * g_scale;
    float maxh = avail - desc > avail * 0.35f ? avail - desc : avail * 0.35f;
    if (PLAT_TOUCH) {  // (the descriptions are in the cards: the list has the whole pane)
        g_list_cards = true;
        ImGui::BeginChild("##list", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 10 * g_scale));
        return;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxh));
    ImGui::BeginChild("##list", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
}

void opt_list_end(void) {
    if (g_list_cards) {
        g_list_cards = false;
        ImGui::PopStyleVar();
        ImGui::EndChild();
        return;
    }
    ImGui::EndChild();
    // a groove between the list and the description under it
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, y = p.y + 6 * g_scale;
    const PlasticTheme &pt = plastic_theme();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(p.x + 4 * g_scale, y), ImVec2(p.x + w - 4 * g_scale, y), IM_COL32(0, 0, 0, 110), 1.5f * g_scale);
    dl->AddLine(ImVec2(p.x + 4 * g_scale, y + 1.5f * g_scale), ImVec2(p.x + w - 4 * g_scale, y + 1.5f * g_scale),
                ImGui::ColorConvertFloat4ToU32(ImVec4(pt.ring.x, pt.ring.y, pt.ring.z, 0.45f)), 1.2f * g_scale);
    ImGui::Dummy(ImVec2(0, 14 * g_scale));
}

void opt_focus_next_row(void) { g_take_first = true; }

bool opt_focused_lr(void) { return g_focused_lr; }

bool opt_in_right(void) { return g_row_focus >= 0 && g_row_focus < (int)g_row_pane.size() && g_row_pane[g_row_focus] == 1; }

void opt_focus_item(void) {
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
    ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
}

bool opt_item_nav_focused(void) { return ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible; }

bool opt_pressed_back(void) {
    return ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
}

bool opt_pressed_left(void) {
    return ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft) ||
           ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
}

static int g_collect_indent;  // (the pane's indent, kept while the rows are read)
void opt_collect_begin(std::vector<OptLine> *out) { g_collect = out, g_collect_indent = g_indent, g_indent = 0; }
bool opt_collecting(void) { return g_collect != NULL; }
void opt_collect_end(void) { g_collect = NULL, g_indent = g_collect_indent; }

// The menu's sounds this frame (menu_sfx.h): what the controller's cursor and the presses did. Value steps, greyed-out
// rows (above), tab switches and the menu opening / closing (menu.mm) play theirs where they happen.
static ImGuiID g_tab_ids[16];  // (the menu tabs drawn this frame)
static int g_ntab_ids;
extern "C" void menu_sfx_tab_item(void) {
    if (g_ntab_ids < 16) g_tab_ids[g_ntab_ids++] = ImGui::GetItemID();
}
static bool is_tab(ImGuiID id) {
    for (int i = 0; i < g_ntab_ids; i++)
        if (g_tab_ids[i] == id) return true;
    return false;
}

// (a mouse click on a scroll bar plays nothing)
static bool on_scrollbar(ImGuiContext &g) {
    ImGuiWindow *w = g.HoveredWindow;
    return w && (g.HoveredId == ImGui::GetWindowScrollbarID(w, ImGuiAxis_Y) ||
                 g.HoveredId == ImGui::GetWindowScrollbarID(w, ImGuiAxis_X));
}

extern "C" void menu_sfx_frame(bool back_ok) {
    ImGuiContext &g = *ImGui::GetCurrentContext();
    bool silent = g_click_silent;
    g_click_silent = false;
    static ImGuiContext *ctx;  // (the start screen's and the game's overlay are two)
    static ImGuiID last;
    if (ctx != &g) ctx = &g, last = 0;
    int cap = -1;
    bind_capture_active(&cap, NULL, NULL);
    if (!ui_text_busy() && cap < 0) {
        if (back_ok && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            menu_sfx_play(MENU_SFX_CANCEL);
        else if (g.NavActivatePressedId || (ImGui::IsMouseClicked(0) && g.HoveredId && !on_scrollbar(g) && !silent))
            menu_sfx_play(is_tab(g.NavActivatePressedId ? g.NavActivatePressedId : g.HoveredId) ? MENU_SFX_TAB
                                                                                                : MENU_SFX_DECIDE);
        else if (g.NavCursorVisible && g.NavId && last && g.NavId != last)
            menu_sfx_play(MENU_SFX_CURSOR);
    }
    last = g.NavId;
    g_ntab_ids = 0;
    menu_sfx_idle_check();
}
