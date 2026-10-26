// Settings pages in two panes (runtime/ui/option_rows.mm): option rows on the left - an uppercase label, the value on
// the right (brighter when it isn't the default) - and the selected (controller) or hovered (mouse) row's description on
// the right, instead of tooltips. A / click changes a row (toggle / next value), left / right step it.
//
//   opt_begin("##gameplay", scale);
//   opt_section("Difficulty");
//   if (opt_toggle("One-hit KO", &g_settings.diff_ohko, false, "Any hit defeats Link.")) settings_save();
//   opt_end();
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

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
// a Cosmetics colour row (the shared palette, runtime/cosmetic_colour.h): Default, the named colours, Random, Custom;
// with Custom, a row under it shows the colour and opens a colour picker. True when the colour changed.
struct CosColour;
// slot: the row (COL_SLOT_*): on Default it shows the randomizer seed's colour as "Seed: <colour>" when it has one.
bool opt_colour(const char *label, CosColour *c, int slot, const char *desc);
// an action: true when activated
bool opt_button(const char *label, const char *desc);
// one of a set of choices (a ring on the right, filled when selected); true when chosen
bool opt_radio(const char *label, bool selected, const char *desc);
// an action / item row with a value on the right (upper: the label in capitals, as option rows; false: as written,
// e.g. a seed name); true when activated
bool opt_action(const char *label, const char *value, const char *desc, bool upper = true, bool changed = false);
// a row that opens more (a submenu, a list on the right): a chevron on the right; true on A / click / Right. value:
// shown left of the chevron
bool opt_link(const char *label, const char *desc, const char *value = NULL);
// text: the value shown on the right (hint when empty); A / click turns the row into a text field (the on-screen
// keyboard with a controller). True when Enter / the keyboard confirmed a change.
#include <stddef.h>
bool opt_text(const char *label, char *buf, size_t cap, const char *hint, const char *desc);
// that text row is being edited (its buffer is the user's: don't overwrite it)
bool opt_text_editing(const char *label);
// the next row is shown greyed out and can't be changed (e.g. forced by the seed)
void opt_disable_next(void);
// the next row is drawn as chosen (pale orange without the cursor: the list shown beside it)
void opt_mark_next(void);
// the next row's value in red (a problem: e.g. the same button bound twice)
void opt_warn_next(void);
void opt_button_next(void);  // the next row (one with a value too, or a text row) as a button
void opt_own_next(void);  // the next row is the player's own setting, not part of a seed: the summary leaves it out
// rows after this sit one step in (+1) or back out (-1): options that belong to the row above
void opt_indent(int step);
void opt_end(void);
// Instead of opt_end: the right pane holds rows of the caller's (e.g. the bindings of the category chosen on the left)
// - opt_split ends the left pane and opens the right one (its own nav scope: up / down stay in it), opt_desc_text
// shows the description there instead, opt_split_end closes it.
void opt_split(void);
void opt_desc_text(void);
void opt_split_end(void);
// the next page's first enabled row (greyed-out rows are passed over) takes the controller's cursor (Down on the tab
// bar, or the first Down on a page just switched to: ImGui's own pick across the nested panes was unreliable - some
// tabs went to a row further down, some nowhere). Cleared by the next opt_begin / call.
void opt_focus_first(bool on);
// this frame's page: the row with this index (in drawing order, opt_row_count) takes the controller's cursor
void opt_focus_row(int i);
// Up (-1) / Down (+1) with the cursor on an option row (the frame before): the row before / after it in the same pane
// takes the cursor this frame (Down on the last row: it stays). False: not on a row, or Up from the top row - the
// caller leaves that move to ImGui. Call before the page's opt_begin and before opt_frame_reset.
bool opt_nav_step(int dir);
// after a page: true if a focus request is still waiting (the page has no enabled option row); clears it
bool opt_focus_first_left(void);
// this frame's page (focus checks, LARECOMP_MENU_FOCUS_LOG): its rows in order, the first enabled one and the focused
// one (indices, -1: none); opt_frame_reset clears them before a page is drawn
int opt_row_count(void);
const char *opt_row_label(int i);
int opt_first_row(void);
int opt_focused_row(void);
void opt_frame_reset(void);

// Two columns (the Randomizer, Save Editor and Warp tabs): categories on the left, the chosen one's rows on the right
// with the selected / hovered row's description under them. After opt_split: opt_list_begin, the rows, opt_list_end,
// opt_desc_text, opt_split_end. The list scrolls on its own when it's longer than the room above the description.
void opt_list_begin(void);
// this page's rows a little lower (after opt_begin: the two-column tabs, whose left column must fit)
void opt_compact(void);
// the next two rows share a line, half each (Up / Down go through them in order)
void opt_pair_next(void);
void opt_list_end(void);
// the next enabled row drawn takes the controller's cursor (e.g. the first row of the right column)
void opt_focus_next_row(void);
// the row just drawn takes the controller's cursor (kept in view)
void opt_focus_item(void);
// the row just drawn has the controller's cursor
bool opt_item_nav_focused(void);
// the selected row steps with left / right (a toggle, a choice, a number): Left there changes it, not the column
bool opt_focused_lr(void);
// after the page: the selected row is in the right column
bool opt_in_right(void);
// B / Esc, and Left (d-pad, stick, arrow key), pressed this frame
bool opt_pressed_back(void);
bool opt_pressed_left(void);
// Rows between these are only read, not drawn (the Randomizer's summary): each enabled row with a value goes into out.
// Calls of the caller's own that use the last item (ImGui::IsItemFocused, popups) must be skipped while opt_collecting().
struct OptLine { std::string label, value; bool changed; int indent; };  // (indent: opt_indent steps)
void opt_collect_begin(std::vector<OptLine> *out);
bool opt_collecting(void);
void opt_collect_end(void);
