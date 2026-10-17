// Photo controls run on the presentation thread, which continues servicing input while the scene is held.
// Requests from the event thread are atomic; framing edits, capture paths and helper handles stay here.
#include "../photo.h"
#include "../paths.h"
#include "../input_bind.h"
#include "../input_prompt.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <vector>
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
static std::atomic<bool> framing;
enum { TOGGLE = 1, EXIT = 2, CAPTURE = 4, CONTROLS = 8, FRAME_CANCEL = 16, FRAME_TAKE = 32 };
static bool controls = true, capture_pending, capture_drawing, focus;
static std::string capture_path, saved_path, message;
static PlatProc reveal_process;
static bool frame_focus, return_focus, dragging;
static float framing_fade;
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
extern "C" bool photo_mode_pad_button(int host, bool down) {
    // The panel keeps ordinary menu navigation. Once framing, its actions follow the same bindings as the legend.
    // Track edges on the event thread so the button that enters framing cannot immediately take a photo.
    static uint32_t held;
    if (host < 0 || host >= PADB_HOST_COUNT) return false;
    uint32_t bit = 1u << host;
    bool pressed = down && !(held & bit);
    if (down) held |= bit; else held &= ~bit;
    if (!active.load() || !framing.load()) return false;
    if (pressed) {
        if (host == bind_prompt_host(BIND_B)) requests.fetch_or(FRAME_CANCEL);
        else if (host == bind_prompt_host(BIND_A)) requests.fetch_or(FRAME_TAKE);
    }
    return true;
}
extern "C" void photo_mode_frame(uint64_t frame) {
    unsigned r = requests.exchange(0);
    if (reveal_process && plat_proc_wait(reveal_process, false) >= 0) reveal_process = 0;
    static const uint64_t test_at = [] { const char *p = getenv("LARECOMP_PHOTO_AT"); return p ? strtoull(p, NULL, 10) : UINT64_MAX; }();
    static const uint64_t test_exit = [] { const char *p = getenv("LARECOMP_PHOTO_EXIT_AT"); return p ? strtoull(p, NULL, 10) : UINT64_MAX; }();
    static const std::vector<uint64_t> test_shots = [] {
        std::vector<uint64_t> shots;
        for (const char *p = getenv("LARECOMP_PHOTO_SHOT_AT"); p && *p;) {
            char *end; uint64_t at = strtoull(p, &end, 10);
            if (end == p) break;
            shots.push_back(at); p = *end == ',' ? end + 1 : NULL;
        }
        return shots;
    }();
    if (frame + 1 == test_at) r |= TOGGLE;
    for (auto at : test_shots) if (frame + 1 == at) r |= CAPTURE;
    if (frame + 1 == test_exit) r |= EXIT;
    if ((r & EXIT) || ((r & TOGGLE) && state.active)) {
        photo_state_reset(&state); active = framing = false; capture_pending = false; capture_drawing = false;
        capture_path.clear(); input_reset();
        fprintf(stderr, "[photo] resumed at frame %llu\n", (unsigned long long)frame);
        return;
    }
    if ((r & TOGGLE) && !state.active && photo_mode_available()) {
        photo_state_enter(&state, frame); active = true; controls = true; focus = true;
        message.clear(); capture_path.clear(); saved_path.clear(); input_reset();
        dragging = frame_focus = return_focus = false; framing = false; framing_fade = 0;
        static const float test_zoom = [] { const char *p = getenv("LARECOMP_PHOTO_ZOOM"); return p ? (float)atof(p) : 1.0f; }();
        state.zoom = test_zoom;
        fprintf(stderr, "[photo] entered at frame %llu\n", (unsigned long long)frame);
    }
    if (state.active && (r & CAPTURE)) capture_pending = true;
    if (state.framing && (r & (FRAME_CANCEL | FRAME_TAKE))) {
        bool take = !(r & FRAME_CANCEL);
        photo_state_end_framing(&state, take); framing = false;
        if (take) capture_pending = true;
        controls = return_focus = true;
    }
    if (state.active && !state.framing && (r & CONTROLS)) controls = !controls;
}
extern "C" void photo_mode_presented(uint64_t frame) {
    bool before = state.frozen;
    photo_state_presented(&state, frame);
    if (!before && state.frozen) {
        fprintf(stderr, "[photo] frozen at frame %llu\n", (unsigned long long)frame);
        if (state.hide_hud && state.clean_frame != frame - 1) message = "Interface hiding is unavailable in this scene.";
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
        fs::path directory = fs::u8path(rt_data_root()) / "photos";
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
    } catch (const std::exception &e) { message = "Could not save photo."; fprintf(stderr, "[photo] %s\n", e.what()); }
    return NULL;
}
extern "C" void photo_mode_capture_done(void) {
    std::error_code ec;
    auto size = std::filesystem::file_size(std::filesystem::u8path(capture_path), ec);
    if (!ec && size > 0) {
        saved_path = capture_path;
        message = "Photo saved: " + std::filesystem::u8path(saved_path).filename().u8string();
    } else message = "Could not save photo.";
    fprintf(stderr, "[photo] %s\n", message.c_str());
    capture_drawing = false;
}
extern "C" bool photo_mode_suppress_overlay(void) { return capture_drawing; }

