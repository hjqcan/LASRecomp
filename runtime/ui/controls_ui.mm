// The settings' Controls tab (in game: menu.mm; start screen: launcher.mm, both through menu_tabs_draw), in option rows
// (runtime/ui/option_rows.h), and its Configure inputs page for the bindings (runtime/input_bind.h): DEVICE and the
// categories on the left, the chosen category's bindings on the right. A on a category moves the cursor to its
// bindings; A on a binding waits for the new button / key (Esc or 5 s without a press cancels; a button already in
// use swaps over); B goes back a step (bindings -> categories -> the tab). Open menu holds the controller's shortcuts for
// this menu (one button or two together; X / Delete clears one, never the last) and the keyboard's F1 / Esc, which
// can't be changed. Drawn in whichever ImGui context is current.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"  // (SetFocusID, NavCursorVisible)
#include "i18n.h"
#include "option_rows.h"

extern "C" {
#include "../input_bind.h"
#include "../settings.h"
int input_pad_family(void);              // runtime/hle/input.c
SDL_JoystickID input_active_pad(void);   // runtime/hle/input.c: the controller the game reads (0: none yet)
void input_apply_background(void);
}

// ------------------------------------------------------------------ Configure inputs: what's on it
enum { CAT_BUTTONS, CAT_TRIGGERS, CAT_STICKS, CAT_DPAD, CAT_MENU, N_CAT };
static const char *const k_cat_name[N_CAT] = {LN("Buttons"), LN("Triggers"), LN("Sticks"), LN("D-pad"), LN("Open menu")};
static const char *const k_cat_hook[N_CAT] = {"buttons", "triggers", "sticks", "dpad", "menu"};  // (LARECOMP_CONTROLS_SUB)

// section: a header before this row; fixed: a key that can't be changed (slot -1)
struct Bind { int slot; const char *label; const char *section; const char *fixed; };
static int cat_rows(int cat, bool pad, Bind *out) {
    int n = 0;
    auto add = [&](int slot, const char *label, const char *section = NULL) { out[n++] = {slot, label, section, NULL}; };
    switch (cat) {
    case CAT_BUTTONS:
        add(BIND_A, "A"), add(BIND_B, "B"), add(BIND_X, "X"), add(BIND_Y, "Y"), add(BIND_PLUS, LN("Plus")),
            add(BIND_MINUS, LN("Minus"));
        break;
    case CAT_TRIGGERS: add(BIND_L, "L"), add(BIND_R, "R"), add(BIND_ZL, "ZL"), add(BIND_ZR, "ZR"); break;
    case CAT_STICKS:  // (a controller's sticks stay its sticks: only their presses are buttons)
        add(BIND_LSTICK, LN("Press"), LN("Left stick"));
        if (!pad)
            add(BIND_LS_UP, LN("Up")), add(BIND_LS_DOWN, LN("Down")), add(BIND_LS_LEFT, LN("Left")),
                add(BIND_LS_RIGHT, LN("Right"));
        add(BIND_RSTICK, LN("Press"), LN("Right stick"));
        if (!pad)
            add(BIND_RS_UP, LN("Up")), add(BIND_RS_DOWN, LN("Down")), add(BIND_RS_LEFT, LN("Left")),
                add(BIND_RS_RIGHT, LN("Right"));
        break;
    case CAT_DPAD:
        add(BIND_UP, LN("Up")), add(BIND_DOWN, LN("Down")), add(BIND_LEFT, LN("Left")), add(BIND_RIGHT, LN("Right"));
        break;
    case CAT_MENU:  // (F1 and Esc stay: whatever the controller's shortcuts are, the menu can always be reached)
        if (pad) add(BIND_MENU_1, LN("Shortcut 1")), add(BIND_MENU_2, LN("Shortcut 2"));
        else out[n++] = {-1, LN("Shortcut 1"), NULL, "F1"}, out[n++] = {-1, LN("Shortcut 2"), NULL, "Esc"};
        break;
    }
    return n;
}
static int slot_cat(int slot) {
    if (slot >= BIND_MENU_1) return CAT_MENU;
    Bind b[16];
    for (int c = 0; c < N_CAT; c++)
        for (int i = 0, n = cat_rows(c, false, b); i < n; i++)
            if (b[i].slot == slot) return c;
    return CAT_BUTTONS;
}

