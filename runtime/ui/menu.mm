// In-game overlay menu built with Dear ImGui (Metal renderer on macOS, Vulkan elsewhere; input fed from SDL events).
//
// Threads: SDL events arrive on the main thread and are queued; the menu is laid out and drawn on the
// render thread inside backend_present, after the game image, into the same drawable.
#ifdef __APPLE__
#import <Metal/Metal.h>
#endif
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <string>
#include <vector>

#include "imgui.h"
#include "i18n.h"
#include "live_preview.h"
#include "option_rows.h"
#include "imgui_internal.h"  // (NavCursorVisible)
#include "osk.h"
#include "pad_nav.h"
#include "plastic.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
extern "C" bool vk_imgui_backend_init(void);  // runtime/nvn/vulkan/imgui_glue.cpp
extern "C" void vk_imgui_fonts_rebuild(void);
// runtime/nvn/vulkan/device.c: the command buffer ImGui records into (a secondary one the recorder thread executes in
// the window image's commands, or that buffer itself after the recorder caught up), then handed back
extern "C" void *vk_overlay_begin(void *window_cmd);
extern "C" void vk_overlay_end(void *window_cmd, void *cmd);
extern "C" VkImageView g_vk_scene_view;  // (runtime/nvn/vulkan/exec.c: the game image drawn this frame)
extern "C" float g_vk_scene_uv[4];
#endif

extern "C" {
#include "../hle.h"
#include "../settings.h"
#include "../menu_sfx.h"
#include "menu.h"
void input_reset(void);
void save_editor_draw(float scale);  // save_editor.mm
extern "C" int game_in_play(void);  // hle/warp.c
void save_editor_on_open(void);
void rando_tab_draw(float scale);
bool warp_tab_draw(float scale);  // warp_tab.mm: true when a warp started
extern "C" void cheats_draw(float scale);  // cheats.mm
extern "C" void difficulty_draw(float scale);  // difficulty.mm
void controls_tab_draw(float scale);  // controls_ui.mm
bool controls_ui_took_back(void);
bool rando_tab_took_back(void);  // rando_tab.mm: B went back a step in the right column
bool save_editor_took_back(void);  // save_editor.mm (the same)
bool warp_tab_took_back(void);  // warp_tab.mm (the same)
#include "../input_bind.h"
#include "../livesplit.h"
#include "../paths.h"
#include "../rando.h"
}

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_fullscreen_apply{false};

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT, RELEASE_ALL } kind;
    ImGuiKey key;
    bool down;
    float value, x, y;
    int button;
    char text[32];
};
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static std::vector<QEvent> q_events;
static float g_pixel_density = 1;

static void push(const QEvent &e) {
    pthread_mutex_lock(&q_lock);
    q_events.push_back(e);
    pthread_mutex_unlock(&q_lock);
}

bool menu_is_open(void) { return g_open.load(); }
static void set_open(bool open);
// The menu in game: a bar drops down from the top over the game, its entries in a row (the one the cursor is on lit),
// a close X at its right end; each entry opens its page in a window under it. The start screen's Settings is
// launcher.mm's.
enum { BAR_SETTINGS, BAR_WARP, BAR_EDITOR, BAR_RESET, BAR_QUIT, BAR_CLOSE, BAR_N };
static std::atomic<int> g_page{-1};  // the entry whose page is open (-1: the bar alone)
static int g_bar_cur;  // the bar's cursor (render thread)
static void page_open(int page);
static int g_tab = -1000, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab; -1000: the first tab)
static int g_ask = -1;  // the bar's Reset or Quit asked (render thread): ask_popup
extern "C" void menu_test_open(void) {
    set_open(true);
    // (test) LARECOMP_MENU_BAR=<entry>: the bar alone, its cursor on that entry. Otherwise a page, as before the bar:
    // LARECOMP_MENU_TAB=4 Warp, 2 Save Editor, 9 Quit, 10 Reset, any other a Settings tab
    if (getenv("LARECOMP_MENU_BAR")) { g_bar_cur = atoi(getenv("LARECOMP_MENU_BAR")) % BAR_N; return; }
    int t = getenv("LARECOMP_MENU_TAB") ? atoi(getenv("LARECOMP_MENU_TAB")) : -1;
    page_open(t == 4 ? BAR_WARP : t == 2 ? BAR_EDITOR : t == 9 ? BAR_QUIT : t == 10 ? BAR_RESET : BAR_SETTINGS);
    if (t >= 0 && t != 2 && t != 4 && t != 9 && t != 10) { extern int g_tab_request_ext; g_tab_request_ext = t; }
    // (test) LARECOMP_PREVIEW=<row label>: that row's live preview opens straight away (its page: LARECOMP_MENU_TAB)
    if (const char *row = getenv("LARECOMP_PREVIEW")) {
        if (t >= 0) g_tab = t;
        preview_open(row, -1);
    }
}

void menu_tabs_focus(bool first_tab);
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_page = -1;  // (the bar alone first)
    g_open.store(open);
    menu_sfx_play(open ? MENU_SFX_OPEN : MENU_SFX_CLOSE);
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    // nor the other way round: the releases of what was held when it closed never reach the menu (it isn't capturing
    // then), so it starts with nothing held - else the cursor kept scrolling, or a press went missing, on the next open
    { QEvent q = {}; q.kind = QEvent::RELEASE_ALL; push(q); }
    if (!open) { settings_save(); osk_cancel(); g_text_busy = false; preview_reset(); }
}

static void page_open(int page) {
    g_bar_cur = page;
    if (page == BAR_SETTINGS) {
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    } else menu_tabs_focus(false);  // (a page alone: the first Down goes to its first row)
    if (page == BAR_EDITOR) save_editor_on_open();
    if (page == BAR_RESET || page == BAR_QUIT) { g_ask = page; return; }  // (a question over the bar: ask_popup)
    g_page = page;
}

static ImGuiKey key_from_scancode(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    case SDL_SCANCODE_A: return ImGuiKey_A;
    case SDL_SCANCODE_C: return ImGuiKey_C;
    case SDL_SCANCODE_V: return ImGuiKey_V;
    case SDL_SCANCODE_X: return ImGuiKey_X;
    case SDL_SCANCODE_Z: return ImGuiKey_Z;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;  // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;  // next tab
    default: return ImGuiKey_None;
    }
}


// ---- software keyboard (runtime/hle/swkbd.c: the game's name entry etc.) --------------------------------------
// The game thread waits in menu_keyboard_run (re-presenting the last frame); this dialog takes the text: typed
// directly when the player last used a keyboard, an on-screen keyboard (d-pad + confirm, back deletes, Start / + OK; buttons as in pad_nav.h) when
// they last used a controller.
static std::atomic<int> g_kbd{0};  // 1 open, 2 confirmed, 3 cancelled
static std::atomic<bool> g_last_pad{false};
static char g_kbd_text[96];
static int g_kbd_max = 8;
static bool capturing() { return g_open.load() || g_kbd.load() == 1; }
// The game's name limit counts characters as the console's keyboard does, in UTF-16 units (a character past U+FFFF
// takes two); the text is UTF-8. Cuts s after max_units of them, only between characters (a character left incomplete
// - a cut inside it, or bytes that are no character - goes too, with the rest), and returns how many units are left.
static int kbd_cut(char *s, int max_units) {
    int units = 0;
    for (unsigned char *p = (unsigned char *)s; *p;) {
        int n = *p < 0x80 ? 1 : *p >= 0xc2 && *p <= 0xdf ? 2 : *p >= 0xe0 && *p <= 0xef ? 3 : *p >= 0xf0 && *p <= 0xf4 ? 4 : 0;
        int i = 1;
        while (i < n && (p[i] & 0xc0) == 0x80) i++;
        if (i == n && n > 2 && ((*p == 0xe0 && p[1] < 0xa0) || (*p == 0xed && p[1] > 0x9f) || (*p == 0xf0 && p[1] < 0x90) ||
                                (*p == 0xf4 && p[1] > 0x8f)))
            n = 0;  // (overlong, a surrogate or past U+10FFFF: the game's UTF-8 conversion refuses those too)
        int u = n == 4 ? 2 : 1;
        if (!n || i < n || units + u > max_units) { *p = 0; break; }
        units += u, p += n;
    }
    return units;
}
// The game gets no input while this is true (runtime/hle/input.c): menu or name entry up.
extern "C" bool menu_input_captured(void) { return capturing(); }

