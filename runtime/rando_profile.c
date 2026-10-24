#include "rando_profile.h"
#include "rando_patch_words.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool rando_seed_name_valid(const char *name) {
    if (!name || !*name || name[0] == '.' || strlen(name) > 200) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 32 || strchr("/\\:<>\"|?*", *p)) return false;
    return true;
}

const char *rando_patch_filename(GameLayout version) {
    return version == GAME_110 ? "1.0.1.pchtxt" : "1.0.0.pchtxt";
}

bool rando_seed_compatible(const char *directory, GameLayout version) {
    if (version != GAME_100 && version != GAME_110) return false;
    char path[1200], data[128] = {0};
    if (snprintf(path, sizeof path, "%s/game-version.txt", directory) >= (int)sizeof path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT && version == GAME_100; // old seeds are base-version seeds
    size_t n = fread(data, 1, sizeof data - 1, f);
    bool ok = !ferror(f) && n < sizeof data - 1;
    fclose(f);
    // Accept LF and Windows CRLF without accepting extra fields or unknown versions.
    char normalized[128]; size_t k = 0;
    for (size_t i = 0; i < n; ++i) if (data[i] != '\r') normalized[k++] = data[i];
    normalized[k] = 0;
    return ok && !strcmp(normalized, version == GAME_100 ? "LASRANDO1\n1.0.0\n" : "LASRANDO1\n1.1.0\n");
}

static bool fail(char *why, size_t cap, unsigned line, const char *message) {
    if (cap) snprintf(why, cap, "Randomizer patch line %u: %s", line, message);
    return false;
}
static bool allowed(uint32_t offset, uint32_t word) {
    for (size_t i = 0; i < sizeof rando_allowed_words / sizeof *rando_allowed_words; ++i)
        if (rando_allowed_words[i].offset == offset && rando_allowed_words[i].word == word) return true;
    return false;
}
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool rando_patch_read(const char *path, GameLayout version, RandoPatchSet *out, char *why, size_t cap) {
    memset(out, 0, sizeof *out);
    if (version != GAME_100 && version != GAME_110) return fail(why, cap, 0, "unsupported executable");
    FILE *f = fopen(path, "rb");
    if (!f) return fail(why, cap, 0, "cannot read patch file");
    RandoPatchSet parsed = {0};
    char line[1024]; unsigned number = 0, total = 0;
    bool enabled = false, header = false, ok = true;
    while (fgets(line, sizeof line, f)) {
        ++number; total += (unsigned)strlen(line);
        if (total > 65536 || (!strchr(line, '\n') && !feof(f))) { ok = fail(why, cap, number, "file or line is too long"); break; }
        char *p = line; while (isspace((unsigned char)*p)) ++p;
        p[strcspn(p, "\r\n")] = 0;
        size_t len = strlen(p); while (len && isspace((unsigned char)p[len-1])) p[--len] = 0;
        if (!*p || *p == '#' || !strncmp(p, "//", 2)) continue;
        if (!strncmp(p, "@nsobid-", 8)) {
            char id[65] = {0}; size_t n = strlen(p + 8);
            if (header || (n != 40 && n != 64)) { ok = fail(why, cap, number, "invalid Build ID"); break; }
            for (size_t i = 0; i < n; ++i) id[i] = (char)tolower((unsigned char)p[8+i]);
            if (n == 64 && strspn(id + 40, "0") != 24) { ok = fail(why, cap, number, "unexpected Build ID suffix"); break; }
            id[40] = 0;
            const char *expected = version == GAME_100 ? "ae16f71e002af8cb059a9a74c4d90f34ba984892" : "909e904af78ac1b8deefe97ab2ccdb51968f0ec7";
            if (strcmp(id, expected)) { ok = fail(why, cap, number, "patch belongs to another executable"); break; }
            header = true; continue;
        }
        if (!strcmp(p, "@flag offset_shift 0x100")) continue; // offsets are image-relative, as emitted by LAS
        if (!strcmp(p, "@enabled")) { enabled = true; continue; }
        if (!strcmp(p, "@stop") || !strcmp(p, "@disabled")) { enabled = false; continue; }
        if (*p == '@') { ok = fail(why, cap, number, "unsupported directive"); break; }
        if (!enabled) continue;
        if (!header) { ok = fail(why, cap, number, "missing Build ID"); break; }
        char *end; unsigned long offset = strtoul(p, &end, 16);
        if (end-p != 8 || !isspace((unsigned char)*end) || offset > 0xfffffffful) { ok = fail(why, cap, number, "invalid offset"); break; }
        p = end; while (isspace((unsigned char)*p)) ++p;
        unsigned char bytes[32]; size_t n = 0;
        if (*p == '"') {
            if (offset != 0x16cbd73 || strcmp(p, "\"NoTiltShift\"")) { ok = fail(why, cap, number, "unsupported string patch"); break; }
            memcpy(bytes, "NoTiltShift", sizeof "NoTiltShift"); n = sizeof "NoTiltShift";
        } else {
            size_t chars = strlen(p);
            if (!chars || chars > sizeof bytes * 2 || chars % 2) { ok = fail(why, cap, number, "invalid patch bytes"); break; }
            for (size_t i = 0; i < chars; i += 2) {
                int a = hex_digit(p[i]), b = hex_digit(p[i+1]);
                if (a < 0 || b < 0) { ok = fail(why, cap, number, "invalid hex byte"); break; }
                bytes[n++] = (unsigned char)(a * 16 + b);
            }
            if (!ok) break;
        }
        if (offset == 0x16cbd73 || offset == 0x16fae60) {
            if (parsed.data_count == 2 || (offset == 0x16fae60 && (n != 1 || bytes[0] != 3)) ||
                (offset == 0x16cbd73 && (n != sizeof "NoTiltShift" || memcmp(bytes, "NoTiltShift", n)))) {
                ok = fail(why, cap, number, "unsupported data patch"); break;
            }
            for (int i = 0; i < parsed.data_count; ++i) if (parsed.data[i].offset == offset) ok = false;
            if (!ok) { fail(why, cap, number, "duplicate data patch"); break; }
            RandoDataPatch *d = &parsed.data[parsed.data_count++];
            d->offset = (uint32_t)offset; d->size = (uint32_t)n; memcpy(d->bytes, bytes, n);
        } else {
            if (n % 4 || offset % 4) { ok = fail(why, cap, number, "unaligned code patch"); break; }
            for (size_t i = 0; i < n; i += 4) {
                uint32_t word = (uint32_t)bytes[i] | (uint32_t)bytes[i+1] << 8 | (uint32_t)bytes[i+2] << 16 | (uint32_t)bytes[i+3] << 24;
                uint32_t at = (uint32_t)offset + (uint32_t)i;
                if (!allowed(at, word) || parsed.code_count == 64) { ok = fail(why, cap, number, "uncompiled code patch"); break; }
                for (int j = 0; j < parsed.code_count; ++j) if (parsed.code[j].offset == at) ok = false;
                if (!ok) { fail(why, cap, number, "duplicate code patch"); break; }
                parsed.code[parsed.code_count++] = (RandoCodePatch){at, word};
            }
            if (!ok) break;
        }
    }
    if (ferror(f)) ok = fail(why, cap, number, "read failed");
    fclose(f);
    if (ok && (!header || !parsed.code_count)) ok = fail(why, cap, number, "missing patch content");
    if (ok) *out = parsed;
    return ok;
}
