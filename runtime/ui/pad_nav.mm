// Controller buttons into our ImGui menus: see pad_nav.h.
#include "pad_nav.h"
#include <atomic>
#include "input_bind.h"

static std::atomic<SDL_JoystickID> g_last_pad{0};

ImGuiKey ui_pad_key(SDL_JoystickID which, Uint8 button, bool down) {
    static ImGuiKey held[SDL_GAMEPAD_BUTTON_COUNT];  // (the layout can change while a button is held: its release)
    if (button >= SDL_GAMEPAD_BUTTON_COUNT) return ImGuiKey_None;
    if (!down) { ImGuiKey k = held[button]; held[button] = ImGuiKey_None; if (k != ImGuiKey_None) return k; }
    if (down) g_last_pad = which;
    bool south_confirms = input_menu_family(which) != 0;
    ImGuiKey k;
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: k = south_confirms ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight; break;
    case SDL_GAMEPAD_BUTTON_EAST: k = south_confirms ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown; break;
    case SDL_GAMEPAD_BUTTON_NORTH: k = ImGuiKey_GamepadFaceUp; break;
    case SDL_GAMEPAD_BUTTON_WEST: k = ImGuiKey_GamepadFaceLeft; break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: k = ImGuiKey_GamepadDpadUp; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: k = ImGuiKey_GamepadDpadDown; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: k = ImGuiKey_GamepadDpadLeft; break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: k = ImGuiKey_GamepadDpadRight; break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: k = ImGuiKey_GamepadL1; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: k = ImGuiKey_GamepadR1; break;
    case SDL_GAMEPAD_BUTTON_START: k = ImGuiKey_GamepadStart; break;
    case SDL_GAMEPAD_BUTTON_BACK: k = ImGuiKey_GamepadBack; break;
    default: k = ImGuiKey_None; break;
    }
    if (down) held[button] = k;
    return k;
}

int ui_pad_family(void) { return input_menu_family(g_last_pad.load()) % 3; }
const char *ui_pad_confirm_label(void) {
    int f = ui_pad_family();
    return bind_pad_label(f ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST, f);
}
const char *ui_pad_back_label(void) {
    int f = ui_pad_family();
    return bind_pad_label(f ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH, f);
}
const char *ui_pad_start_label(void) { return bind_pad_label(SDL_GAMEPAD_BUTTON_START, ui_pad_family()); }