#ifdef __APPLE__
static const char *reveal_label = "Show in Finder";
#elif defined(_WIN32)
static const char *reveal_label = "Show in Explorer";
#else
static const char *reveal_label = "Open folder";
#endif
static void reveal_photo(void) {
    if (saved_path.empty() || reveal_process) return;
    // Pass the path as an argument, never through a shell. The helper can finish without blocking presentation.
#ifdef __APPLE__
    const char *argv[] = {"/usr/bin/open", "-R", saved_path.c_str(), NULL};
#elif defined(_WIN32)
    const char *system = getenv("SystemRoot");
    std::string explorer = std::string(system ? system : "C:\\Windows") + "\\explorer.exe";
    std::string selected = std::filesystem::u8path(saved_path).make_preferred().u8string();
    const char *argv[] = {explorer.c_str(), "/select,", selected.c_str(), NULL};
#else
    std::string directory = std::filesystem::u8path(saved_path).parent_path().u8string();
    const char *argv[] = {"/usr/bin/xdg-open", directory.c_str(), NULL};
#endif
    reveal_process = plat_spawn(argv, NULL, false);
    if (!reveal_process) message = "Could not open the photos folder.";
}
static float stick(ImGuiKey positive, ImGuiKey negative) {
    return ImGui::GetKeyData(positive)->AnalogValue - ImGui::GetKeyData(negative)->AnalogValue;
}
static void button_prompt(int slot, const char *action) {
    InputPrompt prompt = input_button_prompt(slot);
    if (prompt.symbol == INPUT_PROMPT_TEXT) ImGui::TextUnformatted(prompt.text);
    else {
        float size = ImGui::GetFontSize(), inset = size * 0.15f, thickness = fmaxf(1.5f, size * 0.08f);
        ImVec2 p = ImGui::GetCursorScreenPos(), a(p.x + inset, p.y + inset), b(p.x + size - inset, p.y + size - inset);
        ImDrawList *draw = ImGui::GetWindowDrawList();
        ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
        if (prompt.symbol == INPUT_PROMPT_CIRCLE)
            draw->AddCircle(ImVec2(p.x + size * 0.5f, p.y + size * 0.5f), size * 0.35f, color, 24, thickness);
        else if (prompt.symbol == INPUT_PROMPT_CROSS) {
            draw->AddLine(a, b, color, thickness);
            draw->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, a.y), color, thickness);
        } else if (prompt.symbol == INPUT_PROMPT_TRIANGLE)
            draw->AddTriangle(ImVec2(p.x + size * 0.5f, a.y), b, ImVec2(a.x, b.y), color, thickness);
        else draw->AddRect(a, b, color, 0, 0, thickness);
        ImGui::Dummy(ImVec2(size, size));
    }
    ImGui::SameLine(); ImGui::TextUnformatted(action);
}
extern "C" void photo_mode_draw(float scale) {
    if (!state.active || capture_drawing) return;
    auto &io = ImGui::GetIO();
    float dt = fminf(io.DeltaTime, 0.05f);
    bool was_framing = state.framing;
    if (state.framing) {
        photo_state_pan(&state, -stick(ImGuiKey_GamepadLStickRight, ImGuiKey_GamepadLStickLeft) * dt * 0.4f,
                                -stick(ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp) * dt * 0.4f);
        photo_state_zoom(&state, stick(ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown) * dt * 4);
    }
    // This transparent window sits behind the panel. A drag starts only on the photo, so sliders/buttons
    // retain their ordinary input; once started, a drag remains continuous even when it crosses the panel.
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
    if (frame_focus) { ImGui::SetNextWindowFocus(); frame_focus = false; }
    ImGui::Begin("##photo_canvas", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavInputs |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
    bool over_photo = ImGui::IsWindowHovered() && io.MousePos.x >= base_rect[0] * io.DisplaySize.x &&
                      io.MousePos.x < (base_rect[0] + base_rect[2]) * io.DisplaySize.x &&
                      io.MousePos.y >= base_rect[1] * io.DisplaySize.y &&
                      io.MousePos.y < (base_rect[1] + base_rect[3]) * io.DisplaySize.y;
    if (over_photo && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) dragging = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging = false;
    if (dragging && base_rect[2] > 0 && base_rect[3] > 0)
        photo_state_pan(&state, io.MouseDelta.x / (float)(base_rect[2] * io.DisplaySize.x),
                                io.MouseDelta.y / (float)(base_rect[3] * io.DisplaySize.y));
    if (over_photo) photo_state_zoom(&state, io.MouseWheel);
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) photo_state_zoom(&state, 1);
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) photo_state_zoom(&state, -1);
    }
    ImGui::End();
    float target = state.framing ? 1 : 0;
    framing_fade += fmaxf(-dt * 8, fminf(dt * 8, target - framing_fade));
    if (controls && framing_fade < 1) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1 - framing_fade);
        ImGui::SetNextWindowPos(ImVec2((20 - framing_fade * 40) * scale, 20 * scale), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(fminf(680 * scale, io.DisplaySize.x - 40 * scale), 0));
        ImGui::SetNextWindowBgAlpha(0.9f);
        if (focus || return_focus) ImGui::SetNextWindowFocus();
        ImGui::Begin("##photo", NULL, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoTitleBar | (state.framing ? ImGuiWindowFlags_NoInputs : 0));
        ImGui::BeginDisabled(state.framing);
        ImGui::TextColored(ImVec4(.93f,.76f,.33f,1), "%s", "Photo mode");
        ImGui::TextWrapped("%s", "F2 / Esc: resume   F12: save photo   Tab: hide controls");
        ImGui::TextWrapped("%s", "Drag the photo to pan. Scroll or press + / - to zoom.");
        ImGui::BeginDisabled(!state.frozen);
        if (focus || return_focus) {
            ImGui::SetKeyboardFocusHere(); ImGui::SetNavCursorVisible(true); focus = return_focus = false;
        }
        if (ImGui::Button("Frame photo") && !was_framing) {
            photo_state_begin_framing(&state); framing = true; frame_focus = true;
        }
        ImGui::SameLine(); ImGui::Text("%.2fx", state.zoom);
        if (ImGui::Button("Save photo")) photo_mode_capture();
        ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Resume game")) photo_mode_exit();
        ImGui::Separator();
        ImGui::PushItemWidth(220 * scale);
        ImGui::SliderFloat("Exposure", &state.exposure, -2, 2, "%+.1f EV");
        ImGui::SliderFloat("Saturation", &state.saturation, 0, 2, "%.2f");
        ImGui::PopItemWidth();
        if (ImGui::Checkbox("Hide game interface", &state.hide_hud)) photo_state_refresh(&state, renderer_present_count());
        if (ImGui::Button("Reset framing and color")) {
            state.zoom = state.saturation = 1; state.pan_x = state.pan_y = state.exposure = 0;
        }
        if (!message.empty()) {
            if (!saved_path.empty() && ImGui::BeginTable("##photo_saved", 2)) {
                ImGui::TableSetupColumn("message", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("folder", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize(reveal_label).x + 2 * ImGui::GetStyle().FramePadding.x);
                ImGui::TableNextColumn(); ImGui::TextWrapped("%s", message.c_str());
                ImGui::TableNextColumn(); ImGui::BeginDisabled(reveal_process != 0);
                if (ImGui::Button(reveal_label)) reveal_photo();
                ImGui::EndDisabled(); ImGui::EndTable();
            } else ImGui::TextWrapped("%s", message.c_str());
        }
        ImGui::EndDisabled(); ImGui::End(); ImGui::PopStyleVar();
    }
    if (framing_fade > 0) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, framing_fade);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 24 * scale, io.DisplaySize.y - 24 * scale), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##photo_legend", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing);
        ImGui::TextColored(ImVec4(.93f,.76f,.33f,1), "Frame photo   %.2fx", state.zoom);
        ImGui::TextUnformatted("Left stick: pan    Right stick: zoom");
        button_prompt(BIND_B, "Cancel"); ImGui::SameLine(0, 24 * scale);
        button_prompt(BIND_A, "Take photo");
        ImGui::End(); ImGui::PopStyleVar();
    }
}
