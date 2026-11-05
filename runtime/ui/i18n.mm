// Menu text in the player's language (i18n.h). Tables: runtime/ui/lang/<code>.inc, {"English", "translation"} pairs;
// tools/menu_strings.py lists the English the menus use and what each table still lacks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "i18n.h"
#include "imgui_internal.h"  // (ImTextCharFromUtf8)

extern "C" {
#include "../paths.h"
#include "../rt.h"
#include "../settings.h"
}

namespace {
typedef const char *const Pair[2];
const char *const k_ja[][2] = {
#include "lang/ja.inc"
    {NULL, NULL}};
const char *const k_es[][2] = {
#include "lang/es.inc"
    {NULL, NULL}};
const char *const k_fr[][2] = {
#include "lang/fr.inc"
    {NULL, NULL}};
const char *const k_de[][2] = {
#include "lang/de.inc"
    {NULL, NULL}};
const char *const k_it[][2] = {
#include "lang/it.inc"
    {NULL, NULL}};
const char *const k_nl[][2] = {
#include "lang/nl.inc"
    {NULL, NULL}};
const char *const k_ru[][2] = {
#include "lang/ru.inc"
    {NULL, NULL}};
const char *const k_ko[][2] = {
#include "lang/ko.inc"
    {NULL, NULL}};
const char *const k_zh_hans[][2] = {
#include "lang/zh-Hans.inc"
    {NULL, NULL}};
const char *const k_zh_hant[][2] = {
#include "lang/zh-Hant.inc"
    {NULL, NULL}};
const char *const k_pt_br[][2] = {
#include "lang/pt-BR.inc"
    {NULL, NULL}};
// by g_language_codes index (Auto and English: none)
const Pair *const k_tables[N_LANGUAGES] = {NULL, k_ja, NULL, k_es, k_fr, k_de, k_it, k_nl, k_ru, k_ko, k_zh_hans, k_zh_hant,
                                           k_pt_br};
enum { L_JA = 1, L_EN = 2, L_RU = 8, L_KO = 9, L_ZH_HANS = 10, L_ZH_HANT = 11 };

struct Table {
    std::unordered_map<std::string_view, const char *> text;
    // keys with "%s" in them and nothing else of printf's ("Outside %s", "%s entrance leads into %s"), most text first:
    // the parts around the %s, and the translation (its %s in order, or %1$s, %2$s, ... to change it)
    std::vector<std::vector<std::string>> patterns;
    std::vector<const char *> pattern_text;
    std::unordered_map<std::string, std::string> lines;  // (L_line's, as put together)
};
Table g_table[N_LANGUAGES];
bool g_loaded[N_LANGUAGES];
int g_lang = -1, g_lang_setting = -1;

Table &table(int lang) {
    Table &t = g_table[lang];
    if (g_loaded[lang]) return t;
    g_loaded[lang] = true;
    std::vector<std::pair<std::vector<std::string>, const char *>> pats;
    for (const Pair *p = k_tables[lang]; p && (*p)[0]; p++) {
        if (!(*p)[1] || !*(*p)[1]) continue;
        t.text[(*p)[0]] = (*p)[1];
        std::vector<std::string> parts(1);
        bool ok = strstr((*p)[0], "%s") != NULL;
        for (const char *c = (*p)[0]; *c && ok; c++) {
            if (c[0] == '%' && c[1] == 's') parts.push_back(""), c++;
            else if (c[0] == '%') ok = false;  // (a printf format: L() only)
            else parts.back() += *c;
        }
        for (size_t i = 1; ok && i + 1 < parts.size(); i++) ok = !parts[i].empty();  // ("%s%s" can't be split)
        if (ok) pats.push_back({parts, (*p)[1]});
    }
    auto fixed = [](const std::vector<std::string> &v) { size_t n = 0; for (auto &x : v) n += x.size(); return n; };
    std::stable_sort(pats.begin(), pats.end(), [&](auto &a, auto &b) { return fixed(a.first) > fixed(b.first); });
    for (auto &p : pats) t.patterns.push_back(p.first), t.pattern_text.push_back(p.second);
    return t;
}

void missing(const char *en) {
    static const bool log = getenv("LARECOMP_I18N_MISSING") != NULL;
    if (!log) return;
    static std::unordered_set<std::string> seen[N_LANGUAGES];
    // (English only: text already in the language, or a map square like "13M", isn't missing anything)
    bool letters = false, ascii = true;
    for (const char *p = en; *p; p++) letters |= (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'), ascii &= *p > 0;
    if (strlen(en) <= 4 && strpbrk(en, "0123456789")) letters = false;
    if (letters && ascii && seen[g_lang].insert(en).second) LOG("[i18n] %s: missing \"%s\"\n", g_language_codes[g_lang], en);
}
}  // namespace

int i18n_lang(void) {
    if (g_lang < 0 || g_lang_setting != g_settings.language) {
        g_lang_setting = g_settings.language;
        const char *code = settings_menu_language();
        if (const char *e = getenv("LARECOMP_MENU_LANG")) code = e;  // (test) the menus alone, in that language
        g_lang = L_EN;
        for (int i = 1; i < N_LANGUAGES; i++)
            if (!strcmp(code, g_language_codes[i])) g_lang = i;
    }
    return g_lang;
}

bool i18n_english(void) { return !k_tables[i18n_lang()]; }

const char *L(const char *en) {
    if (!en || !*en || i18n_english()) return en;
    Table &t = table(g_lang);
    auto it = t.text.find(en);
    if (it != t.text.end()) return it->second;
    missing(en);
    return en;
}

// the parts, each translated (L_line)
static std::string compose(Table &t, const std::string &s, int depth) {
    auto it = t.text.find(s);
    if (it != t.text.end()) return it->second;
    if (depth > 6 || s.empty()) return s;
    auto split = [&](const char *sep) -> std::string {  // (at every one of them: a long list stays shallow)
        size_t at = s.find(sep, 1), from = 0, n = strlen(sep);
        if (at == std::string::npos) return std::string();
        auto j = t.text.find(sep);  // (a separator of the language's own: "、" for ", ")
        std::string out, between = j != t.text.end() ? j->second : sep;
        for (; at != std::string::npos; from = at + n, at = s.find(sep, at + n))
            out += compose(t, s.substr(from, at - from), depth + 1) + between;
        return out + compose(t, s.substr(from), depth + 1);
    };
    std::string r;
    if (!(r = split("  ->  ")).empty()) return r;
    for (size_t i = 0; i < t.patterns.size(); i++) {
        const std::vector<std::string> &parts = t.patterns[i];
        const std::string &pre = parts.front(), &post = parts.back();
        if (s.size() <= pre.size() + post.size() || s.compare(0, pre.size(), pre) ||
            s.compare(s.size() - post.size(), post.size(), post))
            continue;
        // the gaps between the parts, in order (each part's first place after the gap before)
        std::vector<std::string> gaps;
        size_t at = pre.size(), end = s.size() - post.size();
        bool ok = true;
        for (size_t k = 1; k + 1 < parts.size() && ok; k++) {
            size_t f = s.find(parts[k], at + 1);
            ok = f != std::string::npos && f + parts[k].size() < end;
            if (ok) gaps.push_back(s.substr(at, f - at)), at = f + parts[k].size();
        }
        if (!ok || at >= end) continue;
        gaps.push_back(s.substr(at, end - at));
        std::string out;
        const char *tr = t.pattern_text[i];
        size_t next = 0;
        for (const char *c = tr; *c; c++) {
            if (c[0] == '%' && c[1] == 's') { out += next < gaps.size() ? compose(t, gaps[next++], depth + 1) : ""; c++; }
            else if (c[0] == '%' && c[1] >= '1' && c[1] <= '9' && c[2] == '$' && c[3] == 's') {
                size_t g = (size_t)(c[1] - '1');
                out += g < gaps.size() ? compose(t, gaps[g], depth + 1) : "";
                c += 3;
            } else out += *c;
        }
        return out;
    }
    if (s.size() > 3 && s.back() == ')') {  // "Small Key (Bottle Grotto)"
        size_t open = s.rfind(" (");
        if (open != std::string::npos && open > 0)
            return compose(t, s.substr(0, open), depth + 1) + " (" +
                   compose(t, s.substr(open + 2, s.size() - open - 3), depth + 1) + ")";
    }
    {   // "Bush, Mabe Village (north) 2": the second of that name
        size_t sp = s.find_last_not_of("0123456789");
        if (sp != std::string::npos && sp + 1 < s.size() && sp > 0 && s[sp] == ' ')
            return compose(t, s.substr(0, sp), depth + 1) + s.substr(sp);
    }
    if (!(r = split(": ")).empty()) return r;
    if (!(r = split(", ")).empty()) return r;
    if (!(r = split(" / ")).empty()) return r;
    missing(s.c_str());
    return s;
}

const std::string &L_line(const std::string &en) {
    if (i18n_english()) return en;
    Table &t = table(g_lang);
    auto it = t.lines.find(en);
    if (it != t.lines.end()) return it->second;
    return t.lines[en] = compose(t, en, 0);
}

std::string i18n_upper(const char *s) {
    std::string out;
    out.reserve(strlen(s));
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned c = *p;
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        if (n == 1) { out += (char)(c >= 'a' && c <= 'z' ? c - 32 : c); p++; continue; }
        if (n != 2 || !p[1]) { out.append((const char *)p, strnlen((const char *)p, n)); p += strnlen((const char *)p, n); continue; }
        unsigned cp = (c & 0x1F) << 6 | (p[1] & 0x3F), u = cp;
        if ((cp >= 0xE0 && cp <= 0xFE && cp != 0xF7)) u = cp - 0x20;                            // à..þ
        else if (cp == 0xFF) u = 0x178;                                                       // ÿ
        else if (cp >= 0x100 && cp <= 0x137 && (cp & 1)) u = cp - 1;                          // ā..ķ
        else if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) u = (cp & 1) ? cp : cp - 1;  // ĺ..ň, ź..ž
        else if (cp >= 0x14A && cp <= 0x177 && (cp & 1)) u = cp - 1;                          // ŋ..ŷ (œ)
        else if (cp >= 0x430 && cp <= 0x44F) u = cp - 0x20;                                   // а..я
        else if (cp >= 0x450 && cp <= 0x45F) u = cp - 0x50;                                   // ѐ..џ
        out += (char)(0xC0 | u >> 6), out += (char)(0x80 | (u & 0x3F));
        p += 2;
    }
    return out;
}

