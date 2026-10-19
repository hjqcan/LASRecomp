// Local-only text checking. The proprietary NgWord2 system archive is not
// distributed with this runtime. This small fallback is deliberately identified
// as version 0; it does not claim Nintendo dictionary or online-policy parity.
#include <ctype.h>
#include <string.h>
#include "../hle.h"

#define NGC_UNINITIALIZED 0x292u
#define NGC_ALREADY_INITIALIZED 0x492u
#define NGC_NULL 0x692u
#define NGC_SIZE 0x892u
#define NGC_TEXT_LIMIT 2048
static uint8_t *ngc_object(Cpu *c) { return (uint8_t *)(uintptr_t)c->x[0]; }

static void initialize(Cpu *c) {
    uint8_t *o = ngc_object(c);
    if (!o || !c->x[1]) { c->x[0] = NGC_NULL; return; }
    if (c->x[2] < 0x20000) { c->x[0] = NGC_SIZE; return; }
    if (o[9]) { c->x[0] = NGC_ALREADY_INITIALIZED; return; }
    o[8] = !!c->x[3]; o[9] = 1;
    memset(o + 12, 0, 32);
    LOG("[ngc] offline basic text filter; Nintendo system dictionary is not installed\n");
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", ngc_init) { initialize(c); }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", ngc_init_default) { c->x[3] = 1; initialize(c); }
static void finalize(Cpu *c) {
    uint8_t *o = ngc_object(c);
    if (o) { o[9] = 0; memset(o + 12, 0, 32); }
}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter8FinalizeEv", ngc_finalize) { finalize(c); }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilterD1Ev", ngc_destroy_1) { finalize(c); }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilterD2Ev", ngc_destroy_2) { finalize(c); }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", ngc_version) { c->x[0] = 0; }

static unsigned text_char(const void *text, size_t i, bool wide) {
    return wide ? ((const uint16_t *)text)[i] : ((const uint8_t *)text)[i];
}
static bool letter(unsigned ch) { return ch < 128 && isalpha((unsigned char)ch); }
static unsigned folded(unsigned ch) { return ch < 128 ? (unsigned)tolower((unsigned char)ch) : ch; }
static void mask_char(void *text, size_t i, bool wide) {
    if (wide) ((uint16_t *)text)[i] = '*'; else ((char *)text)[i] = '*';
}
static int scan_text(void *text, bool wide, bool mask, bool at_sign) {
    static const char *const words[] = {"fuck", "shit", "bitch", "asshole"};
    size_t length = 0;
    while (length < NGC_TEXT_LIMIT && text_char(text, length, wide)) length++;
    if (length == NGC_TEXT_LIMIT) return -1;
    int count = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned ch = text_char(text, i, wide);
        if (at_sign && ch == '@') { count++; if (mask) mask_char(text, i, wide); continue; }
        if (i && letter(text_char(text, i - 1, wide))) continue;
        for (size_t w = 0; w < sizeof words / sizeof *words; w++) {
            size_t n = strlen(words[w]);
            if (n > length - i || (i + n < length && letter(text_char(text, i+n, wide)))) continue;
            size_t j = 0;
            while (j < n && folded(text_char(text, i+j, wide)) == (unsigned char)words[w][j]) j++;
            if (j != n) continue;
            count++;
            if (mask) for (j = 0; j < n; j++) mask_char(text, i+j, wide);
            i += n - 1; break;
        }
    }
    return count;
}
static void check_words(Cpu *c, bool wide) {
    uint8_t *o = ngc_object(c);
    if (!o || !o[9]) { c->x[0] = NGC_UNINITIALIZED; return; }
    uint32_t *out = (void *)(uintptr_t)c->x[1];
    const uint64_t *words = (void *)(uintptr_t)c->x[3];
    size_t count = c->x[4];
    if (!out || !words) { c->x[0] = NGC_NULL; return; }
    if (count > 16) { c->x[0] = NGC_SIZE; return; }
    uint32_t mask = (uint32_t)c->x[2] & 0xffff;
    memset(out, 0, count * sizeof *out);
    for (size_t i = 0; i < count; i++) {
        if (!words[i]) { c->x[0] = NGC_NULL; return; }
        int n = scan_text((void *)(uintptr_t)words[i], wide, false, !*(uint32_t *)(o+12));
        if (n < 0) { c->x[0] = NGC_SIZE; return; }
        if (n) out[i] = mask ? mask : 1;
    }
    c->x[0] = 0;
}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", ngc_check_utf8) { check_words(c, false); }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKtm", ngc_check_u16) { check_words(c, true); }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", ngc_check_utf16) { check_words(c, true); }
static void mask_text(Cpu *c, bool wide) {
    uint8_t *o = ngc_object(c);
    if (!o || !o[9]) { c->x[0] = NGC_UNINITIALIZED; return; }
    int *out = (void *)(uintptr_t)c->x[1];
    void *text = (void *)(uintptr_t)c->x[2];
    if (!out || !text) { c->x[0] = NGC_NULL; return; }
    int count = scan_text(text, wide, true, !*(uint32_t *)(o+12));
    *out = count < 0 ? 0 : count; c->x[0] = count < 0 ? NGC_SIZE : 0;
}
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", ngc_mask_utf8) { mask_text(c, false); }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", ngc_mask_u16) { mask_text(c, true); }
HLE_FUNC("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", ngc_mask_utf16) { mask_text(c, true); }
