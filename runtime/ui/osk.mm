// On-screen keyboard (see osk.h). A controller has no keys to type with, and ImGui only starts text entry on a
// gamepad's Y (A on a field did nothing): A on any of our text fields opens this keyboard instead.
//
// One keyboard at a time (only one ImGui UI runs: start screen or in-game overlay, one thread each). It is a modal
// popup at the root of the UI, so the menu behind sees "a popup is open" and B / Esc / L / R don't act on it.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <string>

#include "imgui.h"
#include "localization.h"
#include "imgui_internal.h"
#include "i18n.h"
#include "osk.h"
#include "pad_nav.h"
#include "plastic.h"
extern "C" {
#include "../platform.h"
}

static struct {
    int open;                        // a field's keyboard is up: 1 the on-screen keys, 2 the system prompt (phones)
    ImGuiID field;                   // the field it types for
    char *buf;                       // ... and its buffer (static in every caller: written on OK)
    size_t size;
    ImGuiInputTextFlags flags;
    char text[128], title[96];
    bool appearing, nav_kbd_off;
    ImGuiID done_field;              // answered: reported by that field's next ui_input_text
    int done, done_frame;            // 1 OK, -1 cancelled; when (a field gone by then never hears of it)
} s;
static std::atomic<bool> s_cancel_req{false};
static bool s_cancelled_now;
static bool s_inline;
static std::atomic<bool> s_pad{false};  // (phones: the last device used - a controller, or the touch screen)

bool osk_is_open(void) { return s.open != 0; }
void osk_cancel(void) { s_cancel_req = true; }
void ui_text_inline(bool on) { s_inline = on; }
void ui_typing_pad(bool pad) { s_pad = pad; }
bool ui_typing_by_pad(void) { return s_pad.load(); }

#if PLAT_TOUCH
static bool prompt_open(const char *title, const char *hint, const char *initial, int max_len) {
    plat_touch_prompt(title, hint, initial, max_len, L("OK"), L("Cancel"));
    return true;
}
static int prompt_poll(char *out, size_t n) { return plat_touch_prompt_poll(out, n); }
static void prompt_cancel(void) { plat_touch_prompt_cancel(); }
#else
static bool prompt_open(const char *, const char *, const char *, int) { return false; }
static int prompt_poll(char *, size_t) { return -1; }
static void prompt_cancel(void) {}
#endif

bool ui_text_busy(void) {
    ImGuiContext *g = ImGui::GetCurrentContext();
    return s.open || (g && (g->IO.WantTextInput || g->WantTextInputNextFrame == 1));
}
bool ui_text_typed_into(void) {  // (not the on-screen keys: they'd bring the system keyboard up beside them)
    ImGuiContext *g = ImGui::GetCurrentContext();
    return !s.open && g && g->IO.WantTextInput;
}

bool ui_input_text_cancelled(void) { return s_cancelled_now; }

static struct { ImVec2 mn, mx; int frame = -1; ImGuiID pending; } s_field;  // (ui_text_field_rect: the active field, the frame to send it)
bool ui_text_field_rect(ImVec2 *mn, ImVec2 *mx) {
    if (s_field.frame != ImGui::GetFrameCount()) return false;
    *mn = s_field.mn, *mx = s_field.mx;
    return true;
}
#if PLAT_TOUCH
static bool any_key_pressed(void) {
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_GamepadStart; k++)
        if (ImGui::IsKeyPressed((ImGuiKey)k, false)) return true;
    return false;
}
#endif

// (test) LARECOMP_FIELD_FOCUS=<label>:<n>[:edit]: the n-th time field <label> is drawn it gets the nav cursor (a
// controller's A - LARECOMP_HOST_INPUT sdlpad:east - then opens the keyboard); :edit starts typing in it instead.
static int test_focus(const char *label) {
    static const char *spec = getenv("LARECOMP_FIELD_FOCUS");
    static int count;
    if (!spec) return 0;
    const char *c = strchr(spec, ':');
    if (!c || strncmp(spec, label, (size_t)(c - spec)) || label[c - spec]) return 0;
    if (++count != atoi(c + 1)) return 0;
    return strstr(c + 1, ":edit") ? 2 : 1;
}

