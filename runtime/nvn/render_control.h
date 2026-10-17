#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Conditional rendering compares the 64-bit values of two consecutive
// NVNcounterData records, skipping each record's timestamp.
static inline bool nvn_render_condition(uint32_t mode, const void *data) {
    uint64_t first, second;
    memcpy(&first, data, 8);
    memcpy(&second, (const uint8_t *)data + 16, 8);
    return mode == 0 ? first == second : first != second;
}

static inline void nvn_report_value(void *data, uint32_t value, uint64_t timestamp) {
    uint64_t record[2] = {value, timestamp};
    memcpy(data, record, sizeof record);
}

static inline bool nvn_command_needs_render(uint32_t op) {
    return op == CMD_CLEAR_COLOR || op == CMD_CLEAR_DEPTH_STENCIL ||
           op == CMD_DRAW_ARRAYS || op == CMD_DRAW_ELEMENTS ||
           op == CMD_DRAW_ARRAYS_INDIRECT || op == CMD_DRAW_ELEMENTS_INDIRECT || op == CMD_DRAW_TEXTURE;
}
