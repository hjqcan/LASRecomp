#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    size_t count, over_16ms, over_33ms;
    double mean_ms, median_ms, p95_ms, p99_ms, max_ms, fps, low_1pct_fps;
} FrameStats;
bool frame_stats(const double *ms, size_t count, FrameStats *out);
void performance_frame(uint64_t frame);
void performance_reset(void);
void performance_draw(float scale, bool in_game);
#ifdef __cplusplus
}
#endif
