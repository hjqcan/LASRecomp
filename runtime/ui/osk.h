// On-screen keyboard (ImGui) for controllers, shared by the game's name entry (runtime/ui/menu.mm keyboard_draw,
// runtime/hle/swkbd.c) and every text field in our menus (in-game overlay, start screen): ui_input_text.
#pragma once
#include <stddef.h>
#include "imgui.h"

// A text field. Activated with a controller (A on it) it opens the on-screen keyboard and the confirmed text goes
// back into buf; with a mouse / keyboard it's typed into directly; a finger (phones) opens the system's text prompt
// (ui_text_inline: typed into directly, with the system keyboard - a search field that filters as it's typed).
// Returns true when the text changed (typed, or OK on the on-screen keyboard; with
// ImGuiInputTextFlags_EnterReturnsTrue: Enter / OK). title heads the keyboard.
bool ui_input_text(const char *label, char *buf, size_t size, ImGuiInputTextFlags flags = 0, const char *hint = NULL,
                   const char *title = NULL);
void ui_text_inline(bool on);  // (the fields drawn until it's turned off)
// true (once) after the on-screen keyboard of the field just drawn with ui_input_text was cancelled
bool ui_input_text_cancelled(void);
// Phones: the field being typed into (the system keyboard), its frame in the current context's coordinates, as
// drawn this frame. True on the frames it is to be sent to the system: the one it took the focus, and after every
// key - the keyboard is then slid out of its way (not as it scrolls: a slide under a dragging finger fed the scroll).
bool ui_text_field_rect(ImVec2 *mn, ImVec2 *mx);
// Phones: what types - true: a controller was used last (the on-screen keyboard); false: the touch screen (the system
// keyboard / prompt). Told by the event loops; the start screen's and the in-game menu's.
void ui_typing_pad(bool pad);
bool ui_typing_by_pad(void);

// Once per frame, after the rest of the UI, at the root (outside any window), in the context the field was drawn in.
void osk_draw(ImFont *big, float scale);
bool osk_is_open(void);
void osk_cancel(void);  // (menu closed / reboot: drop it, the field keeps its text)

// Something is being typed in the current context: a text field has keyboard focus or the on-screen keyboard is up.
// Menu shortcuts (back, tabs, close) must not fire then: the keys are text.
bool ui_text_busy(void);
// An ImGui text field has the keyboard focus (what the system's text input / keyboard should follow; not the
// on-screen keyboard or the system prompt)
bool ui_text_typed_into(void);

// The typed text (big font) and the key grid, drawn into the current window: d-pad / stick move, A types, B deletes,
// Start / + OK; typed keys work too (Enter OK, Esc cancel). text: ASCII, at most max_chars. Returns 1 OK, -1 cancel,
// 0 still typing.
// touch: keys sized to the window's width for fingers, no controller cursor, a tap-oriented hint.
int osk_keys(char *text, size_t cap, int max_chars, bool appearing, float scale, ImFont *big, bool upper_first = true,
             bool touch = false);