bool menu_handle_event(const SDL_Event *e) {
    static uint32_t pad_held;  // (controller buttons down, by bit: the menu's two-button shortcuts)
    if (g_open.load() && bind_capture_event(e)) return true;  // Controls tab waiting for a new binding: it takes the press
    if (e->type == SDL_EVENT_KEY_DOWN) g_last_pad = false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) g_last_pad = true;
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {  // Enter confirms, Esc cancels
        if (e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER) { g_kbd = 2; return true; }
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) { g_kbd = 3; return true; }
    }
    if (g_kbd.load() == 1 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button == SDL_GAMEPAD_BUTTON_START) {
        g_kbd = 2;
        return true;
    }
    // typing (a text field, the on-screen keyboard, the name entry): keys are text, not menu shortcuts
    bool typing = g_kbd.load() == 1 || (g_open.load() && g_text_busy.load());
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
                               (e->key.scancode == SDL_SCANCODE_ESCAPE && !g_open.load()))) {
            if (!typing) set_open(!g_open.load());  // (Esc with the menu open goes to ImGui below: it cancels a field)
            return true;
        }
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F11 ||
                               (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & (SDL_KMOD_ALT | SDL_KMOD_GUI))) ||
                               (e->key.scancode == SDL_SCANCODE_F && (e->key.mod & SDL_KMOD_GUI) && (e->key.mod & SDL_KMOD_CTRL)))) {
            g_settings.fullscreen = !g_settings.fullscreen;
            g_want_fullscreen_apply.store(true);
            settings_save();
            return true;
        }
        /* fallthrough */
    case SDL_EVENT_KEY_UP: {
        if (!capturing()) return false;
        bool down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && e->key.scancode == SDL_SCANCODE_ESCAPE) {  // Esc: back out of popups, else close
            QEvent q = {};
            q.kind = QEvent::KEY; q.key = ImGuiKey_Escape; q.down = true; push(q);
            q.down = false; push(q);
            return true;
        }
        ImGuiKey k = key_from_scancode(e->key.scancode);
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::TEXT; strlcpy(q.text, e->text.text, sizeof q.text); push(q); }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button < 32) pad_held = down ? pad_held | 1u << e->gbutton.button : pad_held & ~(1u << e->gbutton.button);
        // the player's shortcuts (Controls > Configure inputs > Open menu; Home and Back + Start by default). A Home
        // that isn't one goes on below, not taken (#591: it opens Steam's and other launchers' overlays).
        if (down && !typing && bind_menu_pressed(e->gbutton.button, pad_held)) {
            set_open(!g_open.load());
            return true;
        }
        if (!capturing()) return false;
        ImGuiKey k = ui_pad_key(e->gbutton.which, e->gbutton.button, down);  // (confirm / back: pad_nav.h)
        if (k != ImGuiKey_None) { QEvent q = {}; q.kind = QEvent::KEY; q.key = k; q.down = down; push(q); }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!capturing()) return false;
        float v = e->gaxis.value / 32767.0f;
        QEvent q = {};
        q.kind = QEvent::ANALOG;
        switch (e->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            q.key = ImGuiKey_GamepadLStickLeft; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickRight; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            q.key = ImGuiKey_GamepadLStickUp; q.value = v < -0.3f ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadLStickDown; q.value = v > 0.3f ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_RIGHTY:  // (scrolls a right column of plain text: option_rows.mm)
            q.key = ImGuiKey_GamepadRStickUp; q.value = v < 0 ? -v : 0; push(q);
            q.key = ImGuiKey_GamepadRStickDown; q.value = v > 0 ? v : 0; push(q);
            break;
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: q.key = ImGuiKey_GamepadL2; q.value = v; push(q); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: q.key = ImGuiKey_GamepadR2; q.value = v; push(q); break;
        default: break;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::MOUSE_POS; q.x = e->motion.x * g_pixel_density; q.y = e->motion.y * g_pixel_density; push(q); }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!capturing()) return false;
        {
            QEvent q = {};
            q.kind = QEvent::MOUSE_BUTTON;
            q.button = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            q.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            push(q);
        }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!capturing()) return false;
        { QEvent q = {}; q.kind = QEvent::WHEEL; q.x = e->wheel.x; q.y = e->wheel.y; push(q); }
        return true;
    default:
        return false;
    }
}

// Remember window size (Video tab): the window opens at the size it had when LAS Recomp last quit. The size is taken
// at exit (every quit ends in exit()), not in fullscreen or maximized - those keep the last plain window's size.
static SDL_Window *g_remember_win;
static void remember_at_exit(void) {
    if (!g_settings.remember_window || !g_remember_win) return;
    SDL_WindowFlags fl = SDL_GetWindowFlags(g_remember_win);
    int w, h;
    if ((fl & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) || !SDL_GetWindowSize(g_remember_win, &w, &h))
        return;
    if (w == g_settings.window_w && h == g_settings.window_h) return;
    g_settings.window_w = w, g_settings.window_h = h;
    settings_save();
}
extern "C" void rt_window_size(int *w, int *h) {  // before the window is made: the remembered size, if it's on
    if (g_settings.remember_window && g_settings.window_w >= 320 && g_settings.window_h >= 180)
        *w = g_settings.window_w, *h = g_settings.window_h;
}
extern "C" void rt_window_remember(void *w) {
    if (!g_remember_win) atexit(remember_at_exit);
    g_remember_win = (SDL_Window *)w;
}

extern "C" uint32_t g_window_resize_request;  // (test scripts: runtime/hle/input.c "resize")
void menu_apply_window_requests(void *win) {
    SDL_Window *w = (SDL_Window *)win;
    g_pixel_density = SDL_GetWindowPixelDensity(w);
    static bool text_input;
    bool want_text = capturing();
    if (want_text != text_input) {
        if (want_text) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
        text_input = want_text;
    }
    static bool first = true;
    if (first || g_want_fullscreen_apply.exchange(false)) {
        first = false;
        SDL_SetWindowFullscreen(w, g_settings.fullscreen);
    }
    if (uint32_t r = __atomic_exchange_n(&g_window_resize_request, 0u, __ATOMIC_RELAXED))
        SDL_SetWindowSize(w, (int)(r >> 16), (int)(r & 0xffff));
}

// ------------------------------------------------------------------ render thread
static bool g_inited;
#ifdef __APPLE__
static id<MTLTexture> g_scene;  // (menu_set_scene: this frame's game image, for the glare guard)
static float g_scene_uv[4];
void menu_set_scene(id<MTLTexture> tex, const float uv[4]) {
    g_scene = tex;
    memcpy(g_scene_uv, uv, sizeof g_scene_uv);
}
#endif
static ImGuiContext *g_ctx;  // ours: the start screen (back to it after a reboot) has its own context
static ImFont *g_font, *g_font_big;
static ImFont *g_menu_font, *g_menu_font_big;  // the menu window (Baloo 2); everything else keeps g_font
static int g_focus_tab = -99;  // put the nav cursor on this tab next frame (-100: the selected one; -99: none)
static int g_focus_frames;

// The start screen's Settings opening: the controller's cursor starts on the first tab the first time in a session,
// later on the tab that's selected
static bool g_page_fresh_seen;  // (focus_check: the menu opened)
static bool g_page_fresh;  // the page shown hasn't been entered yet: the first Down goes to its first enabled row
static bool g_page_focus_next;  // (that page has no option rows: its first control takes the cursor next frame)
static int g_shown_tab = -1000;  // the page drawn last (its id)
void menu_tabs_focus(bool first_tab) {
    g_focus_tab = first_tab ? -1 : -100, g_focus_frames = 0, g_page_fresh = g_page_fresh_seen = true;
}
int g_tab_request_ext = -1;

static void style_init(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14; s.FrameRounding = 8; s.GrabRounding = 8; s.TabRounding = 8; s.ChildRounding = 10;
    s.WindowPadding = ImVec2(28, 24); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(14, 12);
    s.WindowBorderSize = 0; s.ScrollbarSize = 18;
    ImVec4 *c = s.Colors;
    ImVec4 gold(0.93f, 0.76f, 0.33f, 1), gold_dim(0.62f, 0.49f, 0.20f, 1), green(0.15f, 0.36f, 0.25f, 1);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.09f, 0.08f, 0.94f);
    c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.13f, 0.11f, 0.85f);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.10f, 0.09f, 0.98f);
    c[ImGuiCol_Text] = ImVec4(0.96f, 0.94f, 0.88f, 1);
    c[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.64f, 0.58f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.19f, 0.16f, 1);
    c[ImGuiCol_FrameBgHovered] = green;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.18f, 1);
    c[ImGuiCol_ButtonHovered] = green;
    c[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Header] = green;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.45f, 0.31f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.21f, 0.50f, 0.34f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.12f, 0.18f, 0.15f, 1);
    c[ImGuiCol_TabHovered] = green;
    c[ImGuiCol_TabSelected] = ImVec4(0.21f, 0.44f, 0.31f, 1);
    c[ImGuiCol_TabSelectedOverline] = gold;
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = gold_dim;
    c[ImGuiCol_SliderGrabActive] = gold;
    c[ImGuiCol_NavCursor] = gold;
    c[ImGuiCol_Separator] = ImVec4(0.30f, 0.36f, 0.30f, 1);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_WindowBg];
}

