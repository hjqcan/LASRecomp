// Shared address calculation for shader keys and Vulkan descriptor writes.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Vulkan storage alignment is a power of two. Keep word addressing aligned too.
static inline uint32_t gmem_segment_words(uint64_t heap, uint64_t limit, uint64_t alignment) {
    if (heap <= limit) return 0;
    if (alignment < 4) alignment = 4;
    uint64_t span = limit / alignment * alignment;
    return span >= 4 && span / 4 <= UINT32_MAX ? (uint32_t)(span / 4) : UINT32_MAX;
}

static inline bool gmem_segment_range(uint64_t size, uint32_t words, uint32_t segment,
                                      uint64_t *offset, uint64_t *range) {
    uint64_t span = words ? (uint64_t)words * 4 : size;
    *offset = span * segment;
    if (!span || *offset >= size) { *range = 0; return false; }
    *range = size - *offset;
    if (*range > span) *range = span;
    return true;
}
