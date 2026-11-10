// The hookshot's 8-way aim (runtime/hle/hookshot_snap.h): no game, no controller.
// Build + run: clang -std=gnu11 -O1 tools/test_hookshot_snap.c -lm -o build/test-hookshot-snap && build/test-hookshot-snap
#include <math.h>
#include <stdio.h>

#include "../runtime/hle/hookshot_snap.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double deg_of(float x, float z) { return atan2(x, z) * 180 / M_PI; }

int main(void) {
    // every whole and half degree: lands on a multiple of 45, at most 22.5 away, length kept
    for (int h = -720; h <= 720; h++) {
        double d = h / 2.0, r = d * M_PI / 180;
        float x = (float)(sin(r) * 2.5), z = (float)(cos(r) * 2.5);
        hookshot_snap_8way(&x, &z);
        double out = deg_of(x, z), m = fmod(fabs(out) + 1e-3, 45);
        CHECK(m < 2e-3, "%.1f deg -> %.4f is not a multiple of 45", d, out);
        double off = fmod(fabs(out - d), 360);
        if (off > 180) off = 360 - off;
        CHECK(off <= 22.5 + 1e-3, "%.1f deg moved %.4f", d, off);
        CHECK(fabs(hypot(x, z) - 2.5) < 1e-4, "%.1f deg: length %f", d, hypot(x, z));
    }
    // the issue's case: slightly off a straight line goes back onto it, exactly
    float x = (float)sin(3 * M_PI / 180), z = -(float)cos(3 * M_PI / 180);
    hookshot_snap_8way(&x, &z);
    CHECK(x == 0 && z == -1, "3 degrees off south: %f, %f", x, z);
    // already on one of the 8: unchanged
    x = 1, z = 0;
    hookshot_snap_8way(&x, &z);
    CHECK(x == 1 && z == 0, "east: %f, %f", x, z);
    x = 0, z = 0;
    hookshot_snap_8way(&x, &z);
    CHECK(x == 0 && z == 0, "zero: %f, %f", x, z);
    // the game's own numbers (logged in play): facing (0.940, -0.342) is the rotation y 0.819, w 0.574
    float qy, qw;
    hookshot_yaw_quat(0.940f, -0.342f, &qy, &qw);
    CHECK(fabs(qy - 0.819) < 1e-3 && fabs(qw - 0.574) < 1e-3, "yaw: %f, %f", qy, qw);
    hookshot_yaw_quat(-1, 0, &qy, &qw);
    CHECK(fabs(fabs(qy) - M_SQRT1_2) < 1e-6 && fabs(fabs(qw) - M_SQRT1_2) < 1e-6, "west: %f, %f", qy, qw);
    if (!fails) printf("hookshot snap: ok\n");
    return fails != 0;
}