// The menus' parchment colours (plastic.h) over that, only while a menu is drawn: the overlays over the game (FPS
// counter, run timer, trackers, cards) keep the look above. The pieces are the backgrounds - ImGui's own fills go clear
// or take the theme's colours (frames, buttons and windows are pieces too while plastic_widgets is on: their colours
// here only tell them apart).
static void style_parchment(ImGuiStyle &s) {
    ImVec4 *c = s.Colors;
    const PlasticTheme &t = plastic_theme();
    ImVec4 clear(0, 0, 0, 0), text = plastic_col(t.text);
    c[ImGuiCol_WindowBg] = c[ImGuiCol_ChildBg] = clear;
    c[ImGuiCol_PopupBg] = ImVec4(t.panel.x, t.panel.y, t.panel.z, 1);
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = plastic_col(t.text_dim);
    c[ImGuiCol_Border] = t.frame;
    c[ImGuiCol_Button] = t.row;
    c[ImGuiCol_ButtonHovered] = t.row_hot;
    c[ImGuiCol_ButtonActive] = ImVec4(t.row_hot.x * 0.95f, t.row_hot.y * 0.95f, t.row_hot.z * 0.95f, 1);
    c[ImGuiCol_Header] = ImVec4(t.tab.x, t.tab.y, t.tab.z, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(t.row_hot.x, t.row_hot.y * 0.98f, t.row_hot.z, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(t.row_hot.x * 0.93f, t.row_hot.y * 0.93f, t.row_hot.z * 0.93f, 1);
    c[ImGuiCol_FrameBg] = ImVec4(t.inset.x, t.inset.y, t.inset.z, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(t.inset.x, t.inset.y * 0.99f, t.inset.z * 0.98f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(t.inset.x * 0.99f, t.inset.y * 0.98f, t.inset.z * 0.97f, 1);
    c[ImGuiCol_CheckMark] = t.row_sel;
    c[ImGuiCol_SliderGrab] = t.ring;
    c[ImGuiCol_SliderGrabActive] = t.row_sel;
    c[ImGuiCol_PlotHistogram] = c[ImGuiCol_PlotHistogramHovered] = t.row_sel;
    c[ImGuiCol_NavCursor] = clear;  // (the pieces show the cursor themselves)
    c[ImGuiCol_Separator] = ImVec4(t.ring.x, t.ring.y, t.ring.z, 0.40f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.22f);  // (a dark groove; the grab in the rims' khaki gold)
    c[ImGuiCol_ScrollbarGrab] = ImVec4(t.ring.x, t.ring.y, t.ring.z, 0.85f);
    c[ImGuiCol_ScrollbarGrabHovered] = c[ImGuiCol_ScrollbarGrabActive] = t.row_sel;
    c[ImGuiCol_TextSelectedBg] = ImVec4(t.row_sel.x, t.row_sel.y, t.row_sel.z, 0.35f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.10f, 0.06f, 0.02f, 0.40f);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabHovered] = c[ImGuiCol_TabSelected] = clear;
    s.ScrollbarRounding = 9;
}


// The fonts, with the glyphs of the menus' language (i18n.h): again when it changes
static void load_fonts(void) {
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    g_font = g_font_big = g_menu_font = g_menu_font_big = NULL;
    i18n_fonts_stale(io.Fonts);
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    char dejavu[1024];  // none of the system's: the bundled DejaVu Sans (third_party/fonts/DejaVu, in every release)
    const char *bundled = rt_resource("DejaVuSans.ttf", "third_party/fonts/DejaVu/DejaVuSans.ttf", dejavu, sizeof dejavu) ? dejavu : NULL;
    const char *paths[] = {"/System/Library/Fonts/Avenir Next.ttc", "/System/Library/Fonts/Helvetica.ttc",
                           "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                           "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", bundled};
    for (const char *p : paths) {
        if (!p) continue;
        if (FILE *f = fopen(p, "rb")) fclose(f);  // a missing file is an assertion in ImGui, not a NULL return
        else continue;
        if (!g_font) g_font = io.Fonts->AddFontFromFileTTF(p, 34, &cfg, i18n_glyph_ranges()), i18n_merge_fonts(io.Fonts, 34);
        if (g_font && !g_font_big) {
            ImFontConfig b = cfg;
            b.FontNo = 0;
            g_font_big = io.Fonts->AddFontFromFileTTF(p, 56, &b, i18n_glyph_ranges());
            i18n_merge_fonts(io.Fonts, 56);
        }
    }
    // the menu window itself: Baloo 2 SemiBold, as on the start screen (shipped, OFL), at its Settings window's sizes; the
    // FPS counter, overlays and on-screen keyboard keep the fonts above
    char baloo[1024];
    if (const char *f = getenv("LARECOMP_MENU_FONT")) strlcpy(baloo, f, sizeof baloo);  // (test) another weight / face
    if (getenv("LARECOMP_MENU_FONT") || rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo)) {
        g_menu_font = io.Fonts->AddFontFromFileTTF(baloo, 39, &cfg, i18n_glyph_ranges());
        i18n_merge_fonts(io.Fonts, 39, 0.8f);
        g_menu_font_big = io.Fonts->AddFontFromFileTTF(baloo, 72, &cfg, i18n_glyph_ranges());
        i18n_merge_fonts(io.Fonts, 72, 0.8f);
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault(), i18n_merge_fonts(io.Fonts, 13);
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
}

#ifdef __APPLE__
static id<MTLDevice> g_dev;  // (the font texture again: load_fonts)
static void init(id<MTLDevice> dev) {
    g_dev = dev;
#else
static void init(void) {
#endif
    IMGUI_CHECKVERSION();
    g_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    load_fonts();
    style_init();
#ifdef __APPLE__
    ImGui_ImplMetal_Init(dev);
#else
    vk_imgui_backend_init();
#endif
    g_inited = true;
}

static void drain_events(ImGuiIO &io) {
    pthread_mutex_lock(&q_lock);
    std::vector<QEvent> ev;
    ev.swap(q_events);
    pthread_mutex_unlock(&q_lock);
    // a rebind (Controls tab) takes every release while it waits: the confirm press that started it, a stick let go
    static bool capture_was;
    if (bind_capture_active(NULL, NULL, NULL) != capture_was) capture_was = !capture_was, ui_pad_release_all(io);
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        case QEvent::MOUSE_POS: io.AddMousePosEvent(q.x, q.y); break;
        case QEvent::MOUSE_BUTTON: io.AddMouseButtonEvent(q.button, q.down); break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
        case QEvent::RELEASE_ALL: ui_pad_release_all(io); break;
        }
    }
}

extern "C" void renderer_get_scale(float *sx, float *sy);
extern "C" int input_pad_family(void);  // runtime/hle/input.c
extern "C" double output_scale_dynamic_factor(void);

static void tab_display(void) {
    float scale = ImGui::GetFontSize() / 34;
    opt_begin("##video", scale);
    opt_section("Display");
    const char *aspects[] = {LN("Unlocked"), LN("Original 16:9")};
    preview_text(LN("Configure the shape of the picture. Unlocked fills the window, Original keeps 16:9."), NULL);
    opt_preview("Aspect ratio", &g_settings.aspect, aspects, 2, ASPECT_UNLOCKED,
               "Unlocked fills the window and shows more of the island on wide screens, up to ultrawide. Original keeps "
               "the game's 16:9 view with bars.");
    bool fs = g_settings.fullscreen;
    if (opt_toggle("Fullscreen", &fs, false, "Fills the screen. You can also press F11, Alt+Enter or Cmd+Ctrl+F.")) {
        g_settings.fullscreen = fs;
        g_want_fullscreen_apply.store(true);
    }
    static const char *fps_corners[] = {LN("Off"), LN("Top left"), LN("Top right"), LN("Bottom left"), LN("Bottom right")};
    opt_choice("Show FPS", &g_settings.show_fps, fps_corners, N_FPS_CORNERS, FPS_OFF,
               "Shows the frame rate in a corner of the screen.");
    opt_toggle("Frame Generation", &g_settings.frame_interp, false,
               "Experimental. Draws extra images between the game's frames for smoother motion, up to your display's "
               "refresh rate. On a 120 Hz display the game shows 120 fps. Some visual glitches are possible.");
    {   // (a live preview, as an Off / On choice)
        static const char *oo[] = {LN("Off"), LN("On")};
        int rb = g_settings.blur_removal;
        if (opt_preview("Remove blur", &rb, oo, 2, 0, "Turns off the tilt-shift blur at the top and bottom of the screen."))
            g_settings.blur_removal = rb;
    }
    opt_toggle("Pause on focus lost", &g_settings.pause_unfocused, false,
               "The game pauses while its window isn't in front.");
    opt_toggle("Remember window size", &g_settings.remember_window, false,
               "The window opens at the size it was when you quit.");

    opt_section("Rendering");
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    static const char *res[] = {LN("25%"), LN("50%"), LN("75%"), LN("Match display"), LN("Dynamic"),
                                LN("1x"), LN("2x"), LN("3x"), LN("4x")};
    static const int res_mode[] = {RES_Q25, RES_Q50, RES_Q75, RES_MATCH, RES_DYNAMIC, 1, 2, 3, 4};
    enum { RES_MATCH_AT = 3 };
    enum { NRES = sizeof res_mode / sizeof res_mode[0] };
    {
        int ri = RES_MATCH_AT, rdef = RES_MATCH_AT;  // (rdef: the GPU's default - Dynamic on an integrated GPU)
        for (int i = 0; i < NRES; i++) {
            if (res_mode[i] == g_settings.res_mode) ri = i;
            if (res_mode[i] == g_default_res_mode) rdef = i;
        }
        float sx, sy;
        renderer_get_scale(&sx, &sy);
        char d[1024];  // (in the menus' language already: the row leaves it as it is)
        int n = snprintf(d, sizeof d,
                         g_gpu_integrated
                             ? L("How sharp the 3D image is. Match display uses your screen's resolution, and Dynamic "
                                 "goes as sharp as your GPU can at full speed. 1x to 4x multiply the Switch's 1080p, and "
                                 "1x is 720p on this GPU. 75%%, 50%% and 25%% are for slower machines.\n\nRendering at "
                                 "%.0f x %.0f.")
                             : L("How sharp the 3D image is. Match display uses your screen's resolution, and Dynamic "
                                 "goes as sharp as your GPU can at full speed. 1x to 4x multiply the Switch's 1080p. 75%%, "
                                 "50%% and 25%% are for slower machines.\n\nRendering at %.0f x %.0f."),
                         1920.0 * sx, 1080.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC && n > 0 && n < (int)sizeof d) {
            char dyn[256];
            snprintf(dyn, sizeof dyn, L("Dynamic is at %.0f%% of the display right now."), output_scale_dynamic_factor() * 100);
            snprintf(d + n, sizeof d - n, " %s", dyn);
        }
        char note[32];  // (the preview's value: "50% (800×450)", its text the user's - 2026-11-03)
        snprintf(note, sizeof note, "%.0f×%.0f", 1920.0 * sx, 1080.0 * sy);
        preview_text(LN("Configure the resolution used for rendering the game. Higher values are more demanding on "
                        "your graphics hardware."), note);
        if (opt_preview("Internal resolution", &ri, res, NRES, rdef, d, false))
            g_settings.res_mode = res_mode[ri], g_settings.res_auto = false;
    }
    {   // each OS's own upscaler next to bilinear (UPSCALE_SHARP: MetalFX on macOS, AMD FSR 1 on Windows)
#ifdef __APPLE__
        static const char *ups[] = {LN("Bilinear"), LN("MetalFX")};
        const char *ud = LN("How the picture is scaled up when it renders below your window's resolution. MetalFX is "
                            "Apple's upscaler and looks sharper, Bilinear is plain and soft.");
#else
        static const char *ups[] = {LN("Bilinear"), LN("AMD FSR 1")};
        const char *ud = LN("How the picture is scaled up when it renders below your window's resolution. AMD FSR 1 "
                            "sharpens edges as it scales, Bilinear is plain and soft.");
#endif
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : 1;
        char udm[1024];
        if (g_settings.res_mode == RES_MATCH) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, "%s\n\n%s", L(ud),
                     L("Does nothing at Match display, since the game already renders at your screen's resolution."));
            ud = udm;
            opt_disable_next();
        }
        preview_text(LN("Configure how the game is scaled up to fit your window."), NULL);
        if (opt_preview("Upscaler", &ui, ups, 2, g_default_upscaler == UPSCALE_BILINEAR ? 0 : 1, ud))
            g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR, g_settings.upscaler_auto = false;
    }
    static const char *aa[] = {LN("Off"), LN("SMAA")};
    preview_text(LN("Configure the smoothing of jagged edges. SMAA keeps textures sharp."), NULL);
    opt_preview("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges. SMAA keeps textures sharp and helps most at a lower internal resolution.");

    opt_end();
}

extern "C" bool cosmetics_seed_on(void);      // runtime/cosmetics_seed.c
extern "C" void cosmetics_seed_apply(void);
extern "C" bool weather_cycle_on(void);       // runtime/nvn/weather_cycle.c

// Cosmetics: tunic / heart colours, live (runtime/nvn/texpatch.c), and recolours by draw (runtime/nvn/recolour*.c);
// a seed made with "Random cosmetics" puts its own set into the rows the first time it's played (cosmetics_seed.c),
// and they stay the player's to change: "Seed's look" puts the seed's back, "Default all" the game's own
static void tab_cosmetics(void) {
    opt_begin("##cosmetics", ImGui::GetFontSize() / 34);
    CosColour *rows[] = {&g_settings.tunic, &g_settings.hearts, &g_settings.hair, &g_settings.skin, &g_settings.shield,
                         &g_settings.leaves, &g_settings.grass, &g_settings.sand, &g_settings.water,
                         &g_settings.fences, &g_settings.walls, &g_settings.houses};
    if (opt_button("Randomize all", "Picks a random color for everything below."))
        for (CosColour *c : rows) c->pick = 1 + rand() % (COL_RANDOM - 1);  // (a named colour: shown as what it is)
    if (opt_button("Default all", "Puts everything on this page back to the game's normal look.")) {
        for (CosColour *c : rows) c->pick = COL_DEFAULT;
        g_settings.enemy_colours = 0;
        g_settings.screen_filter = 0;
        g_settings.weather = 0;
    }
    if (cosmetics_seed_on() && opt_button("Seed's look", "Puts everything on this page back to this seed's look."))
        cosmetics_seed_apply();
    opt_section("Colors");
    opt_colour("Tunic color", &g_settings.tunic, COL_SLOT_TUNIC, "Link's tunic color.");
    opt_colour("Heart color", &g_settings.hearts, COL_SLOT_HEARTS, "The color of the hearts on the HUD.");
    opt_colour("Hair color", &g_settings.hair, COL_SLOT_HAIR, "Link's hair color.");
    opt_colour("Body color", &g_settings.skin, COL_SLOT_SKIN, "Link's body color.");  // (larecomp.ini: skin)
    opt_colour("Shield color", &g_settings.shield, COL_SLOT_SHIELD, "The color of Link's shield.");
    opt_section("Overworld");
    {   // Season: fills Leaves, Grass and Sand at once (palette: Orange 2, Gold 3, Pink 9, White 10; Water stays out: it
        // has no season, and white water would read as ice; nor fences, walls or houses: wood and stone have none);
        // "Mixed" when they match none
        static const char *const names[] = {LN("Spring"), LN("Autumn"), LN("Winter"),
                                             LN("Cherry blossom"), LN("Mixed")};
        static const int preset[4][3] = {{COL_DEFAULT, COL_DEFAULT, COL_DEFAULT}, {2, 3, 3}, {10, 10, 10},
                                         {9, COL_DEFAULT, COL_DEFAULT}};
        int cur = 4;
        for (int i = 0; i < 4; i++)
            if (g_settings.leaves.pick == preset[i][0] && g_settings.grass.pick == preset[i][1] &&
                g_settings.sand.pick == preset[i][2])
                cur = i;
        int v = cur;
        // (Mixed is never chosen: it's there only while the colours match no season; Right stops at Cherry blossom)
        if (opt_choice("Season", &v, names, cur == 4 ? 5 : 4, 0, "Sets the leaves, grass and sand colors below.") && v != cur) {
            g_settings.leaves.pick = preset[v][0];
            g_settings.grass.pick = preset[v][1];
            g_settings.sand.pick = preset[v][2];
        }
    }
    opt_colour("Leaves color", &g_settings.leaves, COL_SLOT_LEAVES, "The overworld's tree leaves and bushes.");
    opt_colour("Grass color", &g_settings.grass, COL_SLOT_GRASS, "The overworld's grass.");
    opt_colour("Sand color", &g_settings.sand, COL_SLOT_SAND, "The beaches, the desert and other sandy ground.");
    opt_colour("Water color", &g_settings.water, COL_SLOT_WATER, "The sea, rivers, ponds, waterfalls and dungeon water.");
    opt_colour("Fence color", &g_settings.fences, COL_SLOT_FENCES, "The wooden fences.");
    opt_colour("Wall color", &g_settings.walls, COL_SLOT_WALLS, "The overworld's cliff walls and ledges.");
    opt_colour("House color", &g_settings.houses, COL_SLOT_HOUSES, "The roofs of the village houses.");
    opt_section("Enemies");
    opt_choice("Enemy colors", &g_settings.enemy_colours, g_enemy_colour_modes, N_ENEMY_COLOUR_MODES, 0,
               "Gives each kind of enemy a new color. Enemies whose color shows how strong they are keep theirs.");
    opt_section("Screen");
    opt_preview("Screen filter", &g_settings.screen_filter, g_screen_filters, N_SCREEN_FILTERS, 0,
               "Puts a filter over the whole screen, HUD included.");
#ifdef __APPLE__  // (the Metal renderer draws it; Vulkan: not yet - the setting does nothing there)
    {   // (the seed's weather cycle: "Seed", the seed's weather - locked)
        const char *names[N_WEATHERS];
        for (int i = 0; i < N_WEATHERS; i++) names[i] = g_weathers[i];
        bool cycle = weather_cycle_on();
        int shown = 0;
        if (cycle) names[0] = LN("Seed"), opt_disable_next();
        opt_choice("Weather", cycle ? &shown : &g_settings.weather, names, N_WEATHERS, 0,
                   cycle ? "This seed picked the weather. Commit to it!" : "Rain, snow, a storm or a blizzard on the overworld.");
    }
#endif
    opt_end();
}

static void tab_audio(void) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Volume");
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is normal, higher is louder."))
        g_settings.volume = pct / 100.0f;
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the opening and ending movies. Changes the next time a movie plays.");
    opt_section("Effects");
    opt_toggle("Enable reverb", &g_settings.reverb, true,
               "The game's echo in caves, dungeons and big rooms.");
    opt_toggle("LAS Recomp menu sounds", &g_settings.menu_sounds, true,
               "Sounds when you move around and pick things in LAS Recomp's menus.");
    opt_end();
}

