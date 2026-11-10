// No game assets required: exercise the real catalog, ImGui IDs, and optional host-font coverage.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui/localization.h"
#include "ui/i18n.h"
extern "C" {
#include "settings.h"
Settings g_settings;
bool rt_resource(const char *, const char *, char *, size_t) { return false; }
}

#include "imgui.h"
#include "imgui_internal.h"

extern "C" {
const char *const g_language_codes[N_LANGUAGES] = {"auto", "ja", "en-US", "es", "fr", "de", "it", "nl", "ru", "ko",
                                                   "zh-Hans", "zh-Hant", "pt-BR"};
const char *const g_language_names[N_LANGUAGES] = {"Auto", "Japanese", "English", "Spanish", "French", "German",
                                                   "Italian", "Dutch", "Russian", "Korean", "Chinese (Simplified)",
                                                   "Chinese (Traditional)", "Portuguese (Brazil)"};
const char *const g_language_endonyms[N_LANGUAGES] = {"Auto", "日本語", "English", "Español", "Français", "Deutsch",
                                                      "Italiano", "Nederlands", "Русский", "한국어", "简体中文", "繁體中文",
                                                      "Português (Brasil)"};

}
static const char *language = "en-US";
static void select_language(const char *value) { language = value; for (int i=1; i<N_LANGUAGES; i++) if (!strcmp(value,g_language_codes[i])) g_settings.language=i; }
extern "C" const char *settings_menu_language(void) { return language; }
struct Text { const char *en, *hans, *hant; };
static const Text texts[] = {
#define UI_TEXT(en, hans, hant) {en, hans, hant},
#include "ui/localization_strings.h"
#undef UI_TEXT
};

static const char *official_hans[][2] = {
#include "ui/lang/zh-Hans.inc"
};
static const char *official_hant[][2] = {
#include "ui/lang/zh-Hant.inc"
};
template<size_t N> static const char *expected(const char *en, const char *fallback, const char *(&table)[N][2]) {
    const char *value = fallback;
    for (auto &row : table) if (!strcmp(row[0], en) && *row[1]) value = row[1];
    return value;
}

int main(int argc, char **) {
    for (const auto &t : texts) {
        select_language("en-US");
        assert(!strcmp(ui_tr(t.en), t.en));
        ImGuiID id = ImHashStr(ui_label(t.en).c_str());
        select_language("zh-Hans");
        assert(!strcmp(ui_tr(t.en), expected(t.en, t.hans, official_hans)));
        assert(ImHashStr(ui_label(t.en).c_str()) == id);
        select_language("zh-Hant");
        assert(!strcmp(ui_tr(t.en), expected(t.en, t.hant, official_hant)));
        assert(ImHashStr(ui_label(t.en).c_str()) == id);
        select_language("fr");
        assert(!strcmp(ui_tr(t.en), L(t.en)));
    }
    select_language("zh-Hans");
    assert(!strcmp(ui_tr("user seed name"), "user seed name"));
    assert(!strcmp(ui_tr(nullptr), ""));
    // Already-suffixed labels (remapping buttons) must keep the ID through translated status changes.
    assert(ImHashStr(ui_label("K###0_3").c_str()) == ImHashStr(ui_label("等待###0_3").c_str()));
    if (argc > 1) {
        ImGui::CreateContext();
        ImFontConfig config;
        config.SizePixels = 39;
        ImFont *font = ImGui::GetIO().Fonts->AddFontDefault(&config);
        ui_merge_language_font(font);
        ImFont *user_font = ui_add_user_font();
        assert(ImGui::GetIO().Fonts->Build());
        assert(user_font && user_font->FindGlyphNoFallback(0x9884) && user_font->FindGlyphNoFallback(0x89c8)); // arbitrary mod names: 预览
        assert(ImGui::GetIO().Fonts->TexWidth <= 16384 && ImGui::GetIO().Fonts->TexHeight <= 16384);
        int missing = 0;
        for (const auto &t : texts) for (const char *str : {t.hans, t.hant}) {
            for (const char *p = str; *p;) {
                unsigned int cp;
                p += ImTextCharFromUtf8(&cp, p, nullptr);
                if (cp >= 0x20 && !font->FindGlyphNoFallback((ImWchar)cp)) {
                    fprintf(stderr, "Missing glyph U+%04X in %s\n", cp, t.en);
                    missing++;
                }
            }
        }
        assert(missing == 0);
        ImGui::DestroyContext();
    }
    printf("%zu translations: live locale changes, English fallback and stable ImGui IDs passed%s\n",
           sizeof texts / sizeof texts[0], argc > 1 ? "; Chinese font glyph coverage passed" : "");
}
