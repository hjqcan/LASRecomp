#include "../photo.h"
#include "../gameversion.h"
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
#include <algorithm>
#include <fstream>
#include <locale>
#ifdef _WIN32
#include <windows.h>
#endif
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
static double frame_rect[4];
static bool guides = true;
static int captured_width, captured_height;

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
        if (const char *a = getenv("LARECOMP_PHOTO_ASPECT")) state.aspect = atoi(a);
        if (const char *v = getenv("LARECOMP_PHOTO_VIGNETTE")) state.vignette = (float)atof(v);
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
    double rect[4] = {*x, *y, *w, *h};
    photo_state_frame(&state, crop, rect);
    *x = rect[0]; *y = rect[1]; *w = rect[2]; *h = rect[3];
    frame_rect[0] = *x/width; frame_rect[1] = *y/height;
    frame_rect[2] = *w/width; frame_rect[3] = *h/height;
}
extern "C" void photo_mode_color(float color[4]) {
    photo_state_color(&state, color);
}
extern "C" bool photo_mode_capture_rect(unsigned width, unsigned height, int rect[4]) {
    if (!capture_drawing || !width || !height) return false;
    rect[0] = std::clamp((int)lround(frame_rect[0]*width), 0, (int)width-1);
    rect[1] = std::clamp((int)lround(frame_rect[1]*height), 0, (int)height-1);
    rect[2] = std::clamp((int)lround(frame_rect[2]*width), 1, (int)width-rect[0]);
    rect[3] = std::clamp((int)lround(frame_rect[3]*height), 1, (int)height-rect[1]);
    captured_width = rect[2]; captured_height = rect[3];
    return true;
}
extern "C" const char *photo_mode_capture_path(void) {
    capture_drawing = false;
    if (!capture_pending || !state.frozen) return NULL;
    capture_pending = false;
    try {
        namespace fs = std::filesystem;
        fs::path directory = "photos";
        fs::path part;
        for (const auto &c : fs::absolute(directory)) {
            part /= c;
            if (fs::is_symlink(fs::symlink_status(part))) throw std::runtime_error("Photo paths cannot contain links.");
#ifdef _WIN32
            auto attr = GetFileAttributesW(part.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Photo paths cannot contain reparse points.");
#endif
        }
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
            if (!fs::exists(fs::symlink_status(p)) && !fs::exists(fs::symlink_status(fs::u8path(p.u8string()+".ini")))) {
                capture_path = p.u8string(); capture_drawing = true; return capture_path.c_str();
            }
        }
        throw std::runtime_error("Cannot choose a new photo filename.");
    } catch (const std::exception &e) { message = e.what(); }
    return NULL;
}
extern "C" void photo_mode_capture_done(void) {
    std::error_code ec;
    auto size = std::filesystem::file_size(std::filesystem::u8path(capture_path), ec);
    message = !ec && size > 0 ? std::string(ui_tr("Photo saved:")) + " " + capture_path : ui_tr("Could not save photo.");
    if (!ec && size > 0) {
        // A small portable recipe accompanies each PNG; no game resources or save data.
        auto recipe = std::filesystem::u8path(capture_path + ".ini");
        if (!std::filesystem::exists(std::filesystem::symlink_status(recipe, ec))) {
            std::ofstream out(recipe, std::ios::binary);
            out.imbue(std::locale::classic());
            out << "format=1\ngame_version=" << game_version_name(game_version_current())
                << "\nwidth=" << captured_width << "\nheight=" << captured_height
                << "\naspect=" << state.aspect << "\nzoom=" << state.zoom << "\npan_x=" << state.pan_x
                << "\npan_y=" << state.pan_y << "\nexposure=" << state.exposure << "\nsaturation=" << state.saturation
                << "\ncontrast=" << state.contrast << "\nvignette=" << state.vignette << "\nhide_hud=" << state.hide_hud << '\n';
            out.close();
            if (!out) message += std::string(" ") + ui_tr("The photo recipe could not be saved.");
        }
    }
    fprintf(stderr, "[photo] %s\n", message.c_str());
    capture_drawing = false;
}
extern "C" bool photo_mode_suppress_overlay(void) { return capture_drawing || (state.active && !controls); }
extern "C" void photo_mode_draw(float scale) {
    if (!state.active || photo_mode_suppress_overlay()) return;
    auto &io = ImGui::GetIO();
    if (guides) {
        auto *draw = ImGui::GetBackgroundDrawList();
        ImVec2 a((float)frame_rect[0]*io.DisplaySize.x, (float)frame_rect[1]*io.DisplaySize.y);
        ImVec2 b(a.x+(float)frame_rect[2]*io.DisplaySize.x, a.y+(float)frame_rect[3]*io.DisplaySize.y);
        draw->AddRect(a, b, IM_COL32(255,255,255,150));
        for (int i = 1; i <= 2; ++i) {
            float x = a.x+(b.x-a.x)*i/3, y = a.y+(b.y-a.y)*i/3;
            draw->AddLine(ImVec2(x,a.y), ImVec2(x,b.y), IM_COL32(255,255,255,110));
            draw->AddLine(ImVec2(a.x,y), ImVec2(b.x,y), IM_COL32(255,255,255,110));
        }
    }
    ImGui::SetNextWindowPos(ImVec2(20 * scale, 20 * scale), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(fminf(520 * scale, io.DisplaySize.x - 40 * scale), 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0,0), ImVec2(FLT_MAX, io.DisplaySize.y-40*scale));
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
    const char *aspects[] = {"Original", "16:9", "4:3", "1:1", "4:5", "2.39:1"};
    state.aspect = std::clamp(state.aspect, 0, 5);
    if (ImGui::BeginCombo(ui_label("Photo aspect ratio").c_str(), ui_tr(aspects[state.aspect]))) {
        for (int i = 0; i < 6; ++i) if (ImGui::Selectable(ui_tr(aspects[i]), state.aspect == i)) state.aspect = i;
        ImGui::EndCombo();
    }
    ImGui::SliderFloat(ui_label("Zoom").c_str(), &state.zoom, 1, 4, "%.2fx");
    ImGui::SliderFloat(ui_label("Horizontal framing").c_str(), &state.pan_x, -1, 1, "%.2f");
    ImGui::SliderFloat(ui_label("Vertical framing").c_str(), &state.pan_y, -1, 1, "%.2f");
    ImGui::SliderFloat(ui_label("Exposure").c_str(), &state.exposure, -2, 2, "%+.1f EV");
    ImGui::SliderFloat(ui_label("Saturation").c_str(), &state.saturation, 0, 2, "%.2f");
    ImGui::SliderFloat(ui_label("Contrast").c_str(), &state.contrast, -0.5f, 1, "%+.2f");
    ImGui::SliderFloat(ui_label("Vignette").c_str(), &state.vignette, 0, 1, "%.2f");
    ImGui::PopItemWidth();
    if (ImGui::Button(ui_label("Monochrome").c_str())) { state.saturation = 0; state.contrast = .2f; state.exposure = 0; state.vignette = .2f; }
    ImGui::SameLine();
    if (ImGui::Button(ui_label("Vivid").c_str())) { state.saturation = 1.3f; state.contrast = .15f; state.exposure = .1f; state.vignette = 0; }
    ImGui::Checkbox(ui_label("Composition guides").c_str(), &guides);
    ImGui::TextWrapped("%s", ui_tr("Photos are cropped to the selected frame. Guides are excluded; a settings recipe is saved beside each PNG."));
    if (ImGui::Checkbox(ui_label("Hide game interface").c_str(), &state.hide_hud))
        photo_state_refresh(&state, renderer_present_count());
    if (ImGui::Button(ui_label("Reset framing and color").c_str())) {
        state.zoom = state.saturation = 1; state.pan_x = state.pan_y = state.exposure = 0;
        state.contrast = state.vignette = 0; state.aspect = 0;
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