// Devices: each connected controller by name (they share one set of bindings; the name picks the button names), or
// "Controller" when none is connected (bindings for one connected later), then the keyboard.
struct Device { SDL_JoystickID id; char name[64]; int fam; bool pad; };
static int pad_fam(SDL_JoystickID id) {
    if (!id || getenv("LARECOMP_PAD_TYPE")) return bind_label_family();  // (none / a test pretending one)
    return input_family_of_type(SDL_GetGamepadTypeForID(id));
}
static int list_devices(Device *d, int cap) {
    int n = 0, np = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&np);
    for (int i = 0; i < np && n < cap - 1; i++) {
        const char *nm = SDL_GetGamepadNameForID(ids[i]);
        d[n].id = ids[i], d[n].pad = true, d[n].fam = pad_fam(ids[i]);
        snprintf(d[n].name, sizeof d[n].name, "%s", nm && *nm ? nm : LN("Controller"));
        if (strlen(d[n].name) > 30) strcpy(d[n].name + 27, "...");  // (fits beside DEVICE)
        n++;
    }
    SDL_free(ids);
    if (!n) d[n].id = 0, d[n].pad = true, d[n].fam = pad_fam(0), snprintf(d[n].name, sizeof d[n].name, LN("Controller")), n++;
    d[n].id = 0, d[n].pad = false, d[n].fam = 0, snprintf(d[n].name, sizeof d[n].name, LN("Keyboard")), n++;
    return n;
}

// ------------------------------------------------------------------ state
static bool g_sub;                 // on the Configure inputs page
static int g_cat = CAT_BUTTONS;    // the category shown on the right
static bool g_dev_pad = true;      // the device chosen: a controller (g_dev_id) or the keyboard
static SDL_JoystickID g_dev_id;
static bool g_in_right;            // the cursor is on a binding (last frame)
static int g_last_frame = -10;     // (a frame without this tab: the page starts over)
static int g_back_frame = -10;     // the frame B / Esc went back a step here (the menu mustn't close on it)
enum { FOCUS_NONE, FOCUS_CONFIGURE, FOCUS_DEVICE, FOCUS_CAT, FOCUS_RIGHT };
static int g_focus;                // put the cursor on this row when it's drawn

// B / Esc this frame went back a step on the Configure inputs page (menu.mm / launcher.mm: don't close too)
extern "C" bool controls_ui_took_back(void) { return g_back_frame == ImGui::GetFrameCount(); }

static void take_focus(int want) {  // (right after the row: it's the last item)
    if (g_focus != want) return;
    g_focus = FOCUS_NONE;
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
    ImGui::SetScrollHereY(0.3f);
}

// The device the page opens on: the keyboard when it was opened with one (Enter / Space), else the controller the game
// reads (or the first connected; "Controller" when there's none)
static void pick_device(bool keyboard) {
    SDL_JoystickID act = input_active_pad();
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (!act && n > 0) act = ids[0];
    SDL_free(ids);
    g_dev_pad = !keyboard, g_dev_id = act;
}
static bool keyboard_pressed(void) {
    return ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
           ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
}

// a menu shortcut's value: "Home", "Minus + Plus", "(none)" (in the menus' language)
static const char *menu_label(int i, int fam, char *buf, size_t cap) {
    int a = g_bind_menu[i][0], b = g_bind_menu[i][1];
    if (a < 0) snprintf(buf, cap, "%s", L(LN("(none)")));
    else if (b < 0) snprintf(buf, cap, "%s", L(bind_pad_label(a, fam)));
    else snprintf(buf, cap, "%s + %s", L(bind_pad_label(a, fam)), L(bind_pad_label(b, fam)));
    return buf;
}

