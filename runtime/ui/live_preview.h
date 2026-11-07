// The live preview (runtime/ui/live_preview.mm): a setting whose effect you need to see (Internal resolution, Anti-
// aliasing...) opens on its own, the settings window gone and the game behind a small panel at the bottom - the row's
// name and value (‹ ›), its description, Return and Reset to default. Left / right change it live, A keeps it, B goes
// back (a Yes / No to save when it changed: No, or B again, puts the old value back).
//
// A page opts a row in with opt_preview (option_rows.h) instead of opt_choice: A / a click opens the preview. While it
// is up, the page that drew the row is run again each frame with its rows only read (not drawn), so its own code - the
// caller's `if (opt_preview(...)) g_settings.x = ...` and a description put together that frame - keeps working.
#pragma once
#include "imgui.h"

// the preview is up (the menu draws it instead of the settings window)
bool preview_active(void);
// opt_preview: this row (label: its id) opens; focus_row: its index on the page (opt_focus_row on the way back; -1 none)
void preview_open(const char *label, int focus_row);
// closed without a prompt, keeping the value (the menu closed under it)
void preview_reset(void);
// this frame's page run is only reading rows for the preview: opt_preview hands its row to preview_row
bool preview_scanning(void);
// The panel's own short text for the opt_preview row right after this call (a frame's page run): desc - one plain line
// instead of the row's longer description; note - shown after the value in brackets, e.g. the render size. English
// (put through L) / already in the menus' language.
void preview_text(const char *desc, const char *note);
// the row the preview shows (other labels: false); applies this frame's change to *v. True when *v changed.
bool preview_row(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap,
                 bool off);
// One frame of the preview: input, the page run (page(): draws the page the row is on, e.g. tab_display), the panel.
// font / font_big: the menu's. Returns true the frame it closed.
bool preview_draw(float scale, ImFont *font, ImFont *font_big, void (*page)(void));