extern "C" void rt_request_reboot(int kind);  // runtime/main.c (rt.h's RT_REBOOT_GAME / RT_REBOOT_TITLE)

// Reset and Quit (the bar's entries): a question over the bar, as Reset to vanilla's (user 2026-11-03). Reset: the game
// boots again (the intro movie). Quit: back to the start screen ("Return to Launcher") or out to the desktop. B / Esc
// is Cancel, where the cursor starts.
static void ask_popup(float scale) {
    if (g_ask >= 0 && !ImGui::IsPopupOpen("##ask")) ImGui::OpenPopup("##ask");
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##ask", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                                   ImGuiWindowFlags_NoMove))
        return;
    bool quit = g_ask == BAR_QUIT;
    // LARECOMP_MENU_ASK=cancel / launcher / reset (test): presses that button a few frames after the question opens
    static const char *test = getenv("LARECOMP_MENU_ASK");
    static int frames;
    frames++;
    bool test_cancel = test && frames == 30 && !strcmp(test, "cancel");
    bool test_launcher = test && frames == 30 && !strcmp(test, "launcher");
    bool test_reset = test && frames == 30 && !strcmp(test, "reset");
    ImGui::PushFont(g_menu_font_big);
    ImGui::TextColored(plastic_col(plastic_theme().title), "%s", quit ? L("Quit") : L("Reset Game"));
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextUnformatted(L("Progress since your last save is lost."));
    ImGui::Spacing();
    ImGui::Spacing();
    ImVec2 bs(260 * scale, 0);
    bool done = false;
    if (ImGui::Button(L("Cancel"), bs) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false) || test_cancel) {
        menu_sfx_play(MENU_SFX_CANCEL);
        done = true;
    }
    ImGui::SetItemDefaultFocus();
    if (!quit) {
        ImGui::SameLine();
        if (ImGui::Button(L("Reset"), bs) || test_reset) {
            LOG("[menu] reset: the game boots again\n");
            settings_save();
            rt_request_reboot(RT_REBOOT_GAME);
            done = true;
            set_open(false);
        }
    } else {
        ImGui::SameLine();
        if (ImGui::Button(L("Return to Launcher"), bs) || test_launcher) {
            LOG("[menu] quit: back to the start screen\n");
            settings_save();
            rt_request_reboot(RT_REBOOT_TITLE);
            done = true;
            set_open(false);
        }
        ImGui::SameLine();
        if (ImGui::Button(L("Quit"), bs)) {
            LOG("[menu] quit: to the desktop\n");
            settings_save();
            SDL_Event q = {};
            q.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&q);
            done = true;
        }
    }
    if (done) g_ask = -1, frames = 0, ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Reset to vanilla (the round arrow beside the close X): the game as Nintendo made it - 16:9, the Gameplay tab (but
// its language and LiveSplit address), Cheats and Cosmetics at their defaults; Video / Audio otherwise and the controls
// stay. A seed being played ends: the game boots again without it (as the Randomizer tab's Switch to vanilla).
static void reset_to_vanilla(void) {
    Settings &s = g_settings;
    s.aspect = ASPECT_ORIGINAL;
    s.cheat_hookshot = s.no_buff_drops = s.mirror_mode = false;
    s.walk_speed = 1.0f, s.walk_speed_dungeons = false;
    s.diff_damage = 1.0f, s.diff_ohko = false;
    s.timer = false, s.timer_pos = TIMER_POS_LEFT;
    if (s.livesplit) s.livesplit = false, livesplit_poke();
    s.cheat_hearts = s.cheat_ammo = s.cheat_rupees = false;
    for (CosColour *c : {&s.tunic, &s.hearts, &s.hair, &s.skin, &s.shield, &s.leaves, &s.grass, &s.sand, &s.water,
                         &s.fences, &s.walls, &s.houses})
        c->pick = COL_DEFAULT;
    s.enemy_colours = 0, s.screen_filter = 0;
    LOG("[menu] reset to vanilla%s\n", rando_seed_dir() ? " (leaving the seed)" : "");
    settings_save();
    if (rando_seed_dir()) {
        rando_select("");
        rt_request_reboot(RT_REBOOT_GAME);
        set_open(false);
    }
}

static void vanilla_popup(float scale) {
    // LARECOMP_MENU_VANILLA=ask / reset / cancel (test): the question opens with the menu; reset / cancel press that
    // button a few frames later
    static const char *test = getenv("LARECOMP_MENU_VANILLA");
    static int test_frames;
    if (test && !test_frames++) ImGui::OpenPopup("##vanilla");
    bool test_reset = test && test_frames == 30 && !strcmp(test, "reset");
    bool test_cancel = test && test_frames == 30 && !strcmp(test, "cancel");
    if (!ImGui::BeginPopupModal("##vanilla", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(plastic_col(plastic_theme().title), "%s", L("Reset to vanilla?"));
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextUnformatted(L("This resets your settings to the vanilla game experience.\nYour controls stay as they are."));
    if (rando_seed_dir()) {
        ImGui::Spacing();
        ImGui::TextUnformatted(L("This also ends the current randomizer seed.\nIts save files are kept."));
    }
    ImGui::Spacing();
    ImGui::Spacing();
    if (ImGui::Button(L("Reset"), ImVec2(220 * scale, 0)) || test_reset) {
        ImGui::CloseCurrentPopup();
        reset_to_vanilla();
    }
    ImGui::SameLine();
    ImGui::SetItemDefaultFocus();
    if (ImGui::Button(L("Cancel"), ImVec2(220 * scale, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false) || test_cancel) {
        LOG("[menu] reset to vanilla cancelled\n");
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

extern "C" bool inventory_overlay_wanted(void);
extern "C" bool tracker_overlay_wanted(void);
extern "C" bool tracker_dungeon_wanted(void);
extern "C" void tracker_dungeon_draw(ImVec2 display, ImFont *font);
extern "C" void tracker_overlay_draw(ImVec2 display, ImFont *font);
extern "C" bool chaos_overlay_wanted(void);
extern "C" void chaos_overlay_draw(ImVec2 display, ImFont *font);
extern "C" void inventory_overlay_draw(ImVec2 display, ImFont *font);
extern "C" bool race_overlay_wanted(void);
extern "C" void race_overlay_draw(ImVec2 display, ImFont *font, float scale);
extern "C" bool pickup_flyin_wanted(void);  // (silent pickups: pickup_flyin.mm)
extern "C" void pickup_flyin_draw(ImVec2 display, ImFont *font, float scale);
extern "C" bool race_overlay_key(char *out, size_t cap);
extern "C" bool dpad_overlay_wanted(void);  // (d-pad items' HUD cross: dpad_overlay.mm)
extern "C" void dpad_overlay_key(char *out, size_t cap);
extern "C" void dpad_overlay_draw(ImVec2 display, ImFont *font, float scale);

// ---- reusing the overlay's last drawing (LARECOMP_OVERLAY_CACHE=0: off, a new ImGui frame every present as before)
// While only the FPS counter and / or the run timer are up (no menu, no input, nothing interactive or animated), the
// overlay draws the same thing frame after frame: ImGui's NewFrame + layout + Render then run for nothing on the
// render thread. Their drawing is described by a key (the text shown, the window size); while it stays the same, the
// last frame's draw data (ImGui keeps it until the next NewFrame) is drawn again. Only after three frames in a row
// built with that key: an auto-sized window (the FPS counter) settles over the frames after its text changes.
// The FPS counter then shows the same average as ImGui's io.Framerate (the last 60 frame times), sampled twice a
// second instead of every frame.
static int g_ov_cache = -1;
static char g_ov_key[288];
static int g_ov_same;  // frames built in a row with g_ov_key
static float g_fps_dt[60];
static int g_fps_n, g_fps_i;
static double g_fps_sum, g_fps_shown, g_fps_at;
static void fps_note(double now, float dt) {
    if (g_fps_n == 60) g_fps_sum -= g_fps_dt[g_fps_i];
    else g_fps_n++;
    g_fps_dt[g_fps_i] = dt;
    g_fps_sum += dt;
    g_fps_i = (g_fps_i + 1) % 60;
    if (now - g_fps_at >= 0.5 || now < g_fps_at) {
        g_fps_at = now;
        g_fps_shown = g_fps_sum > 0 ? g_fps_n / g_fps_sum : 0;
    }
}

// Reboot (runtime/main.c): the game is gone - menu and name dialog close; the ImGui context stays.
RT_ON_RESET(menu_reset) {
    set_open(false);
    g_kbd = 0;
    osk_cancel();
    pthread_mutex_lock(&q_lock);
    q_events.clear();
    pthread_mutex_unlock(&q_lock);
}

static void keyboard_draw(ImGuiIO &io, float scale) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                                                  IM_COL32(0, 0, 0, (int)(plastic_theme().dim * 255)));
    float w = fminf(io.DisplaySize.x * 0.9f, 1100 * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
    ImGui::Begin("##kbd", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(g_font_big);
    ImGui::TextColored(plastic_col(plastic_theme().title), "%s", L("Enter your name"));
    ImGui::PopFont();
    static bool was_open, was_pad;
    bool pad = g_last_pad.load(), appearing = !was_open || pad != was_pad;  // (switching device: focus the new one)
    was_open = true, was_pad = pad;
    if (!pad) {  // keyboard: type straight into the field
        if (appearing) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        ImGui::PushFont(g_font_big);
        ImGui::InputText("##name", g_kbd_text, (size_t)(g_kbd_max * 4 + 1 < (int)sizeof g_kbd_text ? g_kbd_max * 4 + 1 : sizeof g_kbd_text),
                         ImGuiInputTextFlags_CallbackCharFilter,
                         [](ImGuiInputTextCallbackData *d) -> int { return d->EventChar < 0x20 ? 1 : 0; });
        ImGui::PopFont();
        int units = kbd_cut(g_kbd_text, g_kbd_max);
        ImGui::TextDisabled(L("Enter  confirm      Esc  cancel      (%d / %d)"), units, g_kbd_max);
    } else {  // controller: on-screen keyboard (osk.mm, shared with our menus' text fields)
        int r = osk_keys(g_kbd_text, sizeof g_kbd_text, g_kbd_max, appearing, scale, g_font_big);
        if (r) g_kbd = r > 0 ? 2 : 3;  // (Start / + confirms in menu_handle_event too)
    }
    ImGui::End();
    if (g_kbd.load() != 1) was_open = false;
}

// Game thread (runtime/hle/swkbd.c): show the dialog, re-present the last frame until it's answered.
// Returns 1 confirmed (out = text, UTF-8), 0 cancelled.
extern "C" int menu_keyboard_run(const char *initial, int max_len, char *out, int cap) {
    extern void backend_represent(void);
    snprintf(g_kbd_text, sizeof g_kbd_text, "%s", initial ? initial : "");
    g_kbd_max = max_len > 0 && max_len < (int)sizeof g_kbd_text / 4 ? max_len : 8;
    input_reset();
    g_kbd = 1;
    // (test) LARECOMP_KBD_AUTO=<text>: answer at once
    if (getenv("LARECOMP_KBD_AUTO")) { snprintf(g_kbd_text, sizeof g_kbd_text, "%s", getenv("LARECOMP_KBD_AUTO")); g_kbd = 2; }
    while (g_kbd.load() == 1 && !g_rt_unwind) {  // ~60 Hz (a reboot cancels)
        uint64_t t0 = SDL_GetTicks();
        backend_represent();
        uint64_t dt = SDL_GetTicks() - t0;
        if (dt < 16) SDL_Delay((uint32_t)(16 - dt));
    }
    kbd_cut(g_kbd_text, g_kbd_max);  // (whatever entered it: typed, on-screen keys, the test hook)
    bool ok = g_kbd.load() == 2 && g_kbd_text[0];
    if (ok) snprintf(out, cap, "%s", g_kbd_text);
    g_kbd = 0;
    input_reset();
    return ok;
}

// LARECOMP_MENU_FOCUS_LOG=1 (test; tools/menu_focus_sweep.sh): every Down / Up on a page of option rows is checked a
// few frames later against the rows' order and logged "[menu] focus check ... OK / FAIL": the first Down after a tab
// switch (however it came) or the menu opening, and Down from the tab bar, land on the page's first enabled row; Down
// goes to the next row (the last one stays), Up to the row before, and Up from the top row onto the selected tab.
static void focus_check(bool down, bool up, bool any_down, int focused_tab, int tab, const char *tab_name) {
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_MENU_FOCUS_LOG") != NULL;
    if (!on) return;
    static int shown = -1000, due_in, want, from, prev_focus = -1;  // (want: a row index, -1: the selected tab)
    static bool first_down_due;
    static const char *what;
    if (tab != shown || g_page_fresh_seen) shown = tab, first_down_due = true, g_page_fresh_seen = false, prev_focus = -1;
    int n = opt_row_count(), first = opt_first_row(), focus = opt_focused_row();
    if (any_down && !down) first_down_due = false;  // (a held Down's repeat: the first Down was checked or skipped)
    if ((down || up) && !due_in && n > 0) {
        from = prev_focus, due_in = 4;
        if (down && (first_down_due || from < 0)) want = first, what = first_down_due ? "first Down" : "Down from the tab bar";
        else if (down) want = from + 1 < n ? from + 1 : from, what = "Down";
        else if (from > 0) want = from - 1, what = "Up";
        else if (from == 0) want = -1, what = "Up from the top row";
        else due_in = 0;  // (Up on the tab bar: not a page move)
        if (down) first_down_due = false;
    } else if (down && n == 0 && first_down_due) {
        first_down_due = false;
        LOG("[menu] focus check: first Down on %s SKIP (no option rows)\n", tab_name);
    }
    prev_focus = focus;
    if (!due_in || --due_in) return;
    bool ok = want >= 0 ? focus == want : focused_tab == tab;
    char got[160];
    if (focus >= 0) snprintf(got, sizeof got, "\"%s\"", opt_row_label(focus));
    else snprintf(got, sizeof got, "tab %d", focused_tab);
    char exp[160];
    if (want >= 0) snprintf(exp, sizeof exp, "\"%s\"", opt_row_label(want));
    else snprintf(exp, sizeof exp, "tab %d", tab);
    LOG("[menu] focus check: %s on %s from %s -> %s (want %s) %s\n", what, tab_name,
        from >= 0 ? opt_row_label(from) : "the tab bar", got, exp, ok ? "OK" : "FAIL");
}

// The tab bar and its pages, into the current window: the in-game menu, and the start screen's Settings (in_game
// The live preview (live_preview.h) of a row on the page shown: instead of the tabs, until it closes. The pages with
// opt_preview rows: Video, Cosmetics.
static void preview_page(void) {
    if (g_tab == 0) tab_display();
    else if (g_tab == 7) tab_cosmetics();
}
bool menu_preview_frame(float scale, ImFont *font, ImFont *font_big) {
    return preview_active() && (preview_draw(scale, font, font_big, preview_page), true);
}

// false: nothing of the game is loaded yet - the tabs that need it are greyed out; prelaunch: the start screen's own
// first tab, game files and fonts, drawn by launcher.mm). only: one page alone, without the tab bar (the in-game menu
// bar's Warp, Save Editor and Quit).
static void tabs_body(float scale, bool in_game, void (*prelaunch)(float), int only = -1000) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[11];
    int ntabs = 0;
    if (prelaunch) tabs[ntabs++] = {LN("Game files"), -1, true};
    static const char *names[] = {LN("Video"), LN("Audio"), LN("Save Editor"), LN("Randomizer"), LN("Warp"),
                                  LN("Cheats"), LN("Gameplay"), LN("Cosmetics"), LN("Controls"), LN("Quit")};
    // Controls, Video, Audio, Gameplay, Cosmetics, Randomizer, Cheats (ids stay: test hooks). Warp, Save Editor and
    // Quit are the in-game menu bar's own entries (only usable in a save - user 2026-11-03)
    static const int order[] = {8, 0, 1, 6, 7, 3, 5};
    int keep_tab = g_tab;  // (one page alone: the Settings window's tab stays as it was)
    if (only != -1000) tabs[ntabs++] = {names[only], only, true}, g_tab = only;
    else
        for (int i : order) tabs[ntabs++] = {names[i], i, in_game || i != 5};
    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones and stopping at the first / last (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    static const int cycle = getenv("LARECOMP_MENU_TAB_CYCLE") ? atoi(getenv("LARECOMP_MENU_TAB_CYCLE")) : 0;
    static int cycle_frames, cycle_i;  // (test) <frames>: each tab in turn, that many frames each (screenshot series)
    if (cycle > 0 && cycle_frames++ % cycle == 0 && cycle_i < ntabs) g_tab_request_ext = tabs[cycle_i++].id;
    if (g_tab_request_ext >= 0) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -1; }
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    bool switching = false;  // (L / R this frame: the page changes next frame)
    if (!any_popup && !typing && only == -1000) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        int i = cur;
        if (step) do i += step; while (i >= 0 && i < ntabs && !tabs[i].on);
        if (step && i >= 0 && i < ntabs) {
            g_tab_request = tabs[i].id;
            // the cursor goes onto the new tab (left behind on the tab bar, A on it switched straight back - user
            // 2026-10-11; left in the old page, it stayed on a row that is gone, and Down went on from where that row
            // had been)
            g_focus_tab = tabs[i].id, g_focus_frames = 0;
            switching = true;
            menu_sfx_play(MENU_SFX_TAB);
        }
    }
    bool down = !any_popup && !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown) ||
                                          ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown) ||
                                          ImGui::IsKeyPressed(ImGuiKey_DownArrow));
    bool up = !any_popup && !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp) ||
                                        ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp) || ImGui::IsKeyPressed(ImGuiKey_UpArrow));
    // The first Down on a page just switched to (L / R, a click or A on a tab, the menu opening), or Down with the
    // cursor on the tab bar: the page's first enabled row, top to bottom - never ImGui's pick from wherever its cursor
    // was (a row of the page before, the tab bar's x). Option rows take it (opt_focus_first); a page without them gets
    // its first control (g_page_focus_next).
    if (ImGui::GetIO().MouseClicked[0]) g_page_fresh = false;  // (the mouse went into the page)
    bool down_from_tab = down && !switching &&
                         ((focused_tab != -99 && ImGui::GetCurrentContext()->NavCursorVisible) || g_page_fresh);
    if (down_from_tab) {
        ImGui::NavMoveRequestCancel();  // (ImGui's own move this frame)
        g_page_fresh = false;
        g_focus_tab = -99, g_focus_frames = 0;  // (the cursor stops going back to the tab)
    }
    opt_focus_first(down_from_tab);
    // Up / Down on a page of option rows: the next / previous row in order (opt_nav_step), not ImGui's spatial pick
    if ((down || up) && !down_from_tab && !switching && opt_nav_step(down ? 1 : -1)) ImGui::NavMoveRequestCancel();
    opt_frame_reset();
    // The cursor moved out of the page (ImGui's pick, applied this frame for last frame's press - Up from the page's
    // top control): onto the selected tab. ImGui's pick was the tab nearest the control's x (Controls, from most
    // rows), or the close button
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiWindow *root = ImGui::GetCurrentWindow()->RootWindow;
    static bool nav_in_page;  // (the frame before: the cursor on a control of the page - its child windows)
    if (nav_in_page && g.NavJustMovedToId && g.NavWindow == root && !any_popup) g_focus_tab = -100, g_focus_frames = 0;
    // The tabs: a row of our own pieces (the selected one orange), each a button the controller's cursor can stand on;
    // the selected tab's page under them
    if (g_tab_request != -2)
        for (int k = 0; k < ntabs; k++)
            if (tabs[k].id == g_tab_request && tabs[k].on) g_tab = tabs[k].id;
    g_tab_request = -2;
    if (!tabs[cur].on || tabs[cur].id != g_tab) {
        bool found = false;
        for (int k = 0; k < ntabs; k++) found |= tabs[k].id == g_tab && tabs[k].on;
        if (!found) g_tab = tabs[cur].on ? tabs[cur].id : tabs[0].id;
    }
    if (only == -1000) {
        const PlasticTheme &pt = plastic_theme();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float gap = 8 * scale, pad = 20 * scale, th = ImGui::GetFontSize() + 20 * scale;
        float tw[16], text_w[16], total = 0, avail = ImGui::GetContentRegionAvail().x - 4 * scale;
        for (int k = 0; k < ntabs; k++) text_w[k] = ImGui::CalcTextSize(L(tabs[k].name)).x, total += text_w[k] + (k ? gap : 0);
        if (total + 2 * pad * ntabs > avail) pad = fmaxf((avail - total) / (2 * ntabs), 3 * scale);  // (a narrow window: tighter)
        // still too wide (long names: Russian, German): the names a little smaller
        float tfs = ImGui::GetFontSize(), names = total - gap * (ntabs - 1), room = avail - gap * (ntabs - 1) - (2 * pad + 2 * scale) * ntabs;
        if (names > room && names > 0) {
            float k = fmaxf(room / names, 0.6f);
            tfs *= k;
            for (int i = 0; i < ntabs; i++) text_w[i] *= k;
        }
        for (int k = 0; k < ntabs; k++) tw[k] = text_w[k] + 2 * pad;
        if (g_focus_tab == -1 && !prelaunch) g_focus_tab = 8;  // (no Prelaunch in game: Controls, the first tab)
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGui::BeginDisabled(!t.on);  // (out of the game: Save Editor, Warp, Cheats)
            if (k) ImGui::SameLine(0, gap);
            ImVec2 p = ImGui::GetCursorScreenPos();
            p.x += 2 * scale;
            ImGui::SetCursorScreenPos(p);
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the piece shows the cursor)
            ImGui::PushID(t.id + 2);
            bool clicked = ImGui::InvisibleButton("##tab", ImVec2(tw[k], th),
                                                  ImGuiButtonFlags_EnableNav | ImGuiButtonFlags_PressedOnClick);
            menu_sfx_tab_item();
            ImGui::PopID();
            ImGui::PopStyleColor();
            bool focus_here = g_focus_tab == t.id || (g_focus_tab == -100 && t.id == g_tab);
            if (focus_here) {  // the controller's cursor starts on this tab
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                // a few frames: the page's child window appearing makes ImGui pick its first widget once it's laid out
                if (++g_focus_frames >= 4) g_focus_tab = -99, g_focus_frames = 0;
                else g_focus_tab = t.id;
            }
            if (clicked && t.on) g_tab = t.id;
            bool sel = t.id == g_tab;
            bool lit = (ImGui::IsItemFocused() && g.NavCursorVisible) || ImGui::IsItemHovered();
            ImVec4 col = sel ? pt.tab_sel : lit ? pt.row_mark : pt.tab;
            if (sel && lit) col = ImVec4(col.x * 0.88f, col.y * 0.88f, col.z * 0.88f, col.w);  // (the cursor on it: deeper)
            if (!t.on) col.w *= 0.45f;
            plastic_pill(dl, ImVec2(p.x, p.y + (sel ? 0 : 2 * scale)), ImVec2(p.x + tw[k], p.y + th - 2 * scale), 10 * scale, col,
                         (sel ? 7 : 4) * scale, sel ? 1.0f : 0.55f, scale, sel);
            ImU32 tc = sel ? pt.text_sel : t.on ? pt.text : pt.text_dim;
            ImVec2 ts(text_w[k], tfs);
            dl->AddText(ImGui::GetFont(), tfs, ImVec2(p.x + (tw[k] - ts.x) * 0.5f, p.y + (th - ts.y) * 0.5f - (sel ? 2 : 1) * scale),
                        tc, L(t.name));
            if (ImGui::IsItemFocused()) focused_tab = t.id;
            else if (focused_tab == t.id) focused_tab = -99;
            ImGui::EndDisabled();
        }
    }
    for (int k = 0; k < ntabs; k++) {
        const Tab &t = tabs[k];
        if (t.id != g_tab) continue;
        if (t.id != g_shown_tab) g_shown_tab = t.id, g_page_fresh = true, g_page_focus_next = false;
        ImGui::Dummy(ImVec2(0, 4 * scale));
        ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_None,  // (the rest of the window: no hint line)
                          ImGuiWindowFlags_NavFlattened);
        if (g_page_focus_next) {  // (a page without option rows: its first control)
            g_page_focus_next = false;
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNavCursorVisible(true);
        }
        switch (t.id) {
        case -1: prelaunch(scale); break;
        case 0: tab_display(); break;
        case 1: tab_audio(); break;
        case 2:
            if (game_in_play()) save_editor_draw(scale);
            else ImGui::TextDisabled("%s", L("Load a save to edit it."));
            break;
        case 3: rando_tab_draw(scale); break;
        case 4: if (warp_tab_draw(scale)) set_open(false); break;
        case 5: cheats_draw(scale); break;
        case 6: difficulty_draw(scale); break;
        case 7: tab_cosmetics(); break;
        case 8: controls_tab_draw(scale); break;
        }
        static const char *scroll_test = getenv("LARECOMP_MENU_SCROLL");  // test hook: page scroll, 0..1
        if (scroll_test) ImGui::SetScrollY(ImGui::GetScrollMaxY() * (float)atof(scroll_test));
        // no enabled option row took the cursor: the page's first control, next frame (SetKeyboardFocusHere: the
        // first item that takes the cursor, in order)
        if (opt_focus_first_left() && down_from_tab) g_page_focus_next = true;
        ImGui::EndChild();
    }
    opt_focus_first_left();  // (no page drawn: nothing took it)
    nav_in_page = g.NavWindow && g.NavWindow != g.NavWindow->RootWindow && g.NavWindow->RootWindow == root;
    // (checks: fresh presses only - a held key's repeats come faster than a check's few frames)
    bool down1 = down && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, false) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, false));
    bool up1 = up && (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp, false) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, false));
    for (int k = 0; k < ntabs; k++)
        if (tabs[k].id == g_tab) focus_check(down1, up1, down, focused_tab, g_tab, tabs[k].name);
    if (only != -1000) g_tab = keep_tab;
}