// A finished capture keeps larecomp.ini current; a swap is said under the list (another row changed too)
static void watch_results(int fam, char *msg, size_t cap) {
    static int seen = -1;
    int rdev, rslot, rother, serial, kind = bind_last_result(&rdev, &rslot, &rother, &serial);
    if (seen < 0) seen = serial;
    if (serial == seen) return;
    seen = serial;
    msg[0] = 0;
    char lab[64];
    if (kind == 2 && rother >= BIND_MENU_1)
        snprintf(msg, cap, L("Swapped. Shortcut %d is %s now."), rother - BIND_MENU_1 + 1,
                 menu_label(rother - BIND_MENU_1, fam, lab, sizeof lab));
    else if (kind == 2)
        snprintf(msg, cap, L("Swapped. %s is on %s now."), L(bind_input_name(rother)),
                 L(rdev == BIND_DEV_PAD ? bind_pad_label(bind_pad_host(rother), fam) : bind_key_label(g_bind_key[rother])));
    if (kind == 6) snprintf(msg, cap, "%s", L("The game uses that button. Hold it with a second button."));
    if (kind == 7) snprintf(msg, cap, "%s", L("Keep at least one shortcut, so a controller can open this menu."));
    if (kind == 1 || kind == 2 || kind == 5 || kind == 8) settings_save();
}

static bool nav_on_item(void) { return ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible; }

