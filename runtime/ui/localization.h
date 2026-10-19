#pragma once
#include <string>

// English keys stay stable in code and in saved settings. Only display text is translated.
enum UiLanguage { UI_ENGLISH, UI_HANS, UI_HANT };
UiLanguage ui_language();
const char *ui_tr(const char *english);
std::string ui_label(const char *english); // translated label + stable ImGui ### ID
struct ImFont;
void ui_merge_language_font(ImFont *font);
ImFont *ui_add_user_font(); // one small CJK font for arbitrary mod/seed names, instead of baking every large title font
void ui_user_text(const char *text, bool wrap = false);
bool ui_user_selectable(const char *text, const char *id, bool selected);
