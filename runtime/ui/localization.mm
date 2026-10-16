#include "localization.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include "imgui.h"
extern "C" {
#include "../settings.h"
}

struct Translation { const char *english, *hans, *hant; };
static const Translation texts[] = {
#define UI_TEXT(en, hans, hant) {en, hans, hant},
#include "localization_strings.h"
#undef UI_TEXT
};

UiLanguage ui_language() {
    const char *language = settings_language();
    return !strcmp(language, "zh-Hans") ? UI_HANS : !strcmp(language, "zh-Hant") ? UI_HANT : UI_ENGLISH;
}

const char *ui_tr(const char *english) {
    if (!english) return "";
    UiLanguage language = ui_language();
    if (language == UI_ENGLISH) return english;
    static const auto catalog = [] {
        std::unordered_map<std::string, const Translation *> result;
        for (const auto &t : texts) result.emplace(t.english, &t);
        return result;
    }();
    auto it = catalog.find(english);
    return it == catalog.end() ? english : language == UI_HANS ? it->second->hans : it->second->hant;
}

std::string ui_label(const char *english) {
    return std::string(ui_tr(english)) + "###" + english;
}

void ui_merge_language_font(ImFont *font) {
    if (!font) return;
    // Bake both Chinese catalogs at startup so switching languages never rebuilds a live atlas.
    // Keep this storage alive across the launcher and the game's separate ImGui contexts.
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder builder;
        for (const auto &t : texts) { builder.AddText(t.hans); builder.AddText(t.hant); }
        builder.BuildRanges(&ranges);
    }
    const char *paths[] = {getenv("LARECOMP_UI_FONT"),
        "fonts/ui-cjk.ttf", "fonts/ui-cjk.ttc",
        "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/msjh.ttc",
        "/System/Library/Fonts/PingFang.ttc", "/System/Library/Fonts/STHeiti Light.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc"};
    for (const char *path : paths) {
        if (!path || !*path) continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fclose(f);
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.DstFont = font;
        cfg.OversampleH = cfg.OversampleV = 1;
        if (ImGui::GetIO().Fonts->AddFontFromFileTTF(path, font->Sources[0].SizePixels, &cfg, ranges.Data)) return;
    }
    fprintf(stderr, "[ui] Chinese font missing; install Noto Sans CJK or set LARECOMP_UI_FONT.\n");
}
