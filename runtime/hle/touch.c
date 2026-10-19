// Docked host input has no touchscreen contacts. Always publish a valid empty
// sample: an unpopulated HID shared-memory ring leaves the caller's old stack
// contents unchanged, and games can inspect the newest sample before its count.
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include "../hle.h"

typedef struct {
    uint64_t delta_time;
    uint32_t attributes, finger_id, x, y, diameter_x, diameter_y, rotation, reserved;
} TouchPoint;
typedef struct {
    uint64_t sampling;
    int32_t count;
    uint32_t reserved;
    TouchPoint touches[16];
} TouchSample;
_Static_assert(sizeof(TouchPoint) == 0x28 && sizeof(TouchSample) == 0x290, "touchscreen ABI");
_Static_assert(offsetof(TouchSample, count) == 8, "touch count ABI");

static _Atomic uint64_t touch_sampling;
RT_ON_RESET(touch_reset) { atomic_store(&touch_sampling, 0); }

static void empty_touch_sample(uint64_t address) {
    TouchSample sample = {0};
    sample.sampling = atomic_fetch_add(&touch_sampling, 1) + 1;
    memcpy((void *)(uintptr_t)address, &sample, sizeof sample);
}

HLE_FUNC("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", touch_states) {
    uint64_t address = c->x[0];
    int32_t capacity = (int32_t)c->x[1];
    c->x[0] = 0;
    if (!address || capacity <= 0) return;
    empty_touch_sample(address);
    c->x[0] = 1;
}

HLE_FUNC("_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE", touch_state) {
    if (c->x[0]) empty_touch_sample(c->x[0]);
}
