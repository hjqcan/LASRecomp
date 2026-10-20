// Controller buttons into our ImGui menus (the in-game overlay runtime/ui/menu.mm, the start screen launcher.mm).
// The menus read ImGuiKey_GamepadFaceDown as "confirm" and ImGuiKey_GamepadFaceRight as "back" (as ImGui's own
// navigation does). Which physical button each one is: the Controls tab's "Confirm button" (g_settings.menu_confirm).
// Game layout (default): the right button confirms and the bottom one goes back on every pad, as in the game.
// Controller convention (#172, input_menu_family in runtime/hle/input.c): Xbox / PlayStation / generic pads confirm
// with the bottom button and go back with the right one, Nintendo pads the other way round. The prompts' names follow
// the same choice. The player's game bindings never apply here.
#pragma once
#include <SDL3/SDL.h>
#include "imgui.h"

// The ImGui key for a press / release of `button` on controller `which` (a release gets the key its press got)
ImGuiKey ui_pad_key(SDL_JoystickID which, Uint8 button, bool down);
// The family of the controller last used in a menu (0 Nintendo, 1 Xbox, 2 PlayStation): the hints' words
int ui_pad_family(void);
// The confirm / back / start buttons' names in that family's words (e.g. PlayStation, game layout: "Circle" / "Cross" /
// "Options"; controller convention: "Cross" / "Circle" / "Options")
const char *ui_pad_confirm_label(void);
const char *ui_pad_back_label(void);
const char *ui_pad_start_label(void);
