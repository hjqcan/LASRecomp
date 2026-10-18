// Exercise the real photo panel with ImGui input, without a renderer or game files.
#include <cassert>
#include <fstream>
#include <vector>
#include "settings.h"
#include "ui/photo_mode.mm"

static std::string data_root;
static bool playable;
static uint64_t test_frame = 10;
static std::vector<std::string> launched;
Settings g_settings;
int16_t g_bind_pad[BIND_PAD_COUNT] = {1, 0, 3, 2};
static int pad_family;
static bool lower_left;
extern "C" int input_pad_family(void) { return pad_family; }
extern "C" bool renderer_origin_lower_left(void) { return lower_left; }
extern "C" int bind_prompt_host(int slot) { return slot >= 0 && slot < BIND_PAD_COUNT ? g_bind_pad[slot] : -1; }
extern "C" const char *rt_data_root(void) { return data_root.c_str(); }
extern "C" int game_in_play(void) { return playable; }
extern "C" void input_reset(void) {}
extern "C" uint64_t renderer_present_count(void) { return test_frame; }
extern "C" PlatProc plat_spawn(const char *const args[], const char *, bool) {
    launched.clear();
    for (int i = 0; args[i]; ++i) launched.push_back(args[i]);
    return 1;
}
extern "C" int plat_proc_wait(PlatProc, bool block) { assert(!block); return 0; }
static void tick(int count = 1) {
    while (count--) {
        photo_mode_frame(++test_frame);
        ImGui::NewFrame(); photo_mode_draw(2.0f / 3); ImGui::Render();
    }
}
static void press(ImGuiKey key) {
    ImGui::GetIO().AddKeyEvent(key, true); tick();
    ImGui::GetIO().AddKeyEvent(key, false); tick();
}
static void pad_press(int host) {
    assert(photo_mode_pad_button(host, true)); tick();
    photo_mode_pad_button(host, false); tick();
}
static ImVec2 image_position(void) {
    float crop[4] = {0, 0, 1, 1};
    double x = 0, y = 0, w = 1280, h = 720;
    photo_mode_compose(crop, w, h, &x, &y, &w, &h);
    // Where a fixed source-image landmark appears after the present shader's crop and origin flip.
    float px = (0.5f - crop[0]) / crop[2], py = (0.5f - crop[1]) / crop[3];
    return ImVec2(px, lower_left ? 1 - py : py);
}
static void check_pan_directions(void) {
    auto &io = ImGui::GetIO();
    const ImVec2 moves[] = {{40, 0}, {-40, 0}, {0, 40}, {0, -40}};
    const ImGuiKey keys[] = {ImGuiKey_GamepadLStickRight, ImGuiKey_GamepadLStickLeft,
                            ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadLStickUp};
    for (bool origin : {false, true}) {
        lower_left = origin;
        for (unsigned i = 0; i < 4; i++) {
            state.zoom = 2; state.pan_x = state.pan_y = 0;
            io.AddMousePosEvent(1000, 500); tick(2);
            io.AddMouseButtonEvent(0, true); tick();
            ImVec2 before = image_position();
            io.AddMousePosEvent(1000 + moves[i].x, 500 + moves[i].y); tick();
            ImVec2 after = image_position();
            assert(fabsf(after.x - before.x - moves[i].x / 1280) < 0.00001f);
            assert(fabsf(after.y - before.y - moves[i].y / 720) < 0.00001f);
            io.AddMouseButtonEvent(0, false); tick();

            state.pan_x = state.pan_y = 0;
            photo_state_begin_framing(&state); framing = true;
            before = image_position();
            io.AddKeyAnalogEvent(keys[i], true, 0.75f); tick(2);
            after = image_position();
            assert((after.x - before.x) * moves[i].x + (after.y - before.y) * moves[i].y > 0);
            io.AddKeyAnalogEvent(keys[i], false, 0); tick();
            photo_state_end_framing(&state, true); framing = false;
            tick(12);
        }
    }
}
static void check_prompts(void) {
    g_settings.prompts = PROMPTS_AUTO;
    for (int family = 0; family < 3; ++family) {
        pad_family = family;
        const char *letters[] = {"ABXY", "BAYX"};
        const int symbols[] = {INPUT_PROMPT_CIRCLE, INPUT_PROMPT_CROSS, INPUT_PROMPT_TRIANGLE, INPUT_PROMPT_SQUARE};
        for (int slot = BIND_A; slot <= BIND_Y; ++slot) {
            auto p = input_button_prompt(slot);
            if (family == 2) assert(p.symbol == symbols[slot]);
            else assert(p.symbol == INPUT_PROMPT_TEXT && p.text[0] == letters[family][slot] && !p.text[1]);
        }
    }
    // An explicit style takes priority over the attached controller, as it does for game textures.
    for (int setting : {PROMPTS_NINTENDO, PROMPTS_XBOX, PROMPTS_PLAYSTATION}) {
        g_settings.prompts = setting;
        for (int family = 0; family < 3; ++family) {
            pad_family = family; auto p = input_button_prompt(BIND_A);
            if (setting == PROMPTS_PLAYSTATION) assert(p.symbol == INPUT_PROMPT_CIRCLE);
            else assert(!strcmp(p.text, setting == PROMPTS_XBOX ? "B" : "A"));
        }
    }
    g_settings.prompts = PROMPTS_AUTO; pad_family = 2;
    g_bind_pad[BIND_A] = 3; g_bind_pad[BIND_B] = 2;
    assert(input_button_prompt(BIND_A).symbol == INPUT_PROMPT_TRIANGLE);
    assert(input_button_prompt(BIND_B).symbol == INPUT_PROMPT_SQUARE);
    g_bind_pad[BIND_A] = PADB_RTRIGGER; assert(!strcmp(input_button_prompt(BIND_A).text, "R2"));
    g_bind_pad[BIND_B] = 7; assert(!strcmp(input_button_prompt(BIND_B).text, "L3"));
    g_bind_pad[BIND_A] = -1; assert(!strcmp(input_button_prompt(BIND_A).text, "(none)"));
    g_bind_pad[BIND_A] = 1; g_bind_pad[BIND_B] = 0;
}
int main(int argc, char **argv) {
    assert(argc == 2); data_root = argv[1];
    check_prompts();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = NULL; io.DisplaySize = ImVec2(1280, 720); io.DeltaTime = 1.0f / 60;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.Fonts->AddFontDefault(); io.Fonts->Build();
    photo_mode_toggle(); photo_mode_frame(test_frame); assert(!photo_mode_active());
    playable = true; photo_mode_toggle(); photo_mode_frame(++test_frame);
    float crop[4] = {0, 0, 1, 1}; double x = 0, y = 0, w = 1280, h = 720;
    photo_mode_compose(crop, w, h, &x, &y, &w, &h);
    test_frame += 2; photo_mode_presented(test_frame); assert(photo_mode_hold());
    tick(3);

    // A enters framing from the focused button; sticks then control the photo, not panel navigation.
    assert(!photo_mode_pad_button(1, true));
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true); tick(); assert(state.framing);
    assert(!photo_mode_pad_button(1, false)); // menu_handle_event must forward this release to ImGui
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false); tick();
    assert(!ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown));
    assert(GImGui->NavWindow && !strcmp(GImGui->NavWindow->Name, "##photo_canvas"));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickUp, true, 0.8f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, true, 0.7f);
    tick(30); assert(state.zoom > 1 && state.pan_x < 0 && state.pan_x >= -1);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadRStickUp, false, 0);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, false, 0);
    pad_press(g_bind_pad[BIND_B]);
    assert(!state.framing && state.zoom == 1 && state.pan_x == 0);
    tick(12);
    press(ImGuiKey_GamepadFaceDown); assert(state.framing); // focus returned to Frame photo
    pad_press(g_bind_pad[BIND_A]); assert(!state.framing); // mapped A captures and returns to the panel
    const char *path = photo_mode_capture_path(); assert(path);
    std::ofstream(std::filesystem::u8path(path), std::ios::binary) << "fixture image";
    photo_mode_capture_done();
    assert(message == "Photo saved: " + std::filesystem::u8path(saved_path).filename().u8string());
    assert(message.find(data_root) == std::string::npos);
    reveal_photo();
