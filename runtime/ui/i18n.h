// Menu text in the player's language (runtime/ui/i18n.mm): the menus follow the Language setting, Auto the system's
// (settings_language). Text is looked up by its English, in runtime/ui/lang/<code>.inc; English shows for anything not
// translated yet (LARECOMP_I18N_MISSING=1 logs each once).
//
//   ImGui::TextUnformatted(L("No seeds yet"));
//   snprintf(buf, sizeof buf, L("%d seeds"), n);
//
// The option rows (option_rows.h) translate their own labels, descriptions and values: their call sites stay English.
#pragma once
#include <string>

#include "imgui.h"
#include "../i18n_mark.h"

// the menus' language: an index into g_language_codes (never LANG_AUTO)
int i18n_lang(void);
bool i18n_english(void);
// English text -> the menu language's (the same pointer when there is none)
const char *L(const char *en);
// a line put together from pieces (the spoiler log, warp spots, the tracker's places): the whole line if it has a
// translation, else its parts - "a  ->  b", "%s" patterns ("Outside %s"), a bracketed end "x (y)", "a: b", "a, b"
const std::string &L_line(const std::string &en);
// UTF-8 capitals (Latin and Cyrillic: the option rows' labels)
std::string i18n_upper(const char *s);

// Fonts: the glyphs every menu font needs (Latin, the marks the menus draw, every character of this language's text
// and the language names), and the fonts of the other scripts merged after a Latin one so those glyphs exist.
// rel: the merged glyphs' size to the font's (Baloo 2 runs small: its CJK would stand out at the same size).
const ImWchar *i18n_glyph_ranges(void);
void i18n_merge_fonts(ImFontAtlas *atlas, float size, float rel = 1.0f);
// the atlas's fonts were built for another language than the menus' (or never): clear and add them again before the
// next frame. Marks them built for the current one.
bool i18n_fonts_stale(ImFontAtlas *atlas);