// ---- fonts

namespace {
std::vector<ImWchar> g_ranges[N_LANGUAGES];

void add_text(std::vector<bool> &set, const char *s) {
    for (const char *p = s; *p;) {
        unsigned c = 0;
        int n = ImTextCharFromUtf8(&c, p, NULL);
        if (!n) break;
        p += n;
        if (c < 0x10000) set[c] = true;
    }
}

// a font file, kept (mapped) for every atlas built from it: ImGui reads it while building
struct FontFile { void *data; int size; };
FontFile font_file(const char *path) {
    static std::unordered_map<std::string, FontFile> files;
    auto it = files.find(path);
    if (it != files.end()) return it->second;
    FontFile f = {NULL, 0};
#ifdef _WIN32
    if (FILE *fp = fopen(path, "rb")) {
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        void *d = n > 0 ? malloc((size_t)n) : NULL;
        if (d && fread(d, 1, (size_t)n, fp) == (size_t)n) f = {d, (int)n};
        else free(d);
        fclose(fp);
    }
#else
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd >= 0 && !fstat(fd, &st) && st.st_size > 0) {
        void *d = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (d != MAP_FAILED) f = {d, (int)st.st_size};
    }
    if (fd >= 0) close(fd);
#endif
    return files[path] = f;
}

// a script's font: the first file of these that exists (face: its index in a .ttc)
struct FontPick { const char *path; int face; };
const FontPick k_cyrillic[] = {{"/System/Library/Fonts/Avenir Next.ttc", 2},  // (Demi Bold)
                               {"C:/Windows/Fonts/seguisb.ttf", 0}, {"C:/Windows/Fonts/segoeui.ttf", 0},
                               {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 0},
                               {"/usr/share/fonts/TTF/DejaVuSans.ttf", 0},
                               {"/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", 0}, {"@DejaVuSans.ttf", 0}, {}};
