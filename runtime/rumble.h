// Approximate Switch HD Rumble with a conventional two-motor gamepad.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

typedef struct { float low_amp, low_hz, high_amp, high_hz; } RumbleValue;
_Static_assert(sizeof(RumbleValue) == 16, "HID vibration wire layout");

static inline uint16_t rumble_amplitude(float a) {
    if (!isfinite(a) || a <= 0) return 0;
    if (a >= 1) return 65535;
    return (uint16_t)(a * 65535.0f + 0.5f);
}

// Current HLE uses native HID handles: style | player<<8 | side<<16.
// Accept the older synthetic 0x03000000 | player<<8 | side for compatibility.
static inline int rumble_side(uint32_t h) {
    if ((h & ~1u) == 0x03000000u) return h & 1;
    if ((h & 0xff00ff00u) == 0 && (h & 255) >= 3 && (h & 255) <= 5 && ((h >> 16) & 255) <= 1)
        return (h >> 16) & 1;
    return -1;  // this runtime exposes player 1 only
}

// No frequency control exists in SDL_RumbleGamepad: low/high amplitudes map
// linearly to its large/small motors. Mix L/R devices by maximum, not sum,
// avoiding clipping when the game sends the same effect to both Joy-Cons.
static inline void rumble_mix(const RumbleValue v[2], uint16_t *lo, uint16_t *hi) {
    uint16_t a = rumble_amplitude(v[0].low_amp), b = rumble_amplitude(v[1].low_amp);
    *lo = a > b ? a : b;
    a = rumble_amplitude(v[0].high_amp); b = rumble_amplitude(v[1].high_amp);
    *hi = a > b ? a : b;
}

void input_rumble_send(uint32_t handle, const RumbleValue *value);
RumbleValue input_rumble_get(uint32_t handle);
void input_rumble_permit(bool permitted);
bool input_rumble_permitted(void);
void input_rumble_poll(void);  // UI thread, same owner as pad open/close