static void configure_inputs(float scale) {
    Device devs[9];
    int nd = list_devices(devs, 9), di = -1;
    for (int i = 0; i < nd; i++)
        if (devs[i].pad == g_dev_pad && (!g_dev_pad || devs[i].id == g_dev_id)) di = i;
    if (di < 0) di = g_dev_pad ? 0 : nd - 1;  // (that controller is gone: the first one)
    g_dev_pad = devs[di].pad, g_dev_id = devs[di].id;
    const int dev = g_dev_pad ? BIND_DEV_PAD : BIND_DEV_KEY, fam = devs[di].fam;

    int cd, cs, ms;
    bind_capture_active(&cd, &cs, &ms);
    static char msg[160];
    watch_results(fam, msg, sizeof msg);

    // B / Esc: from the bindings back to their category, from the categories back to the tab (a capture takes Esc and
    // every controller button itself: runtime/hle/bindings.c)
    if (cd < 0 && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        g_back_frame = ImGui::GetFrameCount();
        if (g_in_right) g_focus = FOCUS_CAT;
        else { g_sub = false, g_focus = FOCUS_CONFIGURE, msg[0] = 0; return; }  // (the tab is drawn instead)
    }
    // Left on a binding: back to its category too
    if (g_in_right && cd < 0 && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
                                 ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft)))
        g_focus = FOCUS_CAT;

    opt_begin("##configure", scale);
    opt_section("Configure inputs");
    const char *names[9];
    for (int i = 0; i < nd; i++) names[i] = devs[i].name;
    int ndi = di;
    if (opt_choice("Device", &ndi, names, nd, di, NULL)) {
        bind_capture_cancel();
        g_dev_pad = devs[ndi].pad, g_dev_id = devs[ndi].id, msg[0] = 0;
    }
    take_focus(FOCUS_DEVICE);
    for (int c = 0; c < N_CAT; c++) {
        if (g_in_right && c == g_cat) opt_mark_next();
        bool open = opt_link(k_cat_name[c], NULL);
        if (c == g_cat) take_focus(FOCUS_CAT);
        if (nav_on_item() && g_cat != c) g_cat = c, msg[0] = 0;  // (moving down the list shows each)
        if (open) { if (g_cat != c) msg[0] = 0; g_cat = c, g_focus = FOCUS_RIGHT; bind_capture_cancel(); }
    }
    if (bind_is_default(dev)) opt_disable_next();
    if (opt_action("Restore default controls", "", g_dev_pad ? "Puts every controller button back to its default."
                                                            : "Puts every key back to its default.")) {
        bind_capture_cancel();
        bind_reset(dev);
    }
    // (the cursor or the mouse on a row with a description: it shows on the right instead of the bindings)
    bool desc_right = (ImGui::IsItemHovered() || nav_on_item()) && g_focus != FOCUS_RIGHT;
    opt_split();
    if (desc_right) {
        opt_desc_text();
        g_in_right = false;
    } else {
        Bind rows[16];
        int n = cat_rows(g_cat, g_dev_pad, rows);
        opt_section(k_cat_name[g_cat]);
        bool in_right = false;
        for (int i = 0; i < n; i++) {
            const Bind &b = rows[i];
            if (b.section) opt_section(b.section);
            if (b.fixed) {
                opt_disable_next();
                ImGui::PushID(i);
                opt_action(b.label, b.fixed, NULL);
                ImGui::PopID();
                continue;
            }
            bool waiting = cd == dev && cs == b.slot, menu = b.slot >= BIND_MENU_1;
            char val[64];
            if (waiting) snprintf(val, sizeof val, g_dev_pad ? L("Press a button...  %d") : L("Press a key...  %d"), (ms + 999) / 1000);
            else if (menu) menu_label(b.slot - BIND_MENU_1, fam, val, sizeof val);
            else snprintf(val, sizeof val, "%s", g_dev_pad ? bind_pad_label(bind_pad_host(b.slot), fam) : bind_key_label(g_bind_key[b.slot]));
            // the same button / key on two inputs (a hand-edited larecomp.ini), or a game button that opens the menu: red
            bool dup = false;
            if (menu) {
                int a = g_bind_menu[b.slot - BIND_MENU_1][0];
                for (int j = 0; j < BIND_PAD_COUNT; j++) dup |= g_bind_menu[b.slot - BIND_MENU_1][1] < 0 && bind_pad_host(j) == a && a >= 0;
            } else {
                const int16_t *t = g_dev_pad ? g_bind_pad : g_bind_key;
                for (int j = 0, m = g_dev_pad ? BIND_PAD_COUNT : BIND_KEY_COUNT; j < m; j++)
                    dup |= j != b.slot && t[j] == t[b.slot] && t[b.slot] > (g_dev_pad ? -1 : 0);
                dup |= g_dev_pad && bind_menu_single(bind_pad_host(b.slot));
            }
            if (dup && !waiting) opt_warn_next();
            ImGui::PushID(b.slot);
            bool hit;
            if (menu) {  // (its value is put together here, so already in the menus' language)
                opt_shown_next();
                hit = opt_action(L(b.label), val, NULL, true, waiting || !bind_is_default_slot(dev, b.slot));
            } else hit = opt_action(b.label, val, NULL, true, waiting || !bind_is_default_slot(dev, b.slot));
            ImGui::PopID();
            if (i == 0) take_focus(FOCUS_RIGHT);
            if (ImGui::IsItemFocused()) in_right = true;
            if (waiting && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);  // (a capture started by a test hook)
            // X (the top button) / Delete / Backspace / right click: clear a menu shortcut (#591: Home free for overlays)
            if (menu && !waiting && cd < 0 &&
                ((ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) ||
                                             ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
                                             ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) ||
                 ImGui::IsItemClicked(ImGuiMouseButton_Right))) {
                msg[0] = 0;
                bind_menu_clear(b.slot - BIND_MENU_1);
            }
            if (hit && menu && ImGui::IsKeyDown(ImGuiKey_GamepadFaceUp)) hit = false;  // (ImGui activates on it too)
            if (hit) {
                if (waiting) bind_capture_cancel();
                else { msg[0] = 0; bind_capture_begin(dev, b.slot); }
            }
        }
        g_in_right = in_right;
        if (cd == dev && cs >= BIND_MENU_1) ImGui::TextDisabled("%s", L("Press one button, or two together. Esc cancels."));
        else if (cd == dev) ImGui::TextDisabled("%s", L("Esc cancels."));
        else if (msg[0]) ImGui::TextDisabled("%s", msg);
        else if (g_cat == CAT_MENU && g_dev_pad)
            ImGui::TextDisabled(L("%s or Delete clears a shortcut."), L(bind_pad_label(SDL_GAMEPAD_BUTTON_NORTH, fam)));
        else if (g_cat == CAT_MENU) ImGui::TextDisabled("%s", L("F1 and Esc always open this menu."));
    }
    opt_split_end();
}