const FontPick k_japanese[] = {{"/System/Library/Fonts/ヒラギノ丸ゴ ProN W4.ttc", 1},
                               {"C:/Windows/Fonts/YuGothM.ttc", 0}, {"C:/Windows/Fonts/meiryo.ttc", 0},
                               {"/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc", 0},
                               {"/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc", 0},
                               {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 0},
                               {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 0}, {"@BIZUDPGothic-Regular.ttf", 0}, {}};
const FontPick k_hans[] = {{"/System/Library/Fonts/Hiragino Sans GB.ttc", 0}, {"C:/Windows/Fonts/msyh.ttc", 0},
                           {"/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc", 2},
                           {"/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc", 2},
                           {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 2},
                           {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 2},
                           {"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0},
                           {"/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc", 0}, {}};
const FontPick k_hant[] = {{"/System/Library/Fonts/STHeiti Medium.ttc", 0}, {"/System/Library/Fonts/Hiragino Sans GB.ttc", 0},
                           {"C:/Windows/Fonts/msjh.ttc", 0},
                           {"/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc", 3},
                           {"/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc", 3},
                           {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 3},
                           {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 3},
                           {"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", 0},
                           {"/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc", 0}, {}};
const FontPick k_korean[] = {{"/System/Library/Fonts/AppleSDGothicNeo.ttc", 4},  // (SemiBold)
                             {"C:/Windows/Fonts/malgunbd.ttf", 0}, {"C:/Windows/Fonts/malgun.ttf", 0},
                             {"/usr/share/fonts/truetype/nanum/NanumGothicBold.ttf", 0},
                             {"/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc", 1},
                             {"/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc", 1},
                             {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 1},
                             {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", 1}, {}};

bool pick(const FontPick *list, FontFile *out, int *face) {
    for (const FontPick *f = list; f->path; f++) {
        const char *path = f->path;
        char res[1100];
        if (path[0] == '@') {  // shipped with the app (third_party/fonts)
            const char *dev = !strcmp(path + 1, "DejaVuSans.ttf") ? "third_party/fonts/DejaVu/DejaVuSans.ttf"
                                                                   : "third_party/fonts/BIZUDPGothic/BIZUDPGothic-Regular.ttf";
            if (!rt_resource(path + 1, dev, res, sizeof res)) continue;
            path = res;
        }
        FontFile ff = font_file(path);
        if (ff.data) return *out = ff, *face = f->face, true;
    }
    return false;
}
}  // namespace

const ImWchar *i18n_glyph_ranges(void) {
    int lang = i18n_lang();
    std::vector<ImWchar> &r = g_ranges[lang];
    if (!r.empty()) return r.data();
    std::vector<bool> set(0x10000);
    for (unsigned c = 0x20; c < 0x180; c++) set[c] = true;  // Latin, Latin-1, Latin Extended-A
    for (unsigned c = 0x2010; c <= 0x2027; c++) set[c] = true;  // dashes, quotes, bullet, ellipsis
    for (unsigned c : {0x2039u, 0x203Au, 0x2192u, 0x2190u, 0x2191u, 0x2193u, 0x2713u, 0x25B8u, 0x25A1u, 0x2122u})
        set[c] = true;  // ‹ › arrows ✓ ▸ □ ™
    for (int i = 0; i < N_LANGUAGES; i++) add_text(set, g_language_endonyms[i]);
    for (const Pair *p = k_tables[lang]; p && (*p)[0]; p++)
        if ((*p)[1]) add_text(set, (*p)[1]);
    for (unsigned c = 1; c < 0x10000; c++) {
        if (!set[c] || set[c - 1]) continue;
        unsigned e = c;
        while (e + 1 < 0x10000 && set[e + 1]) e++;
        r.push_back((ImWchar)c), r.push_back((ImWchar)e);
    }
    r.push_back(0);
    return r.data();
}

void i18n_merge_fonts(ImFontAtlas *atlas, float size, float rel) {
    int lang = i18n_lang();
    // this language's script first (its glyphs: kanji in Japanese, hanzi in Chinese), then the others (the language
    // names in the Language row); a glyph comes from the first font that has it
    const FontPick *order[5];
    int n = 0;
    const FontPick *own = lang == L_JA ? k_japanese : lang == L_KO ? k_korean : lang == L_ZH_HANS ? k_hans
                        : lang == L_ZH_HANT ? k_hant : lang == L_RU ? k_cyrillic : NULL;
    if (own) order[n++] = own;
    for (const FontPick *f : {k_cyrillic, k_japanese, k_hans, k_hant, k_korean})
        if (f != own) order[n++] = f;
    for (int i = 0; i < n; i++) {
        FontFile ff;
        int face;
        if (!pick(order[i], &ff, &face)) continue;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.FontDataOwnedByAtlas = false;  // (mapped: font_file)
        cfg.FontNo = face;
        cfg.OversampleH = 1;  // (thousands of glyphs: keep the atlas small)
        cfg.PixelSnapH = true;
        // a script's own font a little smaller than its size: CJK fonts fill their em, Baloo 2 doesn't (Cyrillic's
        // letters are Latin-sized: nearly the same size, on the same baseline)
        float r = order[i] == k_cyrillic ? 1 - (1 - rel) * 0.4f : rel;
        cfg.GlyphOffset.y = order[i] == k_cyrillic ? 0 : size * (1 - r) * 0.35f;
        atlas->AddFontFromMemoryTTF(ff.data, ff.size, size * r, &cfg, i18n_glyph_ranges());
    }
}

bool i18n_fonts_stale(ImFontAtlas *atlas) {
    static std::unordered_map<ImFontAtlas *, int> built;
    int lang = i18n_lang();
    auto it = built.find(atlas);
    if (it != built.end() && it->second == lang) return false;
    built[atlas] = lang;
    return true;
}
