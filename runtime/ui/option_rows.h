// Settings pages in two panes (runtime/ui/option_rows.mm): option rows on the left - an uppercase label, the value on
// the right (brighter when it isn't the default) - and the selected (controller) or hovered (mouse) row's description on
// the right, instead of tooltips. A / click changes a row (toggle / next value), left / right step it.
//
//   opt_begin("##gameplay", scale);
//   opt_section("Difficulty");
//   if (opt_toggle("One-hit KO", &g_settings.diff_ohko, false, "Any hit defeats Link.")) settings_save();
//   opt_end();
#pragma once

void opt_begin(const char *id, float scale);
void opt_section(const char *title);
bool opt_toggle(const char *label, bool *v, bool def, const char *desc);
// wrap: past the last value back to the first (and the other way); false: stops at the ends
bool opt_choice(const char *label, int *v, const char *const *names, int n, int def, const char *desc, bool wrap = true);
// steps of `step` between lo and hi; fmt formats the value (e.g. "%gx")
bool opt_slider(const char *label, float *v, float lo, float hi, float step, float def, const char *fmt, const char *desc);
// whole numbers lo..hi in steps of `step` (holding left / right speeds up); fmt formats it (e.g. "%d rupees"), lo_name
// (if given) is shown for lo instead (e.g. "Any")
bool opt_int(const char *label, int *v, int lo, int hi, int step, int def, const char *fmt, const char *desc,
             const char *lo_name = NULL);
// an action: true when activated
bool opt_button(const char *label, const char *desc);
// one of a set of choices (a ring on the right, filled when selected); true when chosen
bool opt_radio(const char *label, bool selected, const char *desc);
// an action / item row with a value on the right (upper: the label in capitals, as option rows; false: as written,
// e.g. a seed name); true when activated
bool opt_action(const char *label, const char *value, const char *desc, bool upper = true, bool changed = false);
// a row that opens more (a submenu, a list on the right): a chevron on the right; true on A / click / Right
bool opt_link(const char *label, const char *desc);
// text: the value shown on the right (hint when empty); A / click turns the row into a text field (the on-screen
// keyboard with a controller). True when Enter / the keyboard confirmed a change.
#include <stddef.h>
bool opt_text(const char *label, char *buf, size_t cap, const char *hint, const char *desc);
// that text row is being edited (its buffer is the user's: don't overwrite it)
bool opt_text_editing(const char *label);
// the next row is shown greyed out and can't be changed (e.g. forced by the seed)
void opt_disable_next(void);
// the next row is drawn as chosen (a dimmer gold border without the cursor: the list shown beside it)
void opt_mark_next(void);
// the next row's value in red (a problem: e.g. the same button bound twice)
void opt_warn_next(void);
// rows after this sit one step in (+1) or back out (-1): options that belong to the row above
void opt_indent(int step);
void opt_end(void);
// Instead of opt_end: the right pane holds rows of the caller's (e.g. the bindings of the category chosen on the left)
// - opt_split ends the left pane and opens the right one (its own nav scope: up / down stay in it), opt_desc_text
// shows the description there instead, opt_split_end closes it.
void opt_split(void);
void opt_desc_text(void);
void opt_split_end(void);
// the next page's first row takes the controller's cursor (Down on the tab bar: ImGui's own pick across the nested
// panes was unreliable - some tabs went to a row further down, some nowhere). Cleared by the next opt_begin / call.
void opt_focus_first(bool on);
