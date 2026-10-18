// ELF64 GNU hash tables do not store a symbol count directly.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t elf_hash_u32(const uint8_t *image, uint64_t at) {
    uint32_t value;
    memcpy(&value, image + at, sizeof value);
    return value;
}

static inline bool gnu_hash_symbol_count(const uint8_t *image, uint64_t size,
                                         uint64_t table, uint64_t max_symbols,
                                         uint64_t *count) {
    if (table % 4 || table > size || size - table < 16) return false;
    uint32_t buckets = elf_hash_u32(image, table);
    uint32_t first = elf_hash_u32(image, table + 4);
    uint32_t words = elf_hash_u32(image, table + 8);
    if (!buckets || !words || (words & (words - 1)) || first > max_symbols) return false;
    uint64_t prefix = (uint64_t)words * 8 + (uint64_t)buckets * 4;
    if (prefix > size - table - 16) return false;
    uint64_t bucket_at = table + 16 + (uint64_t)words * 8;
    uint64_t chain_at = table + 16 + prefix;
    uint32_t last = 0;
    for (uint32_t i = 0; i < buckets; i++) {
        uint32_t index = elf_hash_u32(image, bucket_at + (uint64_t)i * 4);
        if (index && (index < first || index >= max_symbols)) return false;
        if (index > last) last = index;
    }
    if (!last) { *count = first; return true; }
    // Symbols are grouped by bucket. Only the chain starting at the largest
    // symbol index can contain the final symbol; scanning it keeps work linear.
    for (uint64_t index = last; index < max_symbols; index++) {
        uint64_t delta = (index - first) * 4;
        if (delta > size - chain_at || size - chain_at - delta < 4) return false;
        if (elf_hash_u32(image, chain_at + delta) & 1) {
            *count = index + 1;
            return true;
        }
    }
    return false;
}
