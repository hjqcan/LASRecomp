#include "../photo.h"
#include "menu.h"
#include "localization.h"
#include "imgui.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <string>
extern "C" {
#include "../rt.h"
int game_in_play(void);
void input_reset(void);
uint64_t renderer_present_count(void);
}
static PhotoState state;
static std::atomic<unsigned> requests;
static std::atomic<bool> active;
enum { TOGGLE = 1, EXIT = 2, CAPTURE = 4, CONTROLS = 8 };
static bool controls = true, capture_pending, capture_drawing, focus;
static std::string capture_path, message;
static float base_crop[4];
static double base_rect[4];

extern "C" bool photo_mode_active(void) { return active.load(); }
extern "C" bool photo_mode_available(void) {
    return game_in_play() != 0;
}
extern "C" void photo_mode_toggle(void) { requests.fetch_or(TOGGLE); }
extern "C" void photo_mode_exit(void) { requests.fetch_or(EXIT); }
extern "C" void photo_mode_capture(void) { requests.fetch_or(CAPTURE); }
extern "C" void photo_mode_toggle_controls(void) { requests.fetch_or(CONTROLS); }
extern "C" void photo_mode_frame(uint64_t frame) {
    unsigned r = requests.exchange(0);
    const char *test = getenv("LARECOMP_PHOTO_AT");
    if (test && frame + 1 == strtoull(test, NULL, 10)) r |= TOGGLE;
    test = getenv("LARECOMP_PHOTO_SHOT_AT");
    for (const char *p = test; p && *p;) {
        char *end; uint64_t at = strtoull(p, &end, 10);
        if (end == p) break;
        if (frame + 1 == at) r |= CAPTURE;
        p = *end == ',' ? end + 1 : NULL;
    }
    test = getenv("LARECOMP_PHOTO_EXIT_AT");
    if (test && frame + 1 == strtoull(test, NULL, 10)) r |= EXIT;
    if ((r & EXIT) || ((r & TOGGLE) && state.active)) {
        photo_state_reset(&state); active = false; capture_pending = false; capture_drawing = false;
        capture_path.clear(); input_reset();
        fprintf(stderr, "[photo] resumed at frame %llu\n", (unsigned long long)frame);
        return;
    }
    if ((r & TOGGLE) && !state.active && photo_mode_available()) {
        photo_state_enter(&state, frame); active = true; controls = true; focus = true;
        message.clear(); capture_path.clear(); input_reset();
        if (const char *z = getenv("LARECOMP_PHOTO_ZOOM")) state.zoom = (float)atof(z);
        fprintf(stderr, "[photo] entered at frame %llu\n", (unsigned long long)frame);
    }
    if (state.active && (r & CAPTURE)) capture_pending = true;
    if (state.active && (r & CONTROLS)) controls = !controls;
}
extern "C" void photo_mode_presented(uint64_t frame) {
    bool before = state.frozen;
    photo_state_presented(&state, frame);
    if (!before && state.frozen) {
        fprintf(stderr, "[photo] frozen at frame %llu\n", (unsigned long long)frame);
        if (state.hide_hud && state.clean_frame != frame - 1) message = ui_tr("Interface hiding is unavailable in this scene.");
    }
}
extern "C" bool photo_mode_hold(void) { return state.active && state.frozen; }
extern "C" bool photo_mode_skip_draw(uint64_t frame, bool screen_target, bool composite) {
    return photo_state_skip_draw(&state, frame, screen_target, composite);
}
extern "C" void photo_mode_compose(float crop[4], double width, double height,
                                     double *x, double *y, double *w, double *h) {
    if (!state.active) return;
    if (!state.frozen) {
        memcpy(base_crop, crop, sizeof base_crop);
        base_rect[0] = *x / width; base_rect[1] = *y / height;
        base_rect[2] = *w / width; base_rect[3] = *h / height;
    } else {
        memcpy(crop, base_crop, sizeof base_crop);
        *x = base_rect[0] * width; *y = base_rect[1] * height;
        *w = base_rect[2] * width; *h = base_rect[3] * height;
    }
    photo_state_crop(&state, crop);
}
extern "C" void photo_mode_color(float color[4]) {
    color[0] = state.active ? exp2f(state.exposure) : 1;
    color[1] = state.active ? state.saturation : 1;
    color[2] = color[3] = 0;
}
extern "C" const char *photo_mode_capture_path(void) {
    capture_drawing = false;
    if (!capture_pending || !state.frozen) return NULL;
    capture_pending = false;
    try {
        namespace fs = std::filesystem;
        fs::path directory = "photos";
        if (fs::is_symlink(fs::symlink_status(directory))) throw std::runtime_error("Photo directory cannot be a link.");
        fs::create_directories(directory);
        std::time_t now = std::time(NULL); std::tm tm;
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char stamp[32]; std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
        for (unsigned i = 0; i < 10000; ++i) {
            char name[80]; snprintf(name, sizeof name, "photo-%s-%04u.png", stamp, i);
            fs::path p = directory / name;
            if (!fs::exists(p)) { capture_path = p.u8string(); capture_drawing = true; return capture_path.c_str(); }
        }
        throw std::runtime_error("Cannot choose a new photo filename.");
    } catch (const std::exception &e) { message = e.what(); }
    return NULL;
}
extern "C" void photo_mode_capture_done(void) {
    std::error_code ec;
    auto size = std::filesystem::file_size(std::filesystem::u8path(capture_path), ec);
    message = !ec && size > 0 ? std::string(ui_tr("Photo saved:")) + " " + capture_path : ui_tr("Could not save photo.");
    fprintf(stderr, "[photo] %s\n", message.c_str());
    capture_drawing = false;
}
extern "C" bool photo_mode_suppress_overlay(void) { return capture_drawing || (state.active && !controls); }
extern "C" void photo_mode_draw(float scale) {
    if (!state.active || photo_mode_suppress_overlay()) return;
    auto &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(20 * scale, 20 * scale), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(fminf(520 * scale, io.DisplaySize.x - 40 * scale), 0));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##photo", NULL, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
    ImGui::TextColored(ImVec4(.93f,.76f,.33f,1), "%s", ui_tr("Photo mode"));
    ImGui::TextWrapped("%s", ui_tr("F2 / Esc: resume   F12: save photo   Tab: hide controls"));
    ImGui::TextWrapped("%s", ui_tr("The scene is frozen. Framing changes the captured view; photos use the window resolution."));
    if (focus) { ImGui::SetKeyboardFocusHere(); focus = false; }
    ImGui::BeginDisabled(!state.frozen);
    if (ImGui::Button(ui_label("Save photo").c_str())) photo_mode_capture();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ui_label("Resume game").c_str())) photo_mode_exit();
    ImGui::Separator();
    ImGui::PushItemWidth(220 * scale);
    ImGui::SliderFloat(ui_label("Zoom").c_str(), &state.zoom, 1, 4, "%.2fx");
    ImGui::SliderFloat(ui_label("Horizontal framing").c_str(), &state.pan_x, -1, 1, "%.2f");
    ImGui::SliderFloat(ui_label("Vertical framing").c_str(), &state.pan_y, -1, 1, "%.2f");
    ImGui::SliderFloat(ui_label("Exposure").c_str(), &state.exposure, -2, 2, "%+.1f EV");
    ImGui::SliderFloat(ui_label("Saturation").c_str(), &state.saturation, 0, 2, "%.2f");
    ImGui::PopItemWidth();
    if (ImGui::Checkbox(ui_label("Hide game interface").c_str(), &state.hide_hud))
        photo_state_refresh(&state, renderer_present_count());
    if (ImGui::Button(ui_label("Reset framing and color").c_str())) {
        state.zoom = state.saturation = 1; state.pan_x = state.pan_y = state.exposure = 0;
    }
    if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
    ImGui::End();
}
extern "C" void photo_mode_tab(float, bool in_game) {
    ImGui::TextWrapped("%s", ui_tr("Freeze a scene, hide the game interface, adjust framing and color, and save a PNG in the photos folder."));
    ImGui::TextWrapped("%s", ui_tr("Enter photo mode with F2 during gameplay. Saved photos exclude the photo controls and FPS counter."));
    ImGui::BeginDisabled(!in_game || !photo_mode_available());
    if (ImGui::Button(ui_label("Enter photo mode").c_str())) { menu_close(); photo_mode_toggle(); }
    ImGui::EndDisabled();
}