bool ui_input_text(const char *label, char *buf, size_t size, ImGuiInputTextFlags flags, const char *hint, const char *title) {
    ImGuiContext &g = *GImGui;
    int test = test_focus(label);
    if (test == 2) ImGui::SetKeyboardFocusHere();
    bool r = hint ? ImGui::InputTextWithHint(label, ui_tr(hint), buf, size, flags) : ImGui::InputText(label, buf, size, flags);
    ImGuiID id = ImGui::GetItemID();
#if PLAT_TOUCH  // (the field's rect for the system keyboard: menu.mm g_ime_rect; a desktop has no keyboard to clear)
    if (g.ActiveId == id && !s.open && (ImGui::IsItemActivated() || ImGui::IsItemEdited() || any_key_pressed())) s_field.pending = id;
    if (s_field.pending == id && g.ActiveId == id && !s.open) {  // (scrolled out of its pane: back in first, the frame after)
        if (ImGui::IsItemVisible())
            s_field.mn = ImGui::GetItemRectMin(), s_field.mx = ImGui::GetItemRectMax(), s_field.frame = ImGui::GetFrameCount(), s_field.pending = 0;
        else ImGui::SetScrollHereY(0.5f);
    }
#endif
    if (test == 1) { ImGui::FocusItem(); ImGui::SetNavCursorVisible(true); }
    // (and in view for the screenshots for a few frames: the focus' own scroll misses on a NavFlattened page)
    static ImGuiID in_view_id;
    static int in_view;
    if (test) in_view_id = id, in_view = 10;
    if (in_view_id == id && in_view > 0 && in_view--) ImGui::SetScrollHereY(0.5f);
    if (test) fprintf(stderr, "[osk] test: %s field %s\n", test == 2 ? "typing in" : "cursor on", label);
    static std::string last;  // (test runs: what the field holds, as it changes)
    const char *tf = getenv("LARECOMP_FIELD_FOCUS");
    if (tf && !strncmp(tf, label, strlen(label)) && tf[strlen(label)] == ':' && last != buf) { last = buf; fprintf(stderr, "[osk] test: %s = \"%s\"%s\n", label, buf, g.ActiveId == id ? " (typing)" : ""); }
    s_cancelled_now = false;
    if (s.done && s.done_field == id && ImGui::GetFrameCount() - s.done_frame <= 1) {
        if (s.done > 0) r = true;
        else s_cancelled_now = true;
        s.done = 0, s.done_field = 0;
    }
    // activated from the controller: A (ImGui's "activate", which leaves a text field alone), Y (its "input"), or a
    // focus request (SetKeyboardFocusHere) while the controller drives the menu. A mouse click types directly.
    // Phones: whatever activated it, the last device used decides - a controller: the on-screen keys; the touch
    // screen: the system's prompt (the system keyboard typed into an ImGui field covered half the menu), unless the
    // field is inline (a search: filters as it's typed).
    bool by_nav = g.NavActivateId == id || (ImGui::IsItemActivated() && g.ActiveIdSource != ImGuiInputSource_Mouse);
    bool activated = by_nav || ImGui::IsItemActivated();
    int want = 0;  // 1 the on-screen keys, 2 the system prompt
    if (PLAT_TOUCH && activated) want = s_pad.load() ? 1 : s_inline ? 0 : 2;
    else if (by_nav && g.NavInputSource == ImGuiInputSource_Gamepad) want = 1;
    if (s.open && s.field == id && g.ActiveId == id) {  // (activated again under our keyboard: a late focus request)
        ImGui::ClearActiveID();
        g.WantTextInputNextFrame = -1, g.PlatformImeData.WantVisible = false;
    }
    if (want && !s.open && !(flags & ImGuiInputTextFlags_ReadOnly)) {
        if (g.ActiveId == id) ImGui::ClearActiveID();
        // (the field asked for the system keyboard this frame, before it was taken from it: it flashed up for a frame)
        g.WantTextInputNextFrame = -1, g.PlatformImeData.WantVisible = false, s_field.pending = 0;
        s.open = want, s.appearing = true, s.done = 0;
        s.field = id, s.buf = buf, s.size = size, s.flags = flags;
        snprintf(s.text, sizeof s.text, "%s", buf);
        snprintf(s.title, sizeof s.title, "%s", title ? title : hint ? hint : L("Enter text"));
        if (want == 2) prompt_open(s.title, hint ? hint : "", buf, (int)(size < sizeof s.text ? size : sizeof s.text) - 1);
        r = false;
    }
    return r;
}

