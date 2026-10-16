// No game assets required: exercise the real catalog, ImGui IDs, and optional host-font coverage.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ui/localization.h"
#include "imgui.h"
#include "imgui_internal.h"

static const char *language = "en-US";
extern "C" const char *settings_language(void) { return language; }
struct Text { const char *en, *hans, *hant; };
static const Text texts[] = {
#define UI_TEXT(en, hans, hant) {en, hans, hant},
#include "ui/localization_strings.h"
#undef UI_TEXT
};

int main(int argc, char **) {
    for (const auto &t : texts) {
        language = "en-US";
        assert(!strcmp(ui_tr(t.en), t.en));
        ImGuiID id = ImHashStr(ui_label(t.en).c_str());
        language = "zh-Hans";
        assert(!strcmp(ui_tr(t.en), t.hans));
        assert(ImHashStr(ui_label(t.en).c_str()) == id);
        language = "zh-Hant";
        assert(!strcmp(ui_tr(t.en), t.hant));
        assert(ImHashStr(ui_label(t.en).c_str()) == id);
        language = "fr";
        assert(!strcmp(ui_tr(t.en), t.en));
    }
    language = "zh-Hans";
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
        assert(ImGui::GetIO().Fonts->Build());
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