// The menu's look for a window of the start screen's (its game files window): until menu_style_end
static ImGuiStyle g_style_saved;
void menu_style_begin(float scale) {
    g_style_saved = ImGui::GetStyle();
    style_init();
    style_parchment(ImGui::GetStyle());
    ImGui::GetStyle().ScaleAllSizes(scale);
}
void menu_style_end(void) { ImGui::GetStyle() = g_style_saved; }

void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float)) {
    ImFont *f0 = g_font, *f1 = g_font_big;
    g_font = font, g_font_big = font_big;
    ImGuiStyle saved = ImGui::GetStyle();  // the menu's look (the start screen has its own style)
    style_init();
    style_parchment(ImGui::GetStyle());
    ImGui::GetStyle().ScaleAllSizes(scale);
    tabs_body(scale, in_game, prelaunch);
    ImGui::GetStyle() = saved;
    g_font = f0, g_font_big = f1;
}

// The menu bar (in game; its window is drawn in the menu's font), only while no page is open: left / right (d-pad,
// stick, arrows, L / R, Q / E) move the cursor, A / Enter opens the entry, B / Esc closes the menu. The mouse lights
// the entry under it, a click opens it (the page is drawn from the next frame: the press that opened it isn't the
// page's too).
static double g_bar_t0 = -1;  // when the bar began to drop (-1: not shown - the menu is closed or a page is open)
static void bar_draw(ImGuiIO &io, float scale) {
    static const char *names[BAR_N] = {LN("Settings"), LN("Warp"), LN("Save Editor"), LN("Reset"), LN("Quit"), ""};
    const PlasticTheme &pt = plastic_theme();
    double now = ImGui::GetTime();
    if (g_bar_t0 < 0) g_bar_t0 = now;
    float e = fminf((float)(now - g_bar_t0) / 0.18f, 1);  // (drops down from above the screen, slowing as it lands)
    e = 1 - (1 - e) * (1 - e) * (1 - e);
    ImGui::PushFont(g_menu_font);
    float fs = ImGui::GetFontSize(), in = 30 * scale, ph = fs + 20 * scale, m = 12 * scale;
    float bh = ph + 2 * in, top = m - (1 - e) * (bh + m);
    ImGui::SetNextWindowPos(ImVec2(m, top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - 2 * m, bh), ImGuiCond_Always);
    ImGui::Begin("##bar", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    // the keys
    if (!ui_text_busy() && g_ask < 0) {
        auto pressed = [](ImGuiKey a, ImGuiKey b, ImGuiKey c) {
            return ImGui::IsKeyPressed(a) || ImGui::IsKeyPressed(b) || ImGui::IsKeyPressed(c);
        };
        int step = pressed(ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight, ImGuiKey_RightArrow) ||
                           ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                   : pressed(ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft, ImGuiKey_LeftArrow) ||
                           ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) g_bar_cur = (g_bar_cur + step + BAR_N) % BAR_N, menu_sfx_play(MENU_SFX_CURSOR);
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (g_bar_cur == BAR_CLOSE) set_open(false);
            else page_open(g_bar_cur), menu_sfx_play(MENU_SFX_DECIDE);
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            set_open(false);
    }
    // the entries: a row from the left, the cursor's lit as the selected tab is
    static ImVec2 last_mouse(-1, -1);  // (not io.MouseDelta: none for the mouse's first move)
    bool mouse_moved = io.MousePos.x != last_mouse.x || io.MousePos.y != last_mouse.y;
    last_mouse = io.MousePos;
    float x = wp.x + in, y = wp.y + in, pad = 22 * scale, gap = 10 * scale;
    for (int k = 0; k < BAR_CLOSE; k++) {
        float tw = ImGui::CalcTextSize(L(names[k])).x, w = tw + 2 * pad;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(k);
        bool clicked = ImGui::InvisibleButton("##entry", ImVec2(w, ph), ImGuiButtonFlags_PressedOnClick);
        ImGui::PopID();
        bool hot = ImGui::IsItemHovered();
        if (hot && mouse_moved && g_bar_cur != k) g_bar_cur = k, menu_sfx_play(MENU_SFX_CURSOR);
        if (clicked) page_open(k), menu_sfx_play(MENU_SFX_DECIDE);
        // (as the Settings window's tabs: tabs_body - the user wants the bar's look to match Settings, 2026-11-03)
        bool sel = k == g_bar_cur;
        ImVec4 col = sel ? pt.tab_sel : hot ? pt.row_mark : pt.tab;
        if (sel && hot) col = ImVec4(col.x * 0.88f, col.y * 0.88f, col.z * 0.88f, col.w);
        plastic_pill(dl, ImVec2(x, y + (sel ? 0 : 2 * scale)), ImVec2(x + w, y + ph - 2 * scale), 10 * scale, col,
                     (sel ? 7 : 4) * scale, sel ? 1.0f : 0.55f, scale, sel);
        dl->AddText(ImGui::GetFont(), fs, ImVec2(x + pad, y + (ph - fs) * 0.5f - (sel ? 2 : 1) * scale),
                    sel ? pt.text_sel : pt.text, L(names[k]));
        x += w + gap;
    }
    // the close X at the right end (the cursor on it: a ring round it)
    float bx = ph;
    ImVec2 xp(wp.x + ws.x - in - bx, y);
    if (g_bar_cur == BAR_CLOSE)
        plastic_pill(dl, ImVec2(xp.x - 5 * scale, xp.y - 5 * scale), ImVec2(xp.x + bx + 5 * scale, xp.y + bx + 5 * scale),
                     bx * 0.5f + 5 * scale, pt.tab_sel, 7 * scale, 1.0f, scale, true);
    ImGui::SetCursorScreenPos(xp);
    if (plastic_close_button(bx, scale)) set_open(false);
    if (ImGui::IsItemHovered() && mouse_moved && g_bar_cur != BAR_CLOSE)
        g_bar_cur = BAR_CLOSE, menu_sfx_play(MENU_SFX_CURSOR);
    ImGui::End();
    ImGui::PopFont();
}

