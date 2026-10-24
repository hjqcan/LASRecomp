// Frame pacing shared by both backends (pace.c): the game's 30 fps mode when frames don't fit 60.
#pragma once
#include <stdbool.h>
#include <stdint.h>

void pace_frame(void);  // once per presented frame, on the presenting thread
// a pair's end counter (the 16 bytes after its start) is at least 20 ms after it while frames don't fit 60
uint64_t pace_end_counter(const uint64_t *start_entry, const uint64_t *entry, uint64_t value);
bool pace_is_slow(void);
uint64_t pace_frames_over(void);  // (stats) frames over 60 fps's budget since the last call
// any thread: a one-off stall (a load, a save commit, ...; why: for the log): the next second's frames don't count
// toward holding 30 (pace.c)
void pace_hitch(const char *why);
// any thread: a user setting that changes the GPU's load ("resolution", "upscaler", "anti-aliasing"; not Dynamic's own
// steps): the 30 fps hold and its backoff start over, and 60 gets a fresh try (pace.c)
void pace_settings_changed(const char *what);
