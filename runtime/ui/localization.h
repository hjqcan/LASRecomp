#pragma once
#include <string>

// English keys stay stable in code and in saved settings. Only display text is translated.
enum UiLanguage { UI_ENGLISH, UI_HANS, UI_HANT };
UiLanguage ui_language();
const char *ui_tr(const char *english);
std::string ui_label(const char *english); // translated label + stable ImGui ### ID
struct ImFont;
void ui_merge_language_font(ImFont *font);