// Whether menu_render draws anything: the menu, the keyboard, an overlay (FPS, inventory, tracker, chaos, race, a
// pickup card).
static bool render_needed(bool open, bool kbd, bool inv, bool trk, bool trkd, bool chaos, bool race, bool flyin, bool dpad) {
    return open || kbd || g_settings.show_fps || inv || trk || trkd || chaos || race || flyin || dpad;
}

#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    bool open = g_open.load();
    bool inv = !open && inventory_overlay_wanted();
    bool trk = !open && tracker_overlay_wanted();
    bool trkd = !open && tracker_dungeon_wanted();
    bool chaos = !open && chaos_overlay_wanted();
    bool kbd = g_kbd.load() == 1;
    bool race = race_overlay_wanted();  // (run timer, race seed hash: race_overlay.mm)
    bool flyin = pickup_flyin_wanted() && !open;  // (a silent pickup's card)
    bool dpad = !open && dpad_overlay_wanted();
#ifdef __APPLE__
    plastic_perf_note(cb, open);  // (LARECOMP_UI_PERF test only)
#else
    plastic_perf_note_vk(open);
#endif
    if (!render_needed(open, kbd, inv, trk, trkd, chaos, race, flyin, dpad)) {
        if (g_inited) { pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); }
        g_ov_same = 0;  // (shown again: built afresh first)
        return;
    }
