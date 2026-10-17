// ELF64 DT_RELR decoding (ELF gABI 6.2). All offsets refer to one loaded image.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline bool relr_site(uint8_t *image, uint64_t size, uint64_t base,
                             uint64_t table, uint64_t bytes, uint64_t at, bool apply) {
    if (size < 8 || at > size - 8) return false;
    // The decoder must not rewrite the table that it is still consuming.
    if (at < table + bytes && table < at + 8) return false;
    if (apply) {
        uint64_t value;
        memcpy(&value, image + at, 8);
        value += base;
        memcpy(image + at, &value, 8);
    }
    return true;
}

static inline bool relr_apply(uint8_t *image, uint64_t size, uint64_t base,
                              uint64_t table, uint64_t bytes, uint64_t entry_size) {
    if (entry_size != 8 || table % 8 || bytes % 8 || table > size || bytes > size - table)
        return false;
    // Validate every target first, so a malformed trailing bitmap cannot leave
    // a partially relocated module. Unaligned even target addresses are legal.
    for (int apply = 0; apply < 2; apply++) {
        uint64_t cursor = 0;
        bool have_address = false;
        for (uint64_t p = table; p < table + bytes; p += 8) {
            uint64_t entry;
            memcpy(&entry, image + p, 8);
            if (!(entry & 1)) {
                if (!relr_site(image, size, base, table, bytes, entry, apply != 0)) return false;
                cursor = entry + 8;
                have_address = true;
            } else {
                if (!have_address || cursor > UINT64_MAX - 63 * 8) return false;
                for (unsigned bit = 1; bit < 64; bit++)
                    if ((entry >> bit) & 1)
                        if (!relr_site(image, size, base, table, bytes, cursor + (bit - 1) * 8, apply != 0))
                            return false;
                cursor += 63 * 8;
            }
        }
    }
    return true;
}
