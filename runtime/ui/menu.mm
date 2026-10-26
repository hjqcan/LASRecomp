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
#include "localization.h"
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
#include "menu.h"
#include "../photo.h"
#include "../mods.h"
#include "../workshop.h"
#include "../performance.h"
#include "../game_profiles.h"
void input_reset(void);
extern "C" bool input_second_event(const SDL_Event *);
void save_editor_draw(float scale);  // save_editor.mm
void save_backups_draw(float scale, bool in_game);
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
#include "../paths.h"
}

// ------------------------------------------------------------------ state shared with the main thread
static std::atomic<bool> g_open{false};
static std::atomic<bool> g_text_busy{false};  // a text field / the on-screen keyboard has the keys (render thread: ui_text_busy)
static std::atomic<bool> g_want_fullscreen_apply{false};

struct QEvent {
    enum { KEY, ANALOG, MOUSE_POS, MOUSE_BUTTON, WHEEL, TEXT } kind;
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
extern "C" void menu_close(void) { set_open(false); }
extern "C" void menu_test_open(void) {
    set_open(true);
    if (getenv("LARECOMP_MENU_TAB")) { extern int g_tab_request_ext; g_tab_request_ext = atoi(getenv("LARECOMP_MENU_TAB")); }
}

void menu_tabs_focus(bool first_tab);
static void set_open(bool open) {
    if (open == g_open.load()) return;
    g_open.store(open);
    input_reset();  // nothing held while the menu had focus leaks into the game
    bind_capture_cancel();
    if (open) {
        if (title_is_las()) save_editor_on_open();
        static bool opened;  // the controller's cursor: the first tab (Controls) the first time this session, later the
        menu_tabs_focus(!opened);  // selected one
        opened = true;
    }
    else { settings_save(); osk_cancel(); g_text_busy = false; }
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
static bool capturing() { return g_open.load() || g_kbd.load() == 1 || photo_mode_active(); }
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
    if(input_second_event(e))return false;
    static bool back_down, start_down;
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
    if (!typing && e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        if (e->key.scancode == SDL_SCANCODE_F2) { set_open(false); photo_mode_toggle(); return true; }
        if (photo_mode_active()) {
            if (e->key.scancode == SDL_SCANCODE_ESCAPE) { photo_mode_exit(); return true; }
            if (e->key.scancode == SDL_SCANCODE_F12) { photo_mode_capture(); return true; }
            if (e->key.scancode == SDL_SCANCODE_TAB || e->key.scancode == SDL_SCANCODE_F1) {
                photo_mode_toggle_controls(); return true;
            }
        }
    }
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
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) back_down = down;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_START) start_down = down;
        if (down && !typing && ((back_down && start_down) || e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)) {
            if (photo_mode_active()) photo_mode_exit(); else set_open(!g_open.load());
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
static int g_tab = -1000, g_tab_request = -2;  // (-2: no request; -1 is the Prelaunch tab; -1000: the first tab)
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
    c[ImGuiCol_SliderGrab] = t.frame;
    c[ImGuiCol_SliderGrabActive] = t.row_sel;
    c[ImGuiCol_PlotHistogram] = c[ImGuiCol_PlotHistogramHovered] = t.row_sel;
    c[ImGuiCol_NavCursor] = clear;  // (the pieces show the cursor themselves)
    c[ImGuiCol_Separator] = ImVec4(t.frame.x, t.frame.y, t.frame.z, 0.45f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(t.frame.x, t.frame.y, t.frame.z, 0.10f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(t.frame_inner.x, t.frame_inner.y, t.frame_inner.z, 0.85f);
    c[ImGuiCol_ScrollbarGrabHovered] = c[ImGuiCol_ScrollbarGrabActive] = t.frame;
    c[ImGuiCol_TextSelectedBg] = ImVec4(t.row_sel.x, t.row_sel.y, t.row_sel.z, 0.35f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.10f, 0.06f, 0.02f, 0.40f);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabHovered] = c[ImGuiCol_TabSelected] = clear;
    s.ScrollbarRounding = 9;
}


#ifdef __APPLE__
static void init(id<MTLDevice> dev) {
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
        if (!g_font) g_font = io.Fonts->AddFontFromFileTTF(p, 34, &cfg);
        if (g_font && !g_font_big) { ImFontConfig b = cfg; b.FontNo = 0; g_font_big = io.Fonts->AddFontFromFileTTF(p, 56, &b); }
    }
    // the menu window itself: Baloo 2 SemiBold, as on the start screen (shipped, OFL), at its Settings window's sizes; the
    // FPS counter, overlays and on-screen keyboard keep the fonts above
    char baloo[1024];
    if (const char *f = getenv("LARECOMP_MENU_FONT")) strlcpy(baloo, f, sizeof baloo);  // (test) another weight / face
    if (getenv("LARECOMP_MENU_FONT") || rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo)) {
        g_menu_font = io.Fonts->AddFontFromFileTTF(baloo, 39, &cfg);
        g_menu_font_big = io.Fonts->AddFontFromFileTTF(baloo, 72, &cfg);
    }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    if (!g_font_big) g_font_big = g_font;
    if (!g_menu_font || !g_menu_font_big) g_menu_font = g_font, g_menu_font_big = g_font_big;
    for (ImFont *font : {g_font, g_font_big, g_menu_font, g_menu_font_big}) ui_merge_language_font(font);
    ui_add_user_font();
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
    for (const QEvent &q : ev) {
        switch (q.kind) {
        case QEvent::KEY: io.AddKeyEvent(q.key, q.down); break;
        case QEvent::ANALOG: io.AddKeyAnalogEvent(q.key, q.value > 0.1f, q.value); break;
        case QEvent::MOUSE_POS: io.AddMousePosEvent(q.x, q.y); break;
        case QEvent::MOUSE_BUTTON: io.AddMouseButtonEvent(q.button, q.down); break;
        case QEvent::WHEEL: io.AddMouseWheelEvent(q.x, q.y); break;
        case QEvent::TEXT: io.AddInputCharactersUTF8(q.text); break;
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
    const char *aspects[] = {"Unlocked", "Original 16:9"};
    opt_choice("Aspect ratio", &g_settings.aspect, aspects, 2, ASPECT_UNLOCKED,
               "Unlocked: the view fills the window, so wider (or taller) windows show more of the island - up to "
               "ultrawide; houses and dungeons keep their framing. Original 16:9: the game's own framing, with bars.");
    bool fs = g_settings.fullscreen;
    if (opt_toggle("Fullscreen", &fs, false, "Fills the screen. Also: F11, Alt+Enter or Cmd+Ctrl+F.")) {
        g_settings.fullscreen = fs;
        g_want_fullscreen_apply.store(true);
    }
    opt_toggle("Show FPS", &g_settings.show_fps, false, "Shows the frame rate in the top right corner.");

    opt_section("Rendering");
    // display order -> RES_* (the ini / enum order predates the menu order)
    // (up to 4x: 7680 x 4320, 8K - more only ran out of GPU memory, and sat next to 75% in the list)
    // (Match display in the middle: left from it 75% / 50% / 25%, right Dynamic then 1x-4x; user 2026-10-11)
    static const char *res[] = {"25%", "50%", "75%", "Match display", "Dynamic", "1x", "2x", "3x", "4x"};
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
        char d[1600];
        int n = snprintf(d, sizeof d, ui_tr("How sharp the 3D image is. Match display: your screen's own resolution. Dynamic: "
                                      "as sharp as your GPU keeps at full speed. 1x-4x: the Switch's 1080p times that%s. "
                                      "75%% / 50%% / 25%%: a share of the display, for slower machines.\n\nRendering at "
                                      "%.0f x %.0f."), ui_tr(g_gpu_integrated ? " (1x: 720p on this GPU)" : ""), 1920.0 * sx, 1080.0 * sy);
        if (g_settings.res_mode == RES_DYNAMIC)
            snprintf(d + n, sizeof d - n, ui_tr(" Dynamic: %.0f%% of the display right now."), output_scale_dynamic_factor() * 100);
        if (opt_choice("Internal resolution", &ri, res, NRES, rdef, d, false))
            g_settings.res_mode = res_mode[ri], g_settings.res_auto = false;
    }
    {   // each OS's own upscaler next to bilinear (UPSCALE_SHARP: MetalFX on macOS, AMD FSR 1 on Windows)
#ifdef __APPLE__
        static const char *ups[] = {"Bilinear", "MetalFX"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. MetalFX: Apple's "
                         "upscaler, sharper. Bilinear: plain and soft.";
#else
        static const char *ups[] = {"Bilinear", "AMD FSR 1"};
        const char *ud = "How the image is scaled up to the window when it renders at a lower resolution. AMD FSR 1: "
                         "edge-aware upscaling and sharpening, sharper. Bilinear: plain and soft.";
#endif
        int ui = g_settings.upscaler == UPSCALE_BILINEAR ? 0 : 1;
        char udm[1600];
        if (g_settings.res_mode == RES_MATCH) {  // (renders at the window's size: nothing to scale up)
            snprintf(udm, sizeof udm, ui_tr("%s\n\nNot used at Match display: the game already renders at your display's resolution."), ui_tr(ud));
            ud = udm;
            opt_disable_next();
        }
        if (opt_choice("Upscaler", &ui, ups, 2, g_default_upscaler == UPSCALE_BILINEAR ? 0 : 1, ud))
            g_settings.upscaler = ui ? UPSCALE_SHARP : UPSCALE_BILINEAR, g_settings.upscaler_auto = false;
    }
    static const char *aa[] = {"Off", "SMAA"};
    opt_choice("Anti-aliasing", &g_settings.antialias, aa, 2, AA_OFF,
               "Smooths jagged edges of the game image before it is scaled to the window. SMAA: finds the edges and "
               "blends only along them, so textures stay sharp. Most useful at a lower internal resolution.");

    opt_section("Game");
    opt_toggle("Remove blur", &g_settings.blur_removal, false,
               "Turns off the tilt-shift blur at the top and bottom of the screen.");
    opt_toggle("Hide Hero Mode icon", &g_settings.hide_hero_icon, false,
               "Hides the Hero Mode emblem in the corner of the screen while playing a Hero Mode file.");
    opt_end();
}

extern "C" bool cosmetics_seed_on(void);  // runtime/cosmetics_seed.c

// Cosmetics: tunic / heart colours, live (runtime/nvn/texpatch.c), and recolours by draw (runtime/nvn/recolour*.c);
// a seed made with "Random cosmetics" fills the rows left on Default (shown as "Seed: <colour>")
static void tab_cosmetics(void) {
    opt_begin("##cosmetics", ImGui::GetFontSize() / 34);
    CosColour *rows[] = {&g_settings.tunic, &g_settings.hearts, &g_settings.hair, &g_settings.skin, &g_settings.shield,
                         &g_settings.leaves, &g_settings.grass, &g_settings.sand};
    if (opt_button("Randomize all", "Every color below gets a random color."))
        for (CosColour *c : rows) c->pick = 1 + rand() % (COL_RANDOM - 1);  // (a named colour: shown as what it is)
    if (opt_button("Default all", "Everything on this page back to the game's own look.")) {
        for (CosColour *c : rows) c->pick = COL_DEFAULT;
        g_settings.enemy_colours = 0;
        g_settings.screen_filter = 0;
    }
    opt_section("Colors");
    opt_colour("Tunic color", &g_settings.tunic, COL_SLOT_TUNIC,
               "Link's tunic color. Default: the game's own (or the randomizer seed's).");
    opt_colour("Heart color", &g_settings.hearts, COL_SLOT_HEARTS, "The color of the hearts on the HUD.");
    opt_colour("Hair color", &g_settings.hair, COL_SLOT_HAIR, "Link's hair color.");
    opt_colour("Body color", &g_settings.skin, COL_SLOT_SKIN, "Link's body color.");  // (larecomp.ini: skin)
    opt_colour("Shield color", &g_settings.shield, COL_SLOT_SHIELD, "The color of Link's shield.");
    opt_section("Overworld");
    {   // Season: fills Leaves, Grass and Sand at once (palette: Orange 2, Gold 3, Pink 9, White 10); "Mixed" when
        // they match none
        static const char *const names[] = {"Spring", "Autumn", "Winter", "Cherry blossom", "Mixed"};
        static const int preset[4][3] = {{COL_DEFAULT, COL_DEFAULT, COL_DEFAULT}, {2, 3, 3}, {10, 10, 10},
                                         {9, COL_DEFAULT, COL_DEFAULT}};
        int cur = 4;  // (as shown: a row on Default shows the seed's colour, if it has its own)
        for (int i = 0; i < 4; i++)
            if (col_pick(&g_settings.leaves, COL_SLOT_LEAVES) == preset[i][0] &&
                col_pick(&g_settings.grass, COL_SLOT_GRASS) == preset[i][1] &&
                col_pick(&g_settings.sand, COL_SLOT_SAND) == preset[i][2])
                cur = i;
        int v = cur;
        if (opt_choice("Season", &v, names, 5, 0, "Sets the leaves, grass and sand colors below.") && v != cur) {
            if (v == 4) v = cur == 0 ? 3 : 0;  // (Mixed is never chosen: past the ends, back round)
            g_settings.leaves.pick = preset[v][0];
            g_settings.grass.pick = preset[v][1];
            g_settings.sand.pick = preset[v][2];
        }
    }
    opt_colour("Leaves color", &g_settings.leaves, COL_SLOT_LEAVES, "The overworld's tree leaves and bushes.");
    opt_colour("Grass color", &g_settings.grass, COL_SLOT_GRASS, "The overworld's grass.");
    opt_colour("Sand color", &g_settings.sand, COL_SLOT_SAND, "The beaches, the desert and the sandy south.");
    opt_section("Enemies");
    {   // (Off in a seed with its own cosmetics: "Seed", its enemies are per seed)
        const char *modes[N_ENEMY_COLOUR_MODES];
        for (int i = 0; i < N_ENEMY_COLOUR_MODES; i++) modes[i] = g_enemy_colour_modes[i];
        if (cosmetics_seed_on()) modes[0] = "Seed";
        opt_choice("Enemy colors", &g_settings.enemy_colours, modes, N_ENEMY_COLOUR_MODES, 0,
                   "Each kind of enemy in another color. Enemies whose color shows their strength or type keep theirs.");
    }
    opt_section("Screen");
    opt_choice("Screen filter", &g_settings.screen_filter, g_screen_filters, N_SCREEN_FILTERS, 0,
               "A filter over the whole picture, the HUD included.");
    opt_end();
}

static void tab_audio(void) {
    opt_begin("##audio", ImGui::GetFontSize() / 34);
    opt_section("Sound");
    float pct = roundf(g_settings.volume * 100);
    if (opt_slider("Volume", &pct, 0, 200, 5, 100, "%g%%",
                   "The game's volume. 100% is the game's own level; above it, louder."))
        g_settings.volume = pct / 100.0f;
    opt_toggle("Room reverb", &g_settings.reverb, true,
               "The game's echo in caves, dungeons and big rooms.");
    opt_toggle("Movie audio", &g_settings.movie_audio, true,
               "Sound in the game's movies (the opening and the ending). Applies the next time a movie starts.");
    opt_end();
}

// Quit: back to the start screen (in game: the game reboots to it) or out to the desktop. Each asks first (progress
// since the last save is lost); B / Esc or Cancel backs out.
extern "C" void rt_request_reboot(int kind);  // runtime/main.c (RT_REBOOT_TITLE = 2: the start screen)
static void tab_quit(float scale, bool in_game) {
    static int ask;  // 0: the two choices; 1: start screen?; 2: desktop?
    if (ask && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        ask = 0;
    const char *lost = in_game ? ui_tr("\n\nProgress since your last save is lost.") : "";
    char d1[600], d2[600];
    snprintf(d1, sizeof d1, ui_tr("Leaves the game and goes back to the Multi start screen.%s"), lost);
    snprintf(d2, sizeof d2, ui_tr("Closes Multi.%s"), lost);
    opt_begin("##quit", scale);
    opt_section("Quit");
    if (!ask) {
        if (!in_game) opt_disable_next();  // (already on it)
        if (opt_action("Quit to Start Screen", "", in_game ? d1 : "You're on the start screen.")) ask = 1;
        if (opt_action("Quit to Desktop", "", d2)) ask = 2;
    } else {
        if (opt_action(ask == 1 ? "Yes, quit to Start Screen" : "Yes, quit to Desktop", "", ask == 1 ? d1 : d2)) {
            settings_save();
            if (ask == 1) {
                rt_request_reboot(2);
                set_open(false);
            } else {
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
            }
            ask = 0;
        }
        if (opt_action("Cancel", "", "Stays where you are.")) ask = 0;
    }
    opt_end();
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

// ---- reusing the overlay's last drawing (LARECOMP_OVERLAY_CACHE=0: off, a new ImGui frame every present as before)
// While only the FPS counter and / or the run timer are up (no menu, no input, nothing interactive or animated), the
// overlay draws the same thing frame after frame: ImGui's NewFrame + layout + Render then run for nothing on the
// render thread. Their drawing is described by a key (the text shown, the window size); while it stays the same, the
// last frame's draw data (ImGui keeps it until the next NewFrame) is drawn again. Only after three frames in a row
// built with that key: an auto-sized window (the FPS counter) settles over the frames after its text changes.
// The FPS counter then shows the same average as ImGui's io.Framerate (the last 60 frame times), sampled twice a
// second instead of every frame.
static int g_ov_cache = -1;
static char g_ov_key[96];
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
    ImGui::TextColored(plastic_col(plastic_theme().title), "%s", ui_tr("Enter your name"));
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
        ImGui::TextDisabled(ui_tr("Enter  confirm      Esc  cancel      (%d / %d)"), units, g_kbd_max);
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
// false: nothing of the game is loaded yet - the tabs that need it are greyed out; prelaunch: the start screen's own
// first tab, game files and fonts, drawn by launcher.mm).
static void tabs_body(float scale, bool in_game, void (*prelaunch)(float)) {
    // (id: the page; -1 = Prelaunch)
    struct Tab { const char *name; int id; bool on; };
    Tab tabs[15];
    int ntabs = 0;
    if (prelaunch) tabs[ntabs++] = {"Prelaunch", -1, true};
    static const char *names[] = {"Video", "Audio", "Saves", "Randomizer", "Warp", "Cheats", "Gameplay",
                                  "Cosmetics", "Controls", "Quit", "Photo mode", "Mods", "Workshop", "Performance"};
    static const int order[] = {8, 0, 1, 6, 7, 10, 11, 12, 13, 3, 5, 4, 2, 9};  // existing IDs stay stable for test hooks
                                                                // Cheats, Warp, Save Editor, Quit (ids stay: test hooks)
    for (int i : order)  // (Quit: in game only - the start screen has its own QUIT, and an eleventh tab cut names short)
        if (((title_is_las() && (in_game || game_profile_las_extensions(game_profile_active()))) || i == 8 || i == 0 || i == 1 || i == 9 || i == 13) && (i != 9 || !prelaunch)) tabs[ntabs++] = {names[i], i, in_game || (i != 4 && i != 5)};
    int cur = 0;  // g_tab / g_tab_request hold ids
    for (int i = 0; i < ntabs; i++) if (tabs[i].id == g_tab) cur = i;
    if (!tabs[cur].on) cur = 0;
    // L/R (or Q/E) switch tabs, skipping greyed-out ones (not while typing: Q / E are letters then, and the on-screen
    // keyboard has the controller)
    if (g_tab_request_ext >= 0) { g_tab_request = g_tab_request_ext; g_tab_request_ext = -1; }
    bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();
    static int focused_tab = -99;  // the tab the controller's cursor is on (from the frame before; -99: none)
    bool switching = false;  // (L / R this frame: the page changes next frame)
    if (!any_popup && !typing) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step) {
            int i = cur;
            do i = (i + step + ntabs) % ntabs; while (!tabs[i].on);
            g_tab_request = tabs[i].id;
            // the cursor goes onto the new tab (left behind on the tab bar, A on it switched straight back - user
            // 2026-10-11; left in the old page, it stayed on a row that is gone, and Down went on from where that row
            // had been)
            g_focus_tab = tabs[i].id, g_focus_frames = 0;
            switching = true;
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
    {
        const PlasticTheme &pt = plastic_theme();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float gap = 8 * scale, pad = 20 * scale, th = ImGui::GetFontSize() + 20 * scale;
        float tw[16], text_w[16], total = 0, avail = ImGui::GetContentRegionAvail().x - 4 * scale;
        for (int k = 0; k < ntabs; k++) text_w[k] = ImGui::CalcTextSize(ui_tr(tabs[k].name)).x, total += text_w[k] + (k ? gap : 0);
        if (total + 2 * pad * ntabs > avail) pad = fmaxf((avail - total) / (2 * ntabs), 3 * scale);  // (a narrow window: tighter)
        for (int k = 0; k < ntabs; k++) tw[k] = text_w[k] + 2 * pad;
        float used_width = 0;
        if (g_focus_tab == -1 && !prelaunch) g_focus_tab = 8;  // (no Prelaunch in game: Controls, the first tab)
        for (int k = 0; k < ntabs; k++) {
            const Tab &t = tabs[k];
            ImGui::BeginDisabled(!t.on);  // (out of the game: Save Editor, Warp, Cheats)
            if (k && used_width + gap + tw[k] <= avail) { ImGui::SameLine(0, gap); used_width += gap; }
            else used_width = 0;
            used_width += tw[k];
            ImVec2 p = ImGui::GetCursorScreenPos();
            p.x += 2 * scale;
            ImGui::SetCursorScreenPos(p);
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the piece shows the cursor)
            ImGui::PushID(t.id + 2);
            bool clicked = ImGui::InvisibleButton("##tab", ImVec2(tw[k], th),
                                                  ImGuiButtonFlags_EnableNav | ImGuiButtonFlags_PressedOnClick);
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
            plastic_add(dl, ImVec2(p.x, p.y + (sel ? 0 : 2 * scale)), ImVec2(p.x + tw[k], p.y + th - 2 * scale), 10 * scale, col,
                        PK_PILL, (sel ? 7 : 4) * scale, sel ? 1.0f : 0.55f);
            ImU32 tc = sel ? pt.text_sel : t.on ? pt.text : pt.text_dim;
            ImVec2 ts(text_w[k], ImGui::GetFontSize());
            dl->AddText(ImVec2(p.x + (tw[k] - ts.x) * 0.5f, p.y + (th - ts.y) * 0.5f - (sel ? 2 : 1) * scale), tc, ui_tr(t.name));
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
        case 2: save_backups_draw(scale, in_game); break;
        case 3: rando_tab_draw(scale); break;
        case 4: if (warp_tab_draw(scale)) set_open(false); break;
        case 5: cheats_draw(scale); break;
        case 6: difficulty_draw(scale); break;
        case 7: tab_cosmetics(); break;
        case 8: controls_tab_draw(scale); break;
        case 9: tab_quit(scale, in_game); break;
        case 10: photo_mode_tab(scale, in_game); break;
        case 11: mods_draw(scale, in_game); break;
        case 12: workshop_draw(scale, in_game); break;
        case 13: performance_draw(scale, in_game); break;
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

// Whether menu_render draws anything: the menu, the keyboard, an overlay (FPS, inventory, tracker, chaos, race, a
// pickup card).
static bool render_needed(bool open, bool kbd, bool inv, bool trk, bool trkd, bool chaos, bool race, bool flyin) {
    return open || kbd || g_settings.show_fps || inv || trk || trkd || chaos || race || flyin || photo_mode_active() || workshop_overlay_wanted();
}

#ifdef __APPLE__
void menu_render(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> target) {
    const double tw = target.width, th = target.height;
#else
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h) {
    const double tw = target_w, th = target_h;
#endif
    if (photo_mode_suppress_overlay()) {
        pthread_mutex_lock(&q_lock); q_events.clear(); pthread_mutex_unlock(&q_lock); return;
    }
    bool open = g_open.load();
    bool inv = title_is_las() && !open && inventory_overlay_wanted();
    bool trk = title_is_las() && !open && tracker_overlay_wanted();
    bool trkd = title_is_las() && !open && tracker_dungeon_wanted();
    bool chaos = title_is_las() && !open && chaos_overlay_wanted();
    bool kbd = g_kbd.load() == 1;
    bool race = title_is_las() && race_overlay_wanted();  // (run timer, race seed hash: race_overlay.mm)
    bool flyin = title_is_las() && pickup_flyin_wanted() && !open;  // (a silent pickup's card)
#ifdef __APPLE__
    plastic_perf_note(cb, open);  // (LARECOMP_UI_PERF test only)
#else
    plastic_perf_note_vk(open);
#endif
    if (!render_needed(open, kbd, inv, trk, trkd, chaos, race, flyin)) {
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
    double now = SDL_GetTicks() / 1000.0;
    io.DeltaTime = last > 0 && now > last ? (float)(now - last) : 1.0f / 60;
    last = now;
    if (g_ov_cache < 0) g_ov_cache = !getenv("LARECOMP_OVERLAY_CACHE") || atoi(getenv("LARECOMP_OVERLAY_CACHE")) != 0;
    char key[sizeof g_ov_key];
    key[0] = 0;
    if (g_ov_cache) {
        fps_note(now, io.DeltaTime);
        char rk[64];
        // (only the FPS counter and the run timer; input only reaches ImGui while the menu or the name entry is up)
        if (!open && !kbd && !inv && !trk && !trkd && !chaos && !flyin && race_overlay_key(rk, sizeof rk)) {
            pthread_mutex_lock(&q_lock);
            bool events = !q_events.empty();
            pthread_mutex_unlock(&q_lock);
            if (!events) snprintf(key, sizeof key, "%.0fx%.0f fps %d %.0f | %s", tw, th, (int)g_settings.show_fps,
                                  g_settings.show_fps ? g_fps_shown : 0.0, rk);
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
#ifdef __APPLE__
    ImGui_ImplMetal_NewFrame(rp);
#else
    ImGui_ImplVulkan_NewFrame();
#endif
    ImGui::NewFrame();

    if (g_settings.show_fps && !photo_mode_active()) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16 * scale, 16 * scale), 0, ImVec2(1, 0));
        ImGui::SetNextWindowBgAlpha(0.5f);
        ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::Text("%.0f fps", g_ov_cache ? g_fps_shown : io.Framerate);
        ImGui::End();
    }

    if (race) race_overlay_draw(io.DisplaySize, g_menu_font_big, scale);
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
    if (open) {
        // dim the game behind the menu
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                                                      IM_COL32(0, 0, 0, (int)(plastic_theme().dim * 255)));
        float w = fminf(io.DisplaySize.x * 0.9f, 1500 * scale), h = io.DisplaySize.y - 2 * 18 * scale;  // (all the height: a slim margin)
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImFont *keep_big = g_font_big;  // (the window in the menu's font; header() uses g_font_big)
        g_font_big = g_menu_font_big;
        ImGui::PushFont(g_menu_font);
        // header as on the start screen's Settings: the title, and a close X in the corner (B / Esc too). ("Title
        // screen" - rt_request_reboot(RT_REBOOT_TITLE) - is to find a new home; user 2026-10-11.)
        ImGui::PushFont(g_font_big);
        ImGui::TextColored(plastic_col(plastic_theme().title), "%s", ui_tr("Settings"));
        ImGui::PopFont();
        {
            float bx = 44 * scale;
            if (plastic_close_corner(bx, scale)) set_open(false);
        }

        bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), typing = ui_text_busy();  // (before the
        tabs_body(scale, true, NULL);                                                                          //  pages run)
        // B with nothing focused-deeper closes the menu (not the Esc / B that ends typing: typing is from the frame
        // before, so it's still true the frame a field lets go)
        if (!any_popup && !typing && !ui_text_busy() && !controls_ui_took_back() && !rando_tab_took_back() &&
            !save_editor_took_back() && !warp_tab_took_back() &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            set_open(false);
        ImGui::PopFont();
        g_font_big = keep_big;
        ImGui::End();
        osk_draw(g_font_big, scale);  // (a text field activated with the controller)
    }

    if (photo_mode_active()) photo_mode_draw(scale);
    if (!open && !photo_mode_active() && workshop_overlay_wanted()) workshop_overlay(scale);
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