#ifdef __APPLE__
    if (!g_inited) init(dev);
#else
    if (!g_inited) init();
#endif
    ImGui::SetCurrentContext(g_ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)tw, (float)th);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last;
    // (nanoseconds: two images drawn within a millisecond - an in-between and its frame on Vulkan - counted the gap as
    // 1/60 s, and 120 images a second showed as 70 fps)
    double now = SDL_GetTicksNS() / 1e9;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    if (g_ov_cache < 0) g_ov_cache = !getenv("LARECOMP_OVERLAY_CACHE") || atoi(getenv("LARECOMP_OVERLAY_CACHE")) != 0;
    char key[sizeof g_ov_key];
    key[0] = 0;
    if (g_ov_cache) {
        fps_note(now, io.DeltaTime);
        char rk[64], dk[160] = "";
        // (only the FPS counter, the run timer and the d-pad cross; input only reaches ImGui while the menu or the name
        // entry is up)
        if (!open && !kbd && !inv && !trk && !trkd && !chaos && !flyin && race_overlay_key(rk, sizeof rk)) {
            pthread_mutex_lock(&q_lock);
            bool events = !q_events.empty();
            pthread_mutex_unlock(&q_lock);
            if (dpad) dpad_overlay_key(dk, sizeof dk);
            if (!events) snprintf(key, sizeof key, "%.0fx%.0f fps %d %.0f | %s | %s", tw, th, (int)g_settings.show_fps,
                                  g_settings.show_fps ? g_fps_shown : 0.0, rk, dk);
        }
        ImDrawData *dd = ImGui::GetDrawData();
        if (key[0] && g_ov_same >= 3 && !strcmp(key, g_ov_key) && dd && dd->Valid) {
            g_text_busy = false;  // (the menu is closed)
#ifdef __APPLE__
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = target;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            ImGui_ImplMetal_NewFrame(rp);  // (the backend's target description only)
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            ImGui_ImplMetal_RenderDrawData(dd, cb, enc);
            [enc endEncoding];
#else
            void *oc = vk_overlay_begin(cmd);  // (as below)
            ImGui_ImplVulkan_RenderDrawData(dd, (VkCommandBuffer)oc);
            vk_overlay_end(cmd, oc);
#endif
            return;
        }
        if (key[0] && !strcmp(key, g_ov_key)) g_ov_same++;
        else { memcpy(g_ov_key, key, sizeof key); g_ov_same = key[0] ? 1 : 0; }
    }
    drain_events(io);
    // fonts are built at 34/56 px for a ~1080 px tall output; scale to the window
    float scale = (float)(th / 1080.0);
    if (scale < 0.45f) scale = 0.45f;
    io.FontGlobalScale = scale;