int osk_keys(char *text, size_t cap, int max_chars, bool appearing, float scale, ImFont *big, bool upper_first, bool touch) {
    static bool upper = true;
    if (appearing) upper = upper_first;
    ImGuiIO &io = ImGui::GetIO();
    // back (B / Circle) deletes: owning it keeps ImGui's nav cancel (which drops the cursor off the keys) from seeing it
    ImGuiID own = ImGui::GetID("##oskkeys");
    ImGui::SetKeyOwner(ImGuiKey_GamepadFaceRight, own);
    if (max_chars <= 0 || max_chars > (int)cap - 1) max_chars = (int)cap - 1;
    int len = (int)strlen(text), r = 0;
    auto add = [&](char ch) { if (len < max_chars) text[len++] = ch, text[len] = 0; };
    auto del = [&]() {  // (a whole character: the text may hold UTF-8 typed before a controller took over)
        while (len > 0 && ((unsigned char)text[len - 1] & 0xc0) == 0x80) text[--len] = 0;
        if (len > 0) text[--len] = 0;
    };

    // typed on a keyboard while it's up: straight into the text
    for (ImWchar c : io.InputQueueCharacters) if (c >= 0x20 && c < 0x7f) add((char)c);
    io.InputQueueCharacters.resize(0);
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) del();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) r = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) r = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)) r = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, ImGuiInputFlags_Repeat, own)) del();

    ImGui::PushFont(big);
    {   // (a long text - a seed ID - wraps onto more lines, its cursor at the end)
        char shown[sizeof s.text + 2];
        snprintf(shown, sizeof shown, "%s_", text);
        ImGui::TextWrapped("%s", shown);
    }
    ImGui::PopFont();
    static const char *rows_u[] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-.'!", "0123456789"};
    static const char *rows_l[] = {"abcdefghij", "klmnopqrst", "uvwxyz-.'!", "0123456789"};
    const char **rows = upper ? rows_u : rows_l;
    float k = 70 * scale;
    if (touch) {  // (ten keys and their gaps across the window: as big as fits, for fingers; five rows fit the height)
        float sp = ImGui::GetStyle().ItemSpacing.x, avail = ImGui::GetContentRegionAvail().x;
        k = fmaxf(k, (avail - 9 * sp) / 10);
        float tall = (ImGui::GetIO().DisplaySize.y * 0.62f - 4 * ImGui::GetStyle().ItemSpacing.y) / 5;
        if (k > tall) k = tall;
    }
    for (int row = 0; row < 4; row++) {
        for (int i = 0; rows[row][i]; i++) {
            if (i) ImGui::SameLine();
            char lbl[8] = {rows[row][i], 0};
            if (appearing && row == 0 && i == 0 && (!touch || s_pad.load())) {  // (cursor shown: else the first A only reveals it)
                ImGui::SetKeyboardFocusHere();
                ImGui::SetNavCursorVisible(true);
            }
            if (ImGui::Button(ui_label(lbl).c_str(), ImVec2(k, k))) add(rows[row][i]);
        }
    }
    if (touch) {  // (the bottom row as wide as the letters: 2 + 2 + 2 + 2 + 2 keys)
        float sp = ImGui::GetStyle().ItemSpacing.x, w2 = k * 2 + sp;
        if (ImGui::Button(upper ? "abc" : "ABC", ImVec2(w2, k))) upper = !upper;
        ImGui::SameLine();
        if (ImGui::Button("Space", ImVec2(w2, k)) && len > 0) add(' ');
        ImGui::SameLine();
        if (ImGui::Button("Delete", ImVec2(w2, k))) del();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(w2, k))) r = -1;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.60f, 0.15f, 0.85f));
        if (ImGui::Button("OK", ImVec2(w2, k))) r = 1;
        ImGui::PopStyleColor();
        if (s_pad.load())
            ImGui::TextDisabled(L("%s  type      %s  delete      %s  OK      (%d / %d)"), L(ui_pad_confirm_label()),
                                L(ui_pad_back_label()), L(ui_pad_start_label()), len, max_chars);
        else ImGui::TextDisabled("Tap the letters, then OK      (%d / %d)", len, max_chars);
        return r;
    }
    if (ImGui::Button(upper ? "abc" : "ABC", ImVec2(k * 2, k))) upper = !upper;
    ImGui::SameLine();
    if (ImGui::Button(L("Space"), ImVec2(k * 3, k)) && len > 0) add(' ');
    ImGui::SameLine();
    if (ImGui::Button(L("Delete"), ImVec2(k * 2, k))) del();
    ImGui::SameLine();
    if (ImGui::Button(L("Cancel"), ImVec2(k * 2, k))) r = -1;
    ImGui::SameLine();
    if (ImGui::Button(L("OK"), ImVec2(k * 2, k))) r = 1;
    ImGui::TextDisabled(L("%s  type      %s  delete      %s  OK      (%d / %d)"), L(ui_pad_confirm_label()),
                        L(ui_pad_back_label()), L(ui_pad_start_label()), len, max_chars);
    return r;
}

