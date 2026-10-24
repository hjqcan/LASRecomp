// Slow-frame log (slowframe.c): every presented frame that took longer than LARECOMP_SLOW_FRAME_MS (default 50; 0 =
// off) from the previous present gets one "[slow]" line in the log saying what the renderer did in that time:
//   [slow] frame 4123 83 ms in Field: 3 pipelines built (41 ms, 1 from cache), 2 programs compiled (12 ms),
//          2 shaders translated (3 ms), 12 texture uploads 4.1 MB, 5 full texture hashes, 9 submissions,
//          gpu wait 2 ms, render cpu 18 ms, game cpu 11 ms
// At most 20 lines a minute; the slow frames past that, and those during loads (not in play, or within 2 s of a
// level change), are only counted, in one summary line per minute. Normal frames cost a few counter reads.
//
// The counts are what happened between the two presents, on any thread: builds finish on worker threads, the render
// thread translates the game's command lists, the game thread presents. Builds are counted (and timed) when they
// finish; "from cache": pipelines the driver's cache (Vulkan) or the shader archive (Metal) had.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "maxwell/shader.h"

// (the backends, any thread) events, counted
void slow_note_build(const char *what, uint64_t ns);  // what: "pipeline ..." (graphics / compute pipeline), else a
                                                      // program's shaders compiled (SPIR-V modules / MSL libraries)
void slow_note_cache_hit(void);                       // a pipeline build the driver's / archive's cache served
void slow_note_upload(uint64_t bytes);                // a texture's contents uploaded (guest bytes)
void slow_note_full_hash(void);                       // a texture's contents hashed in full
// maxwell_translate, counted and timed (the backends call this instead)
ShaderInfo *slow_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);

// (each backend) its own running totals: GPU submissions, time any thread waited for the GPU (ns), guest heap ranges
// imported as GPU buffers (Vulkan's lazy heap; Metal shares the heap once at start: 0)
void renderer_slow_counters(uint64_t *submits, uint64_t *gpu_wait_ns, uint64_t *imports);

// (pace.c) once per presented frame on the presenting (game) thread, with the present's time and that thread's CPU
// time, both read there already
void slow_frame_present(uint64_t now_ns, uint64_t thread_cpu_ns);