#ifdef __APPLE__
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
#endif
    // spacing/rounding follow the output size too (style tuned for 1080 p)
    // (the overlays' style, and the menu's: the parchment colours only while the menu / name entry is drawn)
    static ImGuiStyle base, ov_style, menu_style;
    static float style_scale = -1;
    if (style_scale < 0) base = ImGui::GetStyle();
    if (fabsf(style_scale - scale) > 0.01f) {
        ov_style = base;
        ov_style.ScaleAllSizes(scale);
        menu_style = base;
        style_parchment(menu_style);
        menu_style.ScaleAllSizes(scale);
        style_scale = scale;
    }
    ImGui::GetStyle() = ov_style;
    if (i18n_fonts_stale(io.Fonts)) {  // (the Language setting changed: the glyphs of the new one)
        load_fonts();
#ifdef __APPLE__
        ImGui_ImplMetal_DestroyFontsTexture();
        ImGui_ImplMetal_CreateFontsTexture(g_dev);
#else
        vk_imgui_fonts_rebuild();
#endif
    }
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();

    if (g_settings.show_fps) {
        int c = g_settings.show_fps;
        bool right = c == FPS_TOP_RIGHT || c == FPS_BOTTOM_RIGHT, bottom = c == FPS_BOTTOM_LEFT || c == FPS_BOTTOM_RIGHT;
        float m = 16 * scale;
        ImGui::SetNextWindowPos(ImVec2(right ? io.DisplaySize.x - m : m, bottom ? io.DisplaySize.y - m : m), 0,
                                ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text(L("%.0f fps"), g_ov_cache ? g_fps_shown : io.Framerate);
        ImGui::End();
    }

    if (race) race_overlay_draw(io.DisplaySize, g_menu_font_big, scale);
    if (dpad) dpad_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont(), scale);
    if (flyin) pickup_flyin_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont(), scale);
    if (trk) tracker_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (chaos) chaos_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (trkd) tracker_dungeon_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    if (inv) {
        inventory_overlay_draw(io.DisplaySize, g_font_big ? g_font_big : ImGui::GetFont());
    }

    if (open || kbd) {  // (the menu's colours; its windows, popups and buttons as plastic pieces)
        ImGui::GetStyle() = menu_style;
        plastic_widgets(true);
    }
    if (!open || preview_active()) g_bar_t0 = -1;
    if (open && preview_active()) {  // (the live preview: the game, a panel at the bottom)
        menu_preview_frame(scale, g_menu_font, g_menu_font_big);
        menu_sfx_frame(true);
    } else if (open) {
        // A page open: the bar goes up and the page has the whole screen (user 2026-11-03); B brings the bar down again
        int page = g_page.load();
        if (page < 0) bar_draw(io, scale), ask_popup(scale);
        else g_bar_t0 = -1;
        static int shown = -1;  // (a page just opened takes ImGui's focus)
        if (page < 0) shown = -1;
        if (page >= 0) {
            // dim the game behind the page (the bar alone leaves it as it is)
            ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                                                          IM_COL32(0, 0, 0, (int)(plastic_theme().dim * 255)));
            if (page != shown) ImGui::SetNextWindowFocus(), shown = page;
            float w = fminf(io.DisplaySize.x * 0.9f, 1500 * scale), h = io.DisplaySize.y - 2 * 18 * scale;  // (all the height: a slim margin)
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
            ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
            ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
            g_font_big = g_menu_font_big;
            ImGui::PushFont(g_menu_font);
            // header as on the start screen's Settings: the page's title, and a close X in the corner (B / Esc too:
            // back to the bar). ("Title screen" - rt_request_reboot(RT_REBOOT_TITLE) - is to find a new home; user
            // 2026-10-11.)
            static const char *titles[] = {LN("Settings"), LN("Warp"), LN("Save Editor")};
            ImGui::PushFont(g_font_big);
            ImGui::TextColored(plastic_col(plastic_theme().title), "%s", L(titles[page]));
            ImGui::PopFont();
            bool back = false;
            {
                float bx = 44 * scale;
                if (plastic_close_corner(bx, scale)) back = true;
                if (page == BAR_SETTINGS) {
                    if (plastic_reset_corner(bx, scale)) ImGui::OpenPopup("##vanilla");
                    vanilla_popup(scale);
                }
            }

            bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();  // (before
            static const int only[] = {-1000, 4, 2};  // (the pages' tab ids)                    //  the pages run)
            tabs_body(scale, true, NULL, only[page]);
            // B with nothing focused-deeper goes back to the bar (not the Esc / B that ends typing: typing is from the
            // frame before, so it's still true the frame a field lets go)
            if (!any_popup && !typing && !ui_text_busy() && !controls_ui_took_back() && !rando_tab_took_back() &&
                !save_editor_took_back() && !warp_tab_took_back() &&
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                back = true;
            if (back && g_page.load() == page) g_page = -1;
            ImGui::PopFont();
            g_font_big = keep_big;
            ImGui::End();
            osk_draw(g_font_big, scale);  // (a text field activated with the controller)
            menu_sfx_frame(true);
        } else menu_sfx_idle_check();
    }

    if (kbd) keyboard_draw(io, scale);
    plastic_widgets(false);
    ImGui::GetStyle() = ov_style;
    ImGui::Render();
    g_text_busy = open && ui_text_busy();
    // the pieces (and, the menu open over the game, the small copy of the game image they read): only with the menu or
    // the name entry up
    bool pieces = open || kbd;
#ifdef __APPLE__
    id<MTLTexture> scene = g_scene;
    g_scene = nil;
    if (pieces) plastic_frame_begin(dev, cb, target, open ? scene : nil, g_scene_uv, io.MousePos, scale);
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    plastic_set_encoder(pieces ? enc : nil);
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
    plastic_set_encoder(nil);
    [enc endEncoding];
#else
    if (pieces) {
        plastic_frame_begin_vk(open ? (void *)g_vk_scene_view : NULL, g_vk_scene_uv, (float)tw, (float)th, io.MousePos, scale);
    }
    // ImGui records its commands itself. Not into cmd, the window image's command buffer, while the recorder thread
    // (record.c) may still be recording it (one thread at a time on a command buffer and its pool: f054d8b, the device
    // loss entering Mabe Village), and without waiting for the recorder to catch up every frame either (a full drain:
    // 20-30 ms a frame on the Steam Deck): into a secondary command buffer of device.c's, which the recorder executes
    // in cmd in order - after the uploads queued above (overlay icons). Decided right here, where the draw data is
    // final, so an overlay turning on during this function can't miss it. (Recorder off: into cmd directly.)
    void *oc = vk_overlay_begin(cmd);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)oc);  // inside the window image's rendering
    vk_overlay_end(cmd, oc);
#endif
}
