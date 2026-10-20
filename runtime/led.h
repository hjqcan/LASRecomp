// "Controller light" (Controls tab; idea from Dusklight's "Enable LED status"): the light bar of a controller that
// has one (DualSense, DualShock 4, ...) shows Link's health. Pure colour mapping here (tools/test_led.c checks it);
// runtime/hle/input.c reads the health and drives SDL_SetGamepadLED.
//
// Health in quarter-hearts, as the save keeps it. Above one heart: green at full, through yellow to red near one
// heart. One heart or less (the game's low-health beep): a slow red pulse. Damage: a short white flash that fades
// back into the health colour.
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint8_t r, g, b; } LedColor;

#define LED_LOW_HP 4            // one heart: the low-health beep
#define LED_PULSE_MS 1600       // one slow pulse
#define LED_FLASH_MS 300        // the damage flash, fading out
#define LED_MIN_INTERVAL_MS 33  // at most one SDL update every two frames

static inline bool led_equal(LedColor a, LedColor b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

static inline LedColor led_mix(LedColor a, LedColor b, float t) {  // t = 0: a, 1: b
    if (!(t > 0)) return a;
    if (t >= 1) return b;
    LedColor c = {(uint8_t)(a.r + (b.r - a.r) * t + 0.5f), (uint8_t)(a.g + (b.g - a.g) * t + 0.5f),
                  (uint8_t)(a.b + (b.b - a.b) * t + 0.5f)};
    return c;
}

// now_ms: a running clock (the pulse's phase); since_hurt_ms: time since Link last lost health (large: none recently).
static inline LedColor led_health_color(int hp, int max, uint64_t now_ms, uint64_t since_hurt_ms) {
    static const LedColor green = {0, 160, 0}, yellow = {150, 120, 0}, red = {170, 0, 0}, white = {200, 200, 200};
    LedColor c;
    if (hp < 0) hp = 0;
    if (max < hp) max = hp;
    if (hp <= LED_LOW_HP) {
        // Raised cosine, 20..170 red, in steps of 10 (fewer distinct colours = fewer SDL calls).
        float ph = (float)(now_ms % LED_PULSE_MS) / LED_PULSE_MS;
        float level = 0.5f - 0.5f * cosf(ph * 6.2831853f);
        c.r = (uint8_t)(20 + 10 * (int)(level * 15 + 0.5f)), c.g = 0, c.b = 0;
    } else {
        float f = max > LED_LOW_HP ? (float)(hp - LED_LOW_HP) / (float)(max - LED_LOW_HP) : 1;  // 0 at one heart, 1 full
        c = f >= 0.5f ? led_mix(yellow, green, (f - 0.5f) * 2) : led_mix(red, yellow, f * 2);
    }
    if (since_hurt_ms < LED_FLASH_MS) {
        // White, fading back in four steps.
        int step = (int)(since_hurt_ms * 4 / LED_FLASH_MS);
        c = led_mix(c, white, 1 - step / 4.0f);
    }
    return c;
}

// The colour SDL gives the light itself before an app sets one (its HIDAPI PS4 / PS5 drivers: by player index, the
// same list as Linux's hid-sony). There is no SDL call that hands the light back, so this is what we restore.
static inline LedColor led_default_color(bool is_ps4, int player_index) {
    static const LedColor ps5[7] = {{0, 0, 0x40}, {0x40, 0, 0}, {0, 0x40, 0}, {0x20, 0, 0x20},
                                    {0x20, 0x10, 0}, {0, 0x10, 0x10}, {0x10, 0x10, 0x10}};
    static const LedColor ps4[7] = {{0, 0, 0x40}, {0x40, 0, 0}, {0, 0x40, 0}, {0x20, 0, 0x20},
                                    {0x02, 0x01, 0}, {0, 0x01, 0x01}, {0x01, 0x01, 0x01}};
    int i = player_index >= 0 ? player_index % 7 : 0;
    return is_ps4 ? ps4[i] : ps5[i];
}
