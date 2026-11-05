// Frame interpolation (runtime/nvn/interp.c): images between the game's own frames, made by running the frame's
// command lists again with the per-draw uniforms blended between the previous frame and this one.
// Video tab "Frame interpolation" (off by default); LARECOMP_INTERP overrides it (tests).
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { INTERP_OFF = 0, INTERP_MEASURE = 1, INTERP_ON = 2 };
// What's wanted now (the setting, or LARECOMP_INTERP): the backend's present asks once per game frame.
int interp_wanted(void);

// The executor's hooks (renderer.m). g_interp_log: the frame being executed is logged (draws + uniform copies);
// g_interp_replaying: a replay runs (skip side effects, read the logged / blended uniforms).
extern bool g_interp_log, g_interp_replaying;
bool interp_list_submitted(uint64_t handle);  // true for the frame's first list (the backend saves its state then)
void interp_draw(uint64_t key, bool depth_only);  // depth_only: no colour target (a shadow pass)
const void *interp_cbuf(int stage, int k, int slot, const void *src, uint64_t size, uint64_t used);  // used: bytes the shader reads
// The tape (renderer.m: an in-between plays the frame's recorded Metal calls back): the copy interp_cbuf logged last
// (-1: none), and a logged copy's blended bytes for the image being made (NULL: the frame's own are shown)
int32_t interp_last_cb(void);
const void *interp_tape_cb(int32_t i);
// The texture-pool entry a draw's texture i resolves to: logged with the frame (a replay takes the logged one - the
// pool is the game's memory and it rewrites entries while the replays run; an area's lighting texture swapped under
// an in-between showed it with the next area's ambient light for one image).
const void *interp_texentry(int stage, int i, const void *entry, uint32_t size);
// Storage a draw's shader reads straight from guest memory (gmem): logged with the frame so a replay can tell when the
// game has rewritten it since (a replay reads it live; Dusklight's presented frame reads only its own recording), and
// a dispatch's output ranges, which the GPU rewrites in order and need no check.
void interp_storage(uint64_t addr, uint64_t size);
void interp_dispatch_output(uint64_t addr, uint64_t size);
void interp_texture_destroyed(void);  // (render thread, in queue order) a texture went away during the frame

// Objects freed at once by the game's threads (programs, memory pools, samplers): a replay must not run while one goes,
// nor use a frame that one went from after its lists were run.
void interp_free_begin(int kind, uint64_t size);  // kind: INTERP_FREE_* (stats)
enum { INTERP_FREE_POOL, INTERP_FREE_SAMPLER, INTERP_FREE_PROGRAM, INTERP_FREE_KINDS };
void interp_free_end(void);

// The present (render thread), once per game frame: the frame's draws are matched against the previous frame's.
// Returns whether in-between images can be made (false: a cut - level change, warp, camera cut, too few matches).
// log_next: whether the next frame is logged (the setting can change at any frame).
bool interp_frame_end(bool log_next);
extern uint64_t g_interp_present_frame;  // (the backend: the frame number of the present being made, for the research hooks)
void interp_force_cut(void);              // the next frame is a cut (the backend saw a level change, a resize...)
// Before the in-betweens of a frame: false when there must be none - the game thread is already two frames past it
// (its parity-double-buffered memory is being rewritten under the replays) or the render thread reached its present
// over 100 ms late (a load, a hitch: Dusklight's epochs).
bool interp_replays_allowed(uint64_t frame, uint64_t game_frame, uint64_t now);
bool interp_prepare(float t);             // blends for an image at t (0 = previous frame, 1 = this one)
bool interp_replay_lock(void);            // before replays: false when an object the frame used was freed
void interp_replay_unlock(void);
uint32_t interp_handles(const uint64_t **out);
void interp_replay_begin(void);
void interp_replay_end(void);
void interp_frame_rotate(void);           // after the present: this frame becomes the previous one
uint64_t interp_frame_serial(void);       // counts interp_frame_rotate: which frame the replays' logs belong to
uint64_t interp_replay_salt(void);        // (renderer_exec_frame) a different "frame" for each replay

// The game thread at each present: the emulated vsync this frame belongs to (its time on the game's timeline).
void interp_stamp(uint64_t frame, uint64_t vsync_ns, uint64_t call_ns);
void interp_latency_lag(uint64_t frame, uint64_t now);  // (render thread) it reached that frame's present
void interp_latency_shown(uint64_t frame, double presented_s, bool interp);  // (presented handlers) its image is on screen
uint64_t interp_stamp_of(uint64_t frame);

// When to show what: the images for the game interval (s0, s1], placed on the display's vsync grid (phase + k * period)
// a fixed latency behind the game's timeline (adapted so they're rendered in time). t >= 1: the game's own image.
typedef struct { uint64_t at; float t; } InterpShot;
// lead: how long before its vsync an image has to be ready (the presenter's head start).
int interp_schedule(uint64_t s0, uint64_t s1, uint64_t now, uint64_t phase, uint64_t period, uint64_t lead, bool blend,
                    InterpShot *out, int max);

// Stats (any thread) and the pacer: the render thread's time in replays (rqueue.c's waits take it out of busy time).
void interp_replay_gpu(double ms);
void interp_compose_gpu(double ms);
double interp_image_gpu_ms(void);  // an in-between's GPU time, replay and compose (moving average; -1: none yet)
uint64_t interp_replay_ns(uint64_t now);
void interp_log_stats(void);  // (render thread, per game frame) a line every LARECOMP_INTERP_LOG seconds (default 2)
void interp_note_skipped(void);  // (stats) an in-between not made: no free image slot
void interp_note_given_way(void);  // (stats) ... a game thread was waiting for the render thread
void interp_note_no_gpu(void);     // (stats) ... no GPU time left for it in the game's frame
void interp_note_images(int mids);  // (stats, Vulkan's inline presents) a frame's image and its mids in-betweens shown
