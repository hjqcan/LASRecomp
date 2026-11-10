// The hookshot's 8-way aim (setting hookshot_8way, runtime/hle/hookshot.c), apart so tools/test_hookshot_snap.c can run it.
#pragma once
#include <math.h>

// Turns the flat direction (x, z) to the nearest of the 8 directions, keeping its length. A zero vector stays as it is.
static inline void hookshot_snap_8way(float *x, float *z) {
    double len = hypot(*x, *z);
    if (len == 0) return;
    int oct = (int)floor(atan2(*x, *z) / (M_PI / 4) + 0.5);
    static const double dirs[8][2] = {{0, 1}, {M_SQRT1_2, M_SQRT1_2}, {1, 0}, {M_SQRT1_2, -M_SQRT1_2},
                                      {0, -1}, {-M_SQRT1_2, -M_SQRT1_2}, {-1, 0}, {-M_SQRT1_2, M_SQRT1_2}};
    *x = (float)(dirs[oct & 7][0] * len);
    *z = (float)(dirs[oct & 7][1] * len);
}

// The rotation about the vertical axis that faces the flat direction (x, z), as the y and w of the game's quaternion
// (x, y, z, w; facing along +z at no rotation). The direction need not be unit length.
static inline void hookshot_yaw_quat(float x, float z, float *qy, float *qw) {
    double half = atan2(x, z) / 2;
    *qy = (float)sin(half);
    *qw = (float)cos(half);
}