// ------------------------------------------------------------------ the tab
extern "C" void controls_tab_draw(float scale) {
    int frame = ImGui::GetFrameCount();
    if (frame - g_last_frame > 1) g_sub = false, g_focus = FOCUS_NONE, g_in_right = false;  // (back on the tab)
    g_last_frame = frame;
    // (test) LARECOMP_CONTROLS_SUB=<buttons|triggers|sticks|dpad>[:right][:key]: on that page, the cursor on the category
    // (right: on its first binding; key: the keyboard's) - for a few frames, past the menu putting it on the tab bar
    static bool hook_done;
    static int hook_frames, hook_focus;
    if (!hook_done) {
        hook_done = true;
        if (const char *h = getenv("LARECOMP_CONTROLS_SUB")) {
            pick_device(strstr(h, ":key") != NULL);
            g_sub = true, hook_focus = strstr(h, "right") ? FOCUS_RIGHT : FOCUS_CAT, hook_frames = 10;
            for (int c = 0; c < N_CAT; c++) if (!strncmp(h, k_cat_hook[c], strlen(k_cat_hook[c]))) g_cat = c;
        }
    }
    if (hook_frames > 0) hook_frames--, g_focus = hook_focus;
    {   // a capture started elsewhere (LARECOMP_BIND_TEST): show it waiting
        int cd, cs;
        if (bind_capture_active(&cd, &cs, NULL) && (!g_sub || g_dev_pad != (cd == BIND_DEV_PAD) || g_cat != slot_cat(cs))) {
            if (!g_sub) pick_device(cd == BIND_DEV_KEY);
            g_sub = true, g_dev_pad = cd == BIND_DEV_PAD, g_cat = slot_cat(cs);
        }
    }
    if (g_sub) {
        configure_inputs(scale);
        if (g_sub) return;
    }

    opt_begin("##controls", scale);
    opt_section("Inputs");
    if (opt_link("Configure inputs", "Set the buttons and keys for the game, and the buttons that open this menu. "
                                     "F1 and Esc always open it.")) {
        pick_device(keyboard_pressed());
        g_sub = true, g_focus = FOCUS_DEVICE, g_in_right = false;
    }
    take_focus(FOCUS_CONFIGURE);
    if (opt_toggle("Allow background inputs", &g_settings.bg_input, false,
                   "Controllers keep working while another window is in front. macOS may ask for Input Monitoring "
                   "permission."))
        input_apply_background();

    opt_section("Movement");
    {
        static const char *const modes[] = {LN("Nothing"), LN("Items"), LN("Movement")};
        opt_choice("D-pad", &g_settings.dpad_mode, modes, 3, DPAD_NOTHING, "What the D-pad does in play.");
    }
    opt_toggle("360-degree movement", &g_settings.move_360, false,
               "Link walks in the exact direction of the stick instead of snapping to 8 directions.");

    opt_section("Controller");
    opt_toggle("Rumble", &g_settings.rumble, true, "Controller vibration.");
    {
        static const char *const pct[] = {LN("25%"), LN("50%"), LN("75%"), LN("100%")};
        int i = g_settings.rumble_strength / 25 - 1;
        if (i < 0 || i > 3) i = 3;
        if (!g_settings.rumble) opt_disable_next();
        if (opt_choice("Intensity", &i, pct, 4, 3, "How strong the vibration is.", false))
            g_settings.rumble_strength = (i + 1) * 25;
    }
    opt_toggle("Controller light", &g_settings.controller_light, true, "The light bar shows Link's health.");
    {
        static const char *const fam[] = {LN("Nintendo"), LN("Xbox"), LN("PlayStation")};
        char autol[64];  // (in the menus' language already: the row leaves it as it is)
        snprintf(autol, sizeof autol, L("Auto (%s)"), L(fam[input_pad_family() % 3]));
        const char *names[] = {autol, LN("Nintendo"), LN("Xbox"), LN("PlayStation")};
        opt_choice("Button icons", &g_settings.prompts, names, 4, PROMPTS_AUTO,
                   "Which button icons the game shows. Auto matches the controller you have plugged in.");
    }
    {
        static const char *const names[] = {LN("Nintendo layout"), LN("Bottom button")};
        opt_choice("Confirm button", &g_settings.menu_confirm, names, 2, MENU_CONFIRM_CONTROLLER,
                   "Which button confirms in LAS Recomp's menus. Nintendo layout uses the right button like the Switch, "
                   "Bottom button uses the bottom one like Xbox and PlayStation.");
    }
    opt_toggle("Buttons match labels", &g_settings.face_by_label, false,
               "On Xbox and PlayStation controllers, A, B, X and Y work as printed on the controller, so the bottom "
               "button is A. Off keeps the Switch layout. Nintendo controllers aren't affected.");

    opt_end();
}