#ifdef _WIN32
    assert(launched.size() == 3 && launched[1] == "/select,");
    assert(std::filesystem::u8path(launched[2]) == std::filesystem::u8path(saved_path).make_preferred());
#elif defined(__APPLE__)
    assert(launched == std::vector<std::string>({"/usr/bin/open", "-R", saved_path}));
#else
    assert(launched == std::vector<std::string>({"/usr/bin/xdg-open", std::filesystem::u8path(saved_path).parent_path().u8string()}));
#endif
    tick(12); assert(!reveal_process);

    // Rebound face buttons and triggers must do what the displayed legend says, not the old A/B positions.
    g_bind_pad[BIND_A] = 3; g_bind_pad[BIND_B] = 2;
    press(ImGuiKey_GamepadFaceDown); assert(state.framing);
    pad_press(1); assert(state.framing); // old confirm no longer captures
    pad_press(0); assert(state.framing); // old cancel no longer leaves framing
    pad_press(2); assert(!state.framing && !capture_pending);
    tick(12); press(ImGuiKey_GamepadFaceDown); assert(state.framing);
    pad_press(3); assert(!state.framing && capture_pending); capture_pending = false;
    g_bind_pad[BIND_A] = PADB_RTRIGGER; g_bind_pad[BIND_B] = PADB_LTRIGGER;
    tick(12); press(ImGuiKey_GamepadFaceDown); assert(state.framing);
    pad_press(PADB_LTRIGGER); assert(!state.framing && !capture_pending);
    tick(12); press(ImGuiKey_GamepadFaceDown); assert(state.framing);
    pad_press(PADB_RTRIGGER); assert(!state.framing && capture_pending); capture_pending = false;
    tick(12);

    press(ImGuiKey_Equal); assert(state.zoom > 1);
    press(ImGuiKey_Minus); assert(fabsf(state.zoom - 1) < 0.00001f);
    press(ImGuiKey_KeypadAdd); assert(state.zoom > 1);
    press(ImGuiKey_KeypadSubtract); assert(fabsf(state.zoom - 1) < 0.00001f);
    io.AddMousePosEvent(1000, 500); tick(2);
    io.AddMouseWheelEvent(0, 3); tick(); assert(state.zoom > 1);
    io.AddMouseButtonEvent(0, true); tick();
    io.AddMousePosEvent(1100, 550); tick(); assert(state.pan_x < 0 && state.pan_y < 0);
    io.AddMouseButtonEvent(0, false); tick();
    photo_mode_toggle_controls(); tick(); assert(!controls);
    float zoom = state.zoom; press(ImGuiKey_KeypadAdd); assert(state.zoom > zoom);
    check_pan_directions();
    photo_mode_exit(); tick(); assert(!photo_mode_active());
    ImGui::DestroyContext();
    puts("Photo panel: mouse, keyboard, controller framing/focus, family/remapped prompts and actions, private shell-free reveal passed");
}
