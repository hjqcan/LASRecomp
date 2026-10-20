// Controller light colours (runtime/led.h): no game, no controller.
// Build + run: clang -std=gnu11 -O1 tools/test_led.c -lm -o build/test-led && build/test-led (-v: print the scale)
#include <stdio.h>
#include <string.h>

#include "../runtime/led.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NO_HURT UINT64_MAX

int main(int argc, char **argv) {
    bool verbose = argc > 1 && !strcmp(argv[1], "-v");
    int maxes[] = {12, 20, 40, 80};  // 3 hearts (new file) .. 20 hearts
    for (unsigned m = 0; m < sizeof maxes / sizeof *maxes; m++) {
        int max = maxes[m];
        LedColor full = led_health_color(max, max, 0, NO_HURT);
        CHECK(full.g > 100 && full.r == 0 && full.b == 0, "full health %d is green: %d,%d,%d", max, full.r, full.g, full.b);
        LedColor prev = full;
        for (int hp = max; hp > LED_LOW_HP; hp--) {  // green -> yellow -> red: red never falls, green never rises
            LedColor c = led_health_color(hp, max, 0, NO_HURT);
            CHECK(c.r >= prev.r && c.g <= prev.g && c.b == 0, "monotonic at %d/%d", hp, max);
            CHECK(c.r + c.g > 100, "visible at %d/%d", hp, max);
            if (verbose && m == 1) printf("%2d/%d: %3d,%3d,%3d\n", hp, max, c.r, c.g, c.b);
            prev = c;
        }
        LedColor near = led_health_color(LED_LOW_HP + 1, max, 0, NO_HURT);
        CHECK(near.r > near.g, "just above one heart is mostly red at max %d: %d,%d", max, near.r, near.g);
        // Time does not change the colour above one heart.
        CHECK(led_equal(led_health_color(max / 2 + 2, max, 0, NO_HURT), led_health_color(max / 2 + 2, max, 777, NO_HURT)),
              "steady above one heart");
    }
    // One heart or less: a red pulse, dim to bright over LED_PULSE_MS, few distinct values.
    for (int hp = 0; hp <= LED_LOW_HP; hp++) {
        LedColor lo = led_health_color(hp, 12, 0, NO_HURT), hi = led_health_color(hp, 12, LED_PULSE_MS / 2, NO_HURT);
        CHECK(lo.g == 0 && lo.b == 0 && hi.g == 0 && hi.b == 0, "pulse is red at %d", hp);
        CHECK(lo.r <= 30 && hi.r >= 160, "pulse range at %d: %d..%d", hp, lo.r, hi.r);
        CHECK(led_equal(lo, led_health_color(hp, 12, LED_PULSE_MS, NO_HURT)), "pulse period");
    }
    int distinct = 0, last = -1;
    for (uint64_t t = 0; t < LED_PULSE_MS; t += LED_MIN_INTERVAL_MS) {
        LedColor c = led_health_color(4, 12, t, NO_HURT);
        if (c.r != last) distinct++, last = c.r;
        if (verbose) printf("pulse %4llu ms: %d\n", (unsigned long long)t, c.r);
    }
    CHECK(distinct <= 31, "pulse changes at most twice a step: %d", distinct);
    // Damage: white flash, fading back to the health colour by LED_FLASH_MS.
    LedColor base = led_health_color(8, 12, 0, NO_HURT);
    LedColor f0 = led_health_color(8, 12, 0, 0);
    CHECK(f0.r >= 190 && f0.g >= 190 && f0.b >= 190, "flash starts white: %d,%d,%d", f0.r, f0.g, f0.b);
    LedColor fm = led_health_color(8, 12, 0, LED_FLASH_MS / 2);
    CHECK(!led_equal(fm, f0) && !led_equal(fm, base), "flash fades");
    CHECK(led_equal(led_health_color(8, 12, 0, LED_FLASH_MS), base), "flash over");
    // Odd inputs stay sane.
    LedColor odd = led_health_color(-3, 0, 0, NO_HURT);
    CHECK(odd.g == 0 && odd.b == 0, "negative health pulses red");
    odd = led_health_color(30, 12, 0, NO_HURT);  // (more than max: full)
    CHECK(led_equal(odd, led_health_color(30, 30, 0, NO_HURT)), "over max");
    // Restored colours: SDL's player colours.
    CHECK(led_equal(led_default_color(false, 0), (LedColor){0, 0, 0x40}), "player 1 blue");
    CHECK(led_equal(led_default_color(false, -1), (LedColor){0, 0, 0x40}), "no player index: blue");
    CHECK(led_equal(led_default_color(false, 8), (LedColor){0x40, 0, 0}), "player 9 wraps to red");
    CHECK(led_equal(led_default_color(true, 4), (LedColor){0x02, 0x01, 0}), "PS4 table");
    printf(fails ? "test_led: %d failure(s)\n" : "test_led: ok\n", fails);
    return fails != 0;
}