// close it (and give the keyboard back to ImGui's navigation)
static void finish(int result) {
    if (result > 0 && s.buf && s.size) {
        if (s.flags & ImGuiInputTextFlags_CharsUppercase)
            for (char *p = s.text; *p; p++) *p = (char)toupper((unsigned char)*p);
        snprintf(s.buf, s.size, "%s", s.text);
    }
    s.done = result, s.done_field = s.field, s.done_frame = ImGui::GetFrameCount();
    s.open = 0, s.buf = NULL;
    if (s.nav_kbd_off) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    s.nav_kbd_off = false;
    // (the field, drawn before this, may have asked for text input this frame: not for the system keyboard)
    if (PLAT_TOUCH) GImGui->WantTextInputNextFrame = -1, GImGui->PlatformImeData.WantVisible = false;
}

void osk_draw(ImFont *big, float scale) {
    bool cancel = s_cancel_req.exchange(false);
    if (!s.open) return;
    ImGuiIO &io = ImGui::GetIO();
    ImGuiContext &g = *GImGui;
    if (s.open == 2) {  // the system prompt: up until answered (its own keyboard; nothing of ours to draw)
        if (cancel) { prompt_cancel(); finish(-1); return; }
        int r = prompt_poll(s.text, sizeof s.text);
        if (r) finish(r);
        return;
    }
    if (!ImGui::IsPopupOpen("##osk")) {
        ImGui::OpenPopup("##osk");
        s.appearing = true;
        // keys typed while it's up are text: Space / Enter / arrows must not also press the focused key
        if (io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) { io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard; s.nav_kbd_off = true; }
    }
    // (typing stays on: the desktop backends keep text input enabled for a hardware keyboard; a phone's text input
    //  brings the system keyboard up beside the keys - ui_text_busy answers from s.open there)
    if (!PLAT_TOUCH) g.WantTextInputNextFrame = 1;
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("##osk", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        finish(-1);
        return;
    }
    ImGui::PushFont(big);
    ImGui::TextColored(plastic_col(plastic_theme().title), "%s", ui_tr(s.title));
    ImGui::PopFont();
    int max = (int)(s.size < sizeof s.text ? s.size : sizeof s.text) - 1;
    // (phones: the keys sized for fingers, as the game's name entry; a tablet's screen takes the plain layout)
    int r = cancel ? -1 : osk_keys(s.text, sizeof s.text, max, s.appearing, scale, big, true, PLAT_TOUCH && !plat_touch_tablet());
    s.appearing = false;
    if (r) {
        ImGui::CloseCurrentPopup();
        finish(r);
    }
    ImGui::EndPopup();
}
