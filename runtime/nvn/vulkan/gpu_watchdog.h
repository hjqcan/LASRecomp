#pragma once
#include <stdint.h>

// Labels must have static lifetime. The monitor never calls into the driver.
unsigned vk_watchdog_begin(const char *operation, uint64_t sequence);
void vk_watchdog_end(unsigned guard);

#define VK_WATCHDOG(operation, sequence, statement) do { \
    unsigned guard_ = vk_watchdog_begin(operation, sequence); \
    statement; \
    vk_watchdog_end(guard_); \
} while (0)
