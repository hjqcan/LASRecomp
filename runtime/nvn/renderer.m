#include "../photo.h"
// Executes recorded NVN command lists with Metal.
#include <pthread.h>
#include <dirent.h>
#include <unistd.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include <sys/stat.h>
#include <SDL3/SDL_timer.h>

#include "maxwell/shader.h"
#include "nvn.h"
#include "render_control.h"
#include "renderer.h"
#include "pace.h"
#include "shader_prep.h"
#include "../title_profile.h"
#include "recolour.h"
#include "slowframe.h"

static id<MTLDevice> dev;
static id<MTLCommandBuffer> cmdbuf;
static id<MTLRenderCommandEncoder> enc;
static id<MTLRenderPipelineState> clear_pipes[64];
static NSMutableDictionary<NSString *, id> *cache;
// (stats) GPU wait time by cause: 0 = guest fence (nvnSyncWait), 1 = full flush (CPU reads GPU results / ring), 2 = sync all
static uint64_t stat_wait_by[3], stat_wait_n[3];
static uint64_t stat_clears_as_load, stat_clear_passes;  // (deferred clears)
static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
static uint64_t stat_vcopy, stat_vcopy_bytes;  // vertex / index ranges copied at translation (dyn_copy)
static double stat_gpu_s;
// GPU busy time (dynamic resolution: output_scale.c; the "gpu" of the [metal] stats line), ns so far: the game's
// command buffers, the window image's (metal.m backend_present), and the plain sum of their spans (diagnostics)
uint64_t g_gpu_total_ns, g_gpu_win_ns, g_gpu_sum_ns;
// Frame pacing (pace.c): the GPU's busy time so far, the game's command buffers and the window image's (renderer_gpu_busy).
int64_t pace_gpu_ns(void) {
    return (int64_t)(__atomic_load_n(&g_gpu_total_ns, __ATOMIC_RELAXED) + __atomic_load_n(&g_gpu_win_ns, __ATOMIC_RELAXED));
}
// (forward: texture-lifetime cleanup and stats, defined further down)
static int g_views_live;
static void views_forget(void *base);
static NSMutableDictionary *g_snap_copies;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static uint64_t op_counts[CMD_COUNT];
bool g_debug_frame;
void renderer_save_texture(id<MTLTexture> tex, const char *path);
void renderer_save_texture_alpha(id<MTLTexture> tex, const char *path);
void renderer_save_game_texture(NvnTexture *t, id<MTLTexture> tex, const char *path, const char *alpha_path);  // (metal.m)
static char stats_text[320];

// ------------------------------------------------------------------ state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

// Same targets and views (field by field: the views come from guest records, their padding is not ours to compare).
static bool rt_same(const RtState *a, const RtState *b) {
    if (a->ncolor != b->ncolor || a->depth != b->depth || a->has_dview != b->has_dview) return false;
    if (a->has_dview && memcmp(&a->dview, &b->dview, sizeof a->dview)) return false;
    for (int i = 0; i < a->ncolor; i++) {
        if (a->color[i] != b->color[i] || a->has_cview[i] != b->has_cview[i]) return false;
        if (a->has_cview[i] && memcmp(&a->cview[i], &b->cview[i], sizeof a->cview[i])) return false;
    }
    return true;
}

static struct {
    RtState rt;
    bool rt_dirty, render_enabled;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];  // factor, units, clamp (nvnCommandBufferSetPolygonOffsetClamp)
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
} st;

extern uint32_t g_window_origin_lower_left_hack;
uint32_t nvn_window_origin(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

static double stat_compile_ms;  // time spent building shaders/pipelines (first-use stutter)
static int stat_compiles;
static inline double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
#define TIMED_COMPILE(expr) ({ double t0_ = now_ms(); __auto_type r_ = (expr); stat_compile_ms += now_ms() - t0_; stat_compiles++; \
                               slow_note_build("pipeline", (uint64_t)((now_ms() - t0_) * 1e6)); r_; })

static uint64_t stat_counters_gpu;  // timing counters given the GPU's time (the game's 30 / 60 fps choice)
const char *renderer_stats(void) {
    static int dumps;
    if (dumps++ < 3) {
        LOG("[metal] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, op_counts[i]);
        LOG("\n");
    }
    extern int g_tex_live;
    extern uint64_t g_tex_live_bytes;
    struct task_vm_info vmi;
    mach_msg_type_number_t vn = TASK_VM_INFO_COUNT;
    uint64_t foot = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vmi, &vn) == KERN_SUCCESS ? vmi.phys_footprint : 0;
    snprintf(stats_text, sizeof stats_text, "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, gpu %.0fms, wait %.0fms, compile %d/%.0fms, fbcopy %llu | tex %d (%.0f MB) views %d snaps %lu vcopy %llu/%.0fKB mem %.0f MB | waits fence %.0fms/%llu flush %.0fms/%llu all %.0fms/%llu | deferred: clears as load %llu, clear passes %llu | counters gpu %llu | pace %s, %llu frames over", stat_draws, stat_skipped,
             stat_passes, stat_dispatches, stat_flushes, stat_gpu_s * 1000.0, (double)stat_wait_ns / 1e6 * 125.0 / 3.0, stat_compiles, stat_compile_ms, stat_feedback_copies,
             g_tex_live, g_tex_live_bytes / 1048576.0, g_views_live, (unsigned long)(g_snap_copies ? g_snap_copies.count : 0), (unsigned long long)stat_vcopy, stat_vcopy_bytes / 1024.0, foot / 1048576.0, stat_wait_by[0] / 24e3, (unsigned long long)stat_wait_n[0], stat_wait_by[1] / 24e3, (unsigned long long)stat_wait_n[1], stat_wait_by[2] / 24e3, (unsigned long long)stat_wait_n[2], (unsigned long long)stat_clears_as_load, (unsigned long long)stat_clear_passes,
             (unsigned long long)stat_counters_gpu, pace_is_slow() ? "30" : "60", (unsigned long long)pace_frames_over());
    stat_counters_gpu = 0;
    stat_compiles = 0;
    stat_feedback_copies = 0;
    stat_compile_ms = 0;
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    stat_vcopy = stat_vcopy_bytes = 0;
    memset(stat_wait_by, 0, sizeof stat_wait_by); memset(stat_wait_n, 0, sizeof stat_wait_n);
    stat_gpu_s = 0;
    stat_clears_as_load = stat_clear_passes = 0;
    return stats_text;
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    st.render_enabled = true;
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

void renderer_init(id<MTLDevice> d) {
    dev = d;
    if (!cache) cache = [NSMutableDictionary new];  // (kept: shader preparation may have run on the start screen)
    st_defaults();
}

static int g_passes_in_cb;  // render passes encoded into the open command buffer (chunked commits)
static id<MTLCommandBuffer> cb(void) {
    if (!cmdbuf) cmdbuf = [renderer_queue() commandBuffer];
    return cmdbuf;
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
void renderer_texture_fraction(NvnTexture *t, float *fx, float *fy);  // (metal.m: the part the contents occupy)
void renderer_get_scale(float *sx, float *sy);
static float pass_sx = 1, pass_sy = 1;  // allocation scale of the current pass's attachments
static int pass_rt_w, pass_rt_h;        // the current pass's render area (0: its attachments' whole size)
bool renderer_texture_presented(NvnTexture *t);
// Widescreen UI: 2D draws into the swapchain image use a centred 16:9 region (uniform scale) instead of
// the stretched wide mapping. vp_* is the mapping for the current draw: physical = guest * s + off.
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][2];          // per bound texture slot: the contents' scale (for shaders' texel math)
static float tex_fr[2][64][2];          // per bound texture slot: the part of the texture the contents occupy (Dynamic)
static uint32_t tex_wh[2][64][2];       // per bound texture slot: guest size
static int draw_bottle;                 // a fragment texture is the bottle's item icon (1) / model map (2): Escape Bottle
static int draw_heart;                  // ... a HUD / file select heart's: heart colour (Cosmetics tab)
static bool draw_item_cursor;           // a fragment texture is the Items page's cursor brackets
static int draw_recolour[2];            // ... Cosmetics recolour textures: their classes (runtime/nvn/recolour.c)
static char draw_recolour_names[512];   // ... their names (LARECOMP_LOG_RECOLOUR_AT, test)
#define RSCALE_BUFFER_INDEX 18

static int debug_pass_no;
// Attachment traffic. A tile GPU loads and stores every attachment of every pass, and the game clears each target
// with a full clear before drawing it: 13 of the 45 passes of a Field_11C frame were clear quads alone, and the game
// also re-sets the targets it already has between draw groups, which ended the pass (store + load of everything,
// 3840x2160 depth, stencil and colour at 2x). Measured, Field_11C: 488 -> 343 MB of attachment traffic per frame at 1x,
// 1701 -> 1297 MB at 2x, 45 -> 42 passes, picture unchanged. ~2 ms a frame on a 68 GB/s M1 Air.
// - A full clear is recorded on its texture and becomes the load action of the next pass that binds the texture as a
//   target (no load, no clear quad), whatever comes in between (other targets, dispatches, submits: the clear stays
//   with the texture). It gets a pass of its own, which only stores, only if the texture is read first (sampled,
//   copied, presented). That took the remaining 12 clear-only passes of the frame (65 MB at 1x). LARECOMP_MTL_LOAD_CLEAR=0:
//   clears are quads again; LARECOMP_MTL_DEFER_CLEAR=0: a recorded clear is flushed as soon as its targets change.
// - SetRenderTargets with the targets already bound keeps the pass open. LARECOMP_MTL_RT_DEDUP=0: ends it as before.
// (Don't-care depth / stencil stores were tried here too: the next pass loads the depth in nearly every case, only ~2
// of the ~8 stores a frame precede a clear of the texture, and a guess learned from the previous frame read stale
// depth at scene changes. Not worth a wrong frame.)
typedef struct {
    NvnTexture *t;
    void *backend;  // (the texture's TexBackend: a new texture at a reused address isn't this one)
    bool clear_on, clear_color, clear_depth, clear_stencil;  // a recorded clear (full target, every channel)
    double cc[4], dval;
    uint32_t sval;
} TexDefer;
#define TD_TAB 2048
static TexDefer td_tab[TD_TAB];
static int td_pending;  // textures with a recorded clear
static bool opening_pass;  // begin_pass is ending the previous pass (end_pass must not open one)
static void begin_pass(void);
static void end_pass(void);
static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_);

static int td_defer_on(void) { static int v = -1; if (v < 0) { const char *e = ENV("LARECOMP_MTL_DEFER_CLEAR"); v = !e || atoi(e); } return v; }

static TexDefer *td_get(NvnTexture *t, bool create) {
    if (!t) return NULL;
    uint32_t h = (uint32_t)(((uintptr_t)t >> 4) * 0x9E3779B1u) & (TD_TAB - 1);
    for (int probe = 0; probe < TD_TAB; probe++, h = (h + 1) & (TD_TAB - 1)) {
        TexDefer *e = &td_tab[h];
        if (e->t == t) {
            // (the Metal object may not exist yet when a clear is recorded: adopt it; a different one means a new
            // texture at a reused address)
            if (!e->backend) e->backend = t->backend;
            else if (t->backend && e->backend != t->backend) { if (e->clear_on) td_pending--; memset(e, 0, sizeof *e); e->t = t; e->backend = t->backend; }
            return e;
        }
        if (!e->t) {
            if (!create) return NULL;
            memset(e, 0, sizeof *e);
            e->t = t; e->backend = t->backend;
            return e;
        }
    }
    return NULL;
}

static void td_forget(NvnTexture *t) {  // (the texture is gone: drain_gone)
    TexDefer *e = td_get(t, false);
    if (!e) return;
    if (e->clear_on) td_pending--;
    e->t = NULL;  // (a tombstone keeps the probe chains intact: the slot stays unusable until a reset)
    e->backend = (void *)1;
}

// The recorded clear of a texture that is about to be read: a pass of its own on that texture (load action clear,
// store, nothing drawn), like a clear-only pass used to be.
static bool td_flushing;
static void td_flush(NvnTexture *t) {
    TexDefer *e = t ? td_get(t, false) : NULL;
    if (!e || !e->clear_on || td_flushing) return;
    td_flushing = true;
    RtState saved = st.rt;
    bool dirty = st.rt_dirty;
    end_pass();
    memset(&st.rt, 0, sizeof st.rt);
    if (renderer_format_is_depth(t->format)) st.rt.depth = t;
    else { st.rt.ncolor = 1; st.rt.color[0] = t; }
    st.rt_dirty = true;
    begin_pass();  // (consumes the recorded clear)
    end_pass();
    stat_clear_passes++;
    st.rt = saved;
    st.rt_dirty = true;
    td_flushing = false;
    (void)dirty;
}

// The shaders' textures of a draw / dispatch that have a recorded clear get it applied before the pass opens.
static void td_flush_shader_textures(int stage, ShaderInfo *si) {
    if (!td_pending || !si) return;
    for (int i = 0; i < si->ntex; i++) td_flush(shader_texture_object(stage, &si->tex[i]));
}

// Off mode (LARECOMP_MTL_DEFER_CLEAR=0): the recorded clears of the current targets become passes now.
static void td_flush_targets(const RtState *rt) {
    if (!td_pending) return;
    for (int i = 0; i < rt->ncolor; i++) td_flush(rt->color[i]);
    td_flush(rt->depth);
}

void renderer_texture_will_read(NvnTexture *t) { td_flush(t); }  // (metal.m: the presented image)
static void end_pass(void) {
    if (!td_defer_on() && !opening_pass) td_flush_targets(&st.rt);  // (off mode: as before, the clear gets its pass now)
    if (enc) {
        [enc endEncoding];
        enc = nil;
        if (g_debug_frame && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
            id<MTLTexture> t = renderer_texture(st.rt.color[0], nil, true);
            id<MTLCommandBuffer> c = cmdbuf;
            cmdbuf = nil;
            [c commit];
            [c waitUntilCompleted];
            char path[256], apath[256];
            snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no);
            snprintf(apath, sizeof apath, "build/pass_%03d_a.png", debug_pass_no++);
            renderer_save_game_texture(st.rt.color[0], t, path, apath);  // (the part the pass drew: Dynamic resolution)
        }
    }
}

static void ring_reset(void);
// Submitted work runs asynchronously; last_cb is the most recently committed command buffer (the queue
// executes in order, so waiting for it waits for everything before it). Other guest threads (nvnSyncWait)
// read last_cb, so it is guarded by cb_lock; everything else here runs on the submitting thread.
static id<MTLCommandBuffer> last_cb;
static pthread_mutex_t cb_lock = PTHREAD_MUTEX_INITIALIZER;
// Fences: each committed command buffer gets a sequence number; a fence remembers the number of the submission
// that contains it, and nvnSyncWait waits only for that one (not for everything submitted since), so the game's
// CPU work on frame N+1 overlaps the GPU's frame N as on the Switch.
#define CB_HIST 256
static uint64_t submit_seq;
static id<MTLCommandBuffer> cb_hist[CB_HIST];
// CPU / GPU overlap. The game's frame loop submits a frame, presents it, and at once waits on that frame's fence
// (measured: every SyncWait is on the newest submission, one call site, never already done) - so it builds frame
// N+1 only once the GPU has finished frame N, and a frame costs CPU + GPU time in series (M3 Max 5120x1440 MetalFX:
// ~5 ms of CPU + 13 ms of GPU missed 60 fps with the GPU under half busy). The Switch's own structure; there the two
// fit. Here the wait is taken one fence late: SyncWait(fence N) waits for fence N-1's submission and returns, and
// the next SyncWait waits for N. The CPU builds frame N+1 while the GPU draws N; at most two frames of GPU work
// are queued; a fence that is already complete is never waited for. What the game may touch in that one frame
// while the GPU still reads it: its per-frame vertex / index data in the guest heap (bound in place), which an
// engine built to overlap double-buffers anyway; uniform data is snapshotted at translation (ring_alloc). GPU
// results the CPU reads (texture-to-buffer and buffer-to-buffer copies) are flushed synchronously at translation,
// so they don't depend on the fence. The frame timing counters would be read a frame before their GPU time is
// known: in this mode an end counter reports the latest completed frame's GPU span (counters_resolve keeps it host
// side and no longer writes the game's memory late - the game may have reused the slot). LARECOMP_GPU_OVERLAP=0:
// off (the wait on the fence's own submission, as before).
static uint64_t deferred_seq;  // (cb_lock) the fence the game was told is done but whose submission wasn't waited for
static bool overlap_on(void) {
    if (!title_is_las()) return false;
    static int on = -1;
    if (on < 0) {
        const char *e = ENV("LARECOMP_GPU_OVERLAP");
        on = !e || atoi(e) != 0;
        LOG("[metal] GPU overlap %s (the game's frame fence waited one frame late; LARECOMP_GPU_OVERLAP=0: off)\n", on ? "on" : "off");
    }
    return on;
}
static uint64_t g_last_gpu_frame_ticks;  // (overlap) the latest resolved counter pair's GPU span, in Switch GPU ticks

// ---- the game's GPU timing counters (nvnCommandBufferReportCounter, two per frame)
// The game times its GPU work with these and takes its 30 fps mode when frames look slow (pace.c). They used to get
// the host clock at translation time: our command translation (~2 ms however long the GPU took), so a Mac that
// couldn't hold 60 kept the game in its 60 fps mode - slow motion at 30. Now the command buffer is split at each
// counter (Apple GPUs have no timestamps inside a buffer), and once that's done the counter gets the GPU's time
// there: the end of the buffer before it, or the start of the one after it if the GPU sat idle in between. Until
// then the host clock (the game waits on its fence before reading them; resolved after each wait). As vulkan/exec.c.
// LARECOMP_MTL_COUNTER=cpu: the host clock only.
#define CTR_MAX 64
static struct { uint64_t *out; const uint64_t *start; uint64_t before, after; } ctr_pend[CTR_MAX];
static int nctr;
static pthread_mutex_t ctr_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t ns_to_gpu_ticks(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * 384 / 625); }  // (614.4 MHz)

static void counters_resolve(void) {
    pthread_mutex_lock(&ctr_lock);
    pthread_mutex_lock(&cb_lock);
    int k = 0;
    for (int i = 0; i < nctr; i++) {
        uint64_t bs = ctr_pend[i].before, as = ctr_pend[i].after;
        if (submit_seq - bs >= CB_HIST || (as <= submit_seq && submit_seq - as >= CB_HIST)) continue;  // (gone)
        id<MTLCommandBuffer> b = bs ? cb_hist[bs % CB_HIST] : nil;
        id<MTLCommandBuffer> a = as <= submit_seq ? cb_hist[as % CB_HIST] : nil;
        // both sides done: a buffer not committed yet (a frame's first chunk still being translated) would leave a
        // start counter at the previous frame's end - backend_execute commits one at the end of each list
        bool done = as <= submit_seq && (!b || b.status >= MTLCommandBufferStatusCompleted) &&
                    (!a || a.status >= MTLCommandBufferStatusCompleted);
        if (!done) {
            ctr_pend[k++] = ctr_pend[i];
            continue;
        }
        double t = b ? b.GPUEndTime : 0;
        if (a && a.GPUStartTime > t) t = a.GPUStartTime;
        if (t > 0) {
            uint64_t *out = ctr_pend[i].out;
            uint64_t ticks = ns_to_gpu_ticks((uint64_t)(t * 1e9));  // (host time base)
            if (overlap_on()) {
                // kept host side: the pair's span becomes the next end counter's reading (the game has read this
                // pair already, and may have reused the slot for the next frame's counters)
                static const uint64_t *last_out;
                static uint64_t last_ticks;
                if (ctr_pend[i].start && ctr_pend[i].start == last_out && out - ctr_pend[i].start == 2 && ticks >= last_ticks &&
                    ticks - last_ticks < 614400000ull)  // (an end counter 16 bytes after its start; under a second)
                    g_last_gpu_frame_ticks = ticks - last_ticks;
                last_out = out, last_ticks = ticks;
            } else {
                out[1] = pace_end_counter(ctr_pend[i].start, out, ticks);
            }
            stat_counters_gpu++;
        }
    }
    nctr = k;
    pthread_mutex_unlock(&cb_lock);
    pthread_mutex_unlock(&ctr_lock);
}

// A counter waits for the buffer after it: true when one ended the list just translated (backend_execute commits
// one then, empty, so the counter isn't held until the next list).
static bool counters_want_next(void) {
    pthread_mutex_lock(&ctr_lock);
    bool want = false;
    for (int i = 0; i < nctr && !want; i++) want = ctr_pend[i].after > submit_seq;
    pthread_mutex_unlock(&ctr_lock);
    return want;
}

static int g_wait_kind;
static uint64_t cum_wait_ticks;  // GPU waits so far (mach ticks; never reset)
static void wait_cb(id<MTLCommandBuffer> c) {
    if (!c) return;
    uint64_t t0 = mach_absolute_time();
    extern void rq_watch(const char *what, uint64_t arg);
    rq_watch("waitUntilCompleted (GPU)", 0);
    [c waitUntilCompleted];
    rq_watch(NULL, 0);
    uint64_t d = mach_absolute_time() - t0;
    stat_wait_ns += d;
    __atomic_fetch_add(&cum_wait_ticks, d, __ATOMIC_RELAXED);  // (the slow-frame log)
    stat_wait_by[g_wait_kind] += d;
    if (d > 24000) stat_wait_n[g_wait_kind]++;  // (> ~1 ms counts as a real wait)
    counters_resolve();
}

// Textures the game destroyed (any thread): their cached views and snapshot copies are dropped on the render thread.
static pthread_mutex_t gone_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { void *nvn, *mtl; } gone[4096];
static int ngone;
void renderer_texture_gone(void *nvn_tex, void *mtl_tex) {
    pthread_mutex_lock(&gone_lock);
    if (ngone < 4096) { gone[ngone].nvn = nvn_tex; gone[ngone].mtl = mtl_tex; ngone++; }
    pthread_mutex_unlock(&gone_lock);
}
static void drain_gone(void) {
    pthread_mutex_lock(&gone_lock);
    int n = ngone;
    static struct { void *nvn, *mtl; } local[4096];
    memcpy(local, gone, sizeof local[0] * n);
    ngone = 0;
    pthread_mutex_unlock(&gone_lock);
    for (int i = 0; i < n; i++) {
        if (local[i].mtl) views_forget(local[i].mtl);
        if (local[i].nvn) {
            td_forget(local[i].nvn);
            if (g_snap_copies) [g_snap_copies removeObjectForKey:[NSValue valueWithPointer:local[i].nvn]];
            for (int k = 0; k < 8; k++) if (snap_tex[k] == local[i].nvn) { snap_tex[k] = NULL; snap_ok[k] = false; }
        }
    }
}

// A completed command buffer's share of the GPU's busy time (ns), counted once: the GPU starts a buffer while the one
// before it still runs (the game's frame is several - begin_pass commits every 8 passes - and the window image's is
// one more), so their GPUStartTime..GPUEndTime spans overlap; a buffer can also start and then sit waiting for the one
// before it to produce what it reads. Summed, they counted that overlap twice or more: M3 Max 5120x1440, frames that
// held 60 fps measured "game 16 + window 3 ms", and at the 1/3 floor (1707x480) still "game 12 + window 5" - Dynamic
// sat at its floor where a fixed 1x (3840x1080) holds 60. Their union is counted: each buffer from where the latest
// end so far left off (one queue: they start in order). A window buffer gets only its time after the game's work.
static pthread_mutex_t busy_lock = PTHREAD_MUTEX_INITIALIZER;
static double busy_end;  // (s, GPU clock) the latest command buffer end counted
uint64_t renderer_gpu_busy(id<MTLCommandBuffer> done, bool win) {
    double s = done.GPUStartTime, e = done.GPUEndTime;
    if (!(e > s) || e - s > 1.0) return 0;  // (not run, or nonsense)
    pthread_mutex_lock(&busy_lock);
    if (busy_end > e + 1.0) busy_end = 0;  // (clock base changed)
    double from = s > busy_end ? s : busy_end;
    if (e > busy_end) busy_end = e;
    pthread_mutex_unlock(&busy_lock);
    uint64_t ns = e > from ? (uint64_t)((e - from) * 1e9) : 0;
    __atomic_add_fetch(win ? &g_gpu_win_ns : &g_gpu_total_ns, ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_gpu_sum_ns, (uint64_t)((e - s) * 1e9), __ATOMIC_RELAXED);
    return ns;
}

// Commit recorded work without waiting.
void renderer_submit(void) {
    end_pass();
    drain_gone();
    if (cmdbuf) {
        id<MTLCommandBuffer> c = cmdbuf;
        cmdbuf = nil;
        [c addCompletedHandler:^(id<MTLCommandBuffer> done) {
            stat_gpu_s += renderer_gpu_busy(done, false) / 1e9;
            if (done.error) LOG("[metal] command buffer error: %s\n", done.error.localizedDescription.UTF8String);
        }];
        [c commit];
        g_passes_in_cb = 0;
        pthread_mutex_lock(&cb_lock);
        last_cb = c;
        submit_seq++;
        cb_hist[submit_seq % CB_HIST] = c;
        pthread_mutex_unlock(&cb_lock);
        stat_flushes++;
        counters_resolve();  // (also without fence waits: a movie, loading - the table doesn't fill)
    }
}

// Finish all recorded GPU work (the CPU is about to read GPU results, or reuse the ring).
void renderer_flush(void) {
    renderer_submit();
    pthread_mutex_lock(&cb_lock);
    id<MTLCommandBuffer> c = last_cb;
    pthread_mutex_unlock(&cb_lock);
    g_wait_kind = 1;
    wait_cb(c);
    g_wait_kind = 0;
    pthread_mutex_lock(&cb_lock);
    deferred_seq = 0;  // (everything committed so far is done)
    pthread_mutex_unlock(&cb_lock);
    ring_reset();
}

// Wait for submitted work without committing anything new (nvnSyncWait / QueueFinish).
// Callable from any thread: everything submitted so far was committed by renderer_submit.
void renderer_sync(void) {
    pthread_mutex_lock(&cb_lock);
    id<MTLCommandBuffer> c = last_cb;
    pthread_mutex_unlock(&cb_lock);
    g_wait_kind = 2;
    wait_cb(c);
    g_wait_kind = 0;
    pthread_mutex_lock(&cb_lock);
    deferred_seq = 0;
    pthread_mutex_unlock(&cb_lock);
}

// (the slow-frame log, slowframe.h; the game thread) running totals: no heap imports here (the heap is shared once)
void renderer_slow_counters(uint64_t *submits, uint64_t *gpu_wait_ns, uint64_t *imports) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    *submits = __atomic_load_n(&submit_seq, __ATOMIC_RELAXED);
    *gpu_wait_ns = (uint64_t)((unsigned __int128)__atomic_load_n(&cum_wait_ticks, __ATOMIC_RELAXED) * tb.numer / tb.denom);
    *imports = 0;
}

uint64_t renderer_submit_seq(void) {
    pthread_mutex_lock(&cb_lock);
    uint64_t q = submit_seq;
    pthread_mutex_unlock(&cb_lock);
    return q;
}
uint64_t renderer_issued_seq(void) { return renderer_submit_seq(); }  // (rqueue.h: committed here is submitted)

// Sequence number a fence placed now belongs to: the pending (uncommitted) work's submission, else the last one.
uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&cb_lock);
    uint64_t q = submit_seq + (cmdbuf ? 1 : 0);
    pthread_mutex_unlock(&cb_lock);
    return q;
}

// Wait until submission `seq` has completed (0 = nothing to wait for).
void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    if (ENV("LARECOMP_LOG_FENCES")) {  // (debug) how far behind the newest submission the waited fence is
        extern uint64_t renderer_present_count(void);
        static uint64_t f0, hist[6], frames_seen;
        uint64_t f = renderer_present_count();
        uint64_t pend = submit_seq + (cmdbuf ? 1 : 0);
        uint64_t age = pend > seq ? pend - seq : 0;
        bool done_already = false;
        pthread_mutex_lock(&cb_lock);
        if (seq <= submit_seq && submit_seq - seq < CB_HIST && cb_hist[seq % CB_HIST])
            done_already = cb_hist[seq % CB_HIST].status >= MTLCommandBufferStatusCompleted;
        pthread_mutex_unlock(&cb_lock);
        hist[age > 4 ? 4 : age]++;
        if (done_already) hist[5]++;
        if (f - f0 >= 300) {
            frames_seen = f - f0;
            LOG("[fences] per %llu frames: waits by age (submissions behind newest) 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu, already done %llu; submissions now %llu\n",
                (unsigned long long)frames_seen, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], (unsigned long long)submit_seq);
            memset(hist, 0, sizeof hist);
            f0 = f;
        }
    }
    if (overlap_on()) {  // (see deferred_seq) wait for the fence deferred last time, defer this one
        pthread_mutex_lock(&cb_lock);
        bool done = seq <= submit_seq && submit_seq - seq < CB_HIST && cb_hist[seq % CB_HIST] &&
                    cb_hist[seq % CB_HIST].status >= MTLCommandBufferStatusCompleted;
        uint64_t prev = deferred_seq;
        if (done) {
            if (seq >= prev) deferred_seq = 0;  // (in order: the older one is complete too)
            pthread_mutex_unlock(&cb_lock);
            counters_resolve();
            return;
        }
        if (seq > prev) {  // a newer fence: the deferred one is waited for now, this one at the next call
            deferred_seq = seq;
            pthread_mutex_unlock(&cb_lock);
            if (!prev) { counters_resolve(); return; }
            seq = prev;
        } else {  // the deferred fence again, or an older one: a real wait on it
            if (seq == prev) deferred_seq = 0;
            pthread_mutex_unlock(&cb_lock);
        }
    }
    pthread_mutex_lock(&cb_lock);
    id<MTLCommandBuffer> c = nil;
    if (seq > submit_seq) c = last_cb;  // not committed yet (other thread): wait for everything committed so far
    else if (submit_seq - seq < CB_HIST) c = cb_hist[seq % CB_HIST];
    pthread_mutex_unlock(&cb_lock);
    wait_cb(c);
}

// ------------------------------------------------------------------ textures with views
int renderer_wait_seq_timeout(uint64_t seq, uint64_t timeout) {
    uint64_t begin = plat_now_ns();
    bool waited = false;
    for (;;) {
        pthread_mutex_lock(&cb_lock);
        bool submitted = seq <= submit_seq;
        id<MTLCommandBuffer> c = submitted && submit_seq - seq < CB_HIST ? cb_hist[seq % CB_HIST] : nil;
        bool done = !seq || (submitted && (!c || c.status >= MTLCommandBufferStatusCompleted));
        pthread_mutex_unlock(&cb_lock);
        if (done) return waited ? 1 : 0;
        if (plat_now_ns() - begin >= timeout) return 2;
        waited = true;
        plat_sleep_until_ns(plat_now_ns() + 1000000);
    }
}

// Texture view cache: views are made per bind otherwise (an allocation per draw). Keyed by the base texture and
// the view parameters; a cached view retains its base, so a key's base pointer can't be reused while cached.
#define VIEW_TAB 16384
static struct { uint64_t key; void *base; void *view; } view_tab[VIEW_TAB];

// Views keep their base texture alive: when the game frees a texture its views must go too (they piled up -
// render targets of a 32:9 window held long after the game dropped them - and a new texture at the same address
// could get a stale view). Entries are tombstoned (key ~0) so probe chains stay intact.
static void views_forget(void *base) {
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].base == base && view_tab[i].view) {
            CFRelease(view_tab[i].view);
            view_tab[i].view = NULL;
            view_tab[i].base = NULL;
            view_tab[i].key = ~0ull;
            g_views_live--;
        }
}

static id<MTLTexture> view_cached(id<MTLTexture> base, uint64_t params, id<MTLTexture> (^make)(void)) {
    uint64_t k = ((uint64_t)(uintptr_t)(__bridge void *)base * 0x9E3779B97F4A7C15ull) ^ (params * 0xC2B2AE3D27D4EB4Full);
    if (!k) k = 1;
    if (k == ~0ull) k = 2;
    // up to 8 probes; a miss is stored in the first free or tombstoned slot of those, else over the first one.
    // (It used to go to the slot past the 8 probes - never found again - and tombstones were never reused: on the
    // overworld, where textures stream in and out, more and more binds made a new view every time, the slowdown
    // that grew with play time until a level change - user report 2026-10-08, 32:9.)
    uint32_t start = (uint32_t)(k ^ (k >> 32)) & (VIEW_TAB - 1), slot = start;
    int free_slot = -1;
    for (int probe = 0; probe < 8; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        if (view_tab[slot].key == k && view_tab[slot].base == (__bridge void *)base) return (__bridge id<MTLTexture>)view_tab[slot].view;
        if (!view_tab[slot].key || view_tab[slot].key == ~0ull) {
            if (free_slot < 0) free_slot = (int)slot;
            if (!view_tab[slot].key) break;  // (end of the chain)
        }
    }
    slot = free_slot >= 0 ? (uint32_t)free_slot : start;
    id<MTLTexture> v = make();
    if (!v) return base;
    if (ENV("LARECOMP_LOG_VIEWCREATE")) {  // (debug: what keeps making new views - overworld soak, views pile up)
        extern uint64_t renderer_present_count(void);
        static int n;
        if (renderer_present_count() > (uint64_t)atol(ENV("LARECOMP_LOG_VIEWCREATE")) && n++ < 400)
            LOG("[viewnew] f%llu base %p %s %lux%lu fmt %lu type %lu params %#llx live %d\n", (unsigned long long)renderer_present_count(),
                (__bridge void *)base, base.label.UTF8String, (unsigned long)base.width, (unsigned long)base.height,
                (unsigned long)base.pixelFormat, (unsigned long)base.textureType, (unsigned long long)params, g_views_live);
    }
    if (view_tab[slot].view) { CFRelease(view_tab[slot].view); g_views_live--; }  // evict (8 probes all taken)
    g_views_live++;
    view_tab[slot].key = k;
    view_tab[slot].base = (__bridge void *)base;
    view_tab[slot].view = (void *)CFBridgingRetain(v);
    return v;
}

// Metal format views: an ordinary colour format can be viewed as another one of the same size, a compressed one only
// as itself or its sRGB / linear twin, depth and stencil formats only as themselves. Anything else is a Metal
// assertion (not only with API validation: newTextureViewWithPixelFormat checks it in the driver and aborts).
static int mtl_color_bytes(MTLPixelFormat f) {  // texel size of an ordinary colour format, 0 = not one
    switch (f) {
    case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Unorm_sRGB: case MTLPixelFormatR8Snorm: case MTLPixelFormatR8Uint:
    case MTLPixelFormatR8Sint: case MTLPixelFormatA8Unorm:
        return 1;
    case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint:
    case MTLPixelFormatR16Float: case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Unorm_sRGB: case MTLPixelFormatRG8Snorm:
    case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint: case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatA1BGR5Unorm:
    case MTLPixelFormatABGR4Unorm: case MTLPixelFormatBGR5A1Unorm:
        return 2;
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatR32Float: case MTLPixelFormatRG16Unorm:
    case MTLPixelFormatRG16Snorm: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRG16Float:
    case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatRGBA8Snorm:
    case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint: case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB: case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatRGB10A2Uint:
    case MTLPixelFormatBGR10A2Unorm: case MTLPixelFormatRG11B10Float: case MTLPixelFormatRGB9E5Float:
        return 4;
    case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA16Unorm:
    case MTLPixelFormatRGBA16Snorm: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint:
    case MTLPixelFormatRGBA16Float:
        return 8;
    case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: case MTLPixelFormatRGBA32Float:
        return 16;
    default:
        return 0;
    }
}
static MTLPixelFormat mtl_linear_twin(MTLPixelFormat f) {  // sRGB format -> its linear one
    switch (f) {
    case MTLPixelFormatR8Unorm_sRGB: return MTLPixelFormatR8Unorm;
    case MTLPixelFormatRG8Unorm_sRGB: return MTLPixelFormatRG8Unorm;
    case MTLPixelFormatRGBA8Unorm_sRGB: return MTLPixelFormatRGBA8Unorm;
    case MTLPixelFormatBGRA8Unorm_sRGB: return MTLPixelFormatBGRA8Unorm;
    case MTLPixelFormatBC1_RGBA_sRGB: return MTLPixelFormatBC1_RGBA;
    case MTLPixelFormatBC2_RGBA_sRGB: return MTLPixelFormatBC2_RGBA;
    case MTLPixelFormatBC3_RGBA_sRGB: return MTLPixelFormatBC3_RGBA;
    case MTLPixelFormatBC7_RGBAUnorm_sRGB: return MTLPixelFormatBC7_RGBAUnorm;
    default:
        if (f >= MTLPixelFormatASTC_4x4_sRGB && f <= MTLPixelFormatASTC_12x12_sRGB)
            return f - MTLPixelFormatASTC_4x4_sRGB + MTLPixelFormatASTC_4x4_LDR;
        return f;
    }
}
static bool mtl_view_compatible(MTLPixelFormat base, MTLPixelFormat view) {
    if (base == view) return true;
    int bb = mtl_color_bytes(base), vb = mtl_color_bytes(view);
    if (bb || vb) return bb == vb;
    return mtl_linear_twin(base) == mtl_linear_twin(view);  // (depth / stencil formats have no twin)
}
// A view in another format than the sRGB / linear twin needs MTLTextureUsagePixelFormatView on the texture (metal.m
// create_backend_texture: given only to textures the game views that way, as the flag stops lossless compression).
bool renderer_view_needs_format_flag(MTLPixelFormat base, MTLPixelFormat view) {
    return base != view && mtl_linear_twin(base) != mtl_linear_twin(view);
}

static id<MTLTexture> view_texture(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render,
                                   id<MTLBlitCommandEncoder> blit) {
    id<MTLTexture> base = renderer_texture(t, blit, render);
    if (!has_view || !v->setmask) return base;
    MTLPixelFormat pf = (v->setmask & 4) ? renderer_pixel_format(v->format) : base.pixelFormat;
    if (!mtl_view_compatible(base.pixelFormat, pf) ||  // (see mtl_view_compatible: the base format instead)
        (renderer_view_needs_format_flag(base.pixelFormat, pf) && !(base.usage & MTLTextureUsagePixelFormatView))) {
        static int nlog;
        if (!nlog++)
            LOG("[view] texture %ux%u fmt %#x (mtl %lu) can't be viewed as nvn %#x (mtl %lu): its own format instead "
                "(logged once)\n", t->width, t->height, t->format, (unsigned long)base.pixelFormat, v->format, (unsigned long)pf);
        pf = base.pixelFormat;
    }
    NSUInteger lv0 = (v->setmask & 1) ? v->base_level : 0;
    NSUInteger nlv = (v->setmask & 1) && v->num_levels ? v->num_levels : base.mipmapLevelCount - lv0;
    NSUInteger ly0 = (v->setmask & 2) ? v->min_layer : 0;
    NSUInteger nly = (v->setmask & 2) && v->num_layers ? v->num_layers : base.arrayLength - ly0;
    if (base.textureType == MTLTextureTypeCube) nly = 6;
    MTLTextureType type = base.textureType;
    if (render && (type == MTLTextureType2DArray || type == MTLTextureTypeCube) && nly == 1) type = MTLTextureType2D;
    if (lv0 + nlv > base.mipmapLevelCount) nlv = base.mipmapLevelCount - lv0;
    if (pf == base.pixelFormat && lv0 == 0 && nlv == base.mipmapLevelCount && ly0 == 0 && type == base.textureType)
        return base;
    static int vlog;  // LARECOMP_LOG_VIEWS: texture views that change the pixel format
    if (pf != base.pixelFormat && vlog < 20 && getenv("LARECOMP_LOG_VIEWS")) {
        vlog++;
        LOG("[view] tex fmt %#x (mtl %lu) viewed as nvn %#x (mtl %lu)\n", t->format, (unsigned long)base.pixelFormat, v->format, (unsigned long)pf);
    }
    NSUInteger ns = type == MTLTextureType2D ? 1 : nly;
    uint64_t params = (uint64_t)pf | (uint64_t)type << 16 | (uint64_t)lv0 << 24 | (uint64_t)nlv << 32 | (uint64_t)ly0 << 40 | (uint64_t)ns << 52;
    id<MTLTexture> got = view_cached(base, params, ^{
        return [base newTextureViewWithPixelFormat:pf textureType:type levels:NSMakeRange(lv0, nlv) slices:NSMakeRange(ly0, ns)];
    });
    if (got == base && pf != base.pixelFormat) {
        static int nfail;
        if (nfail++ < 10) LOG("[view] FAILED %lu -> %lu (nvn %#x -> %#x)\n", (unsigned long)base.pixelFormat, (unsigned long)pf, t->format, v->format);
    }
    return got;
}

// ------------------------------------------------------------------ render passes
static __unsafe_unretained id last_bias_enc;  // encoder the depth bias was last set on

static void begin_pass(void) {
    if (enc && !st.rt_dirty) return;
    opening_pass = true;  // (the chunk submit's end_pass below must not flush anything into this pass)
    end_pass();
    st.rt_dirty = false;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    int w = 0, h = 0;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        id<MTLTexture> tex = view_texture(t, &st.rt.cview[i], st.rt.has_cview[i], true, nil);
        rp.colorAttachments[i].texture = tex;
        rp.colorAttachments[i].loadAction = MTLLoadActionLoad;
        // reshaped output: areas outside the game's 16:9 drawing (interiors, fades) start black each frame
        extern bool renderer_texture_first_write(NvnTexture *t);
        extern bool renderer_texture_level_fresh(NvnTexture *t);
        extern bool renderer_texture_frame_fresh(NvnTexture *t);
        bool fresh = renderer_texture_level_fresh(t) | renderer_texture_frame_fresh(t);
        if (renderer_texture_first_write(t) || fresh) {
            rp.colorAttachments[i].loadAction = MTLLoadActionClear;
            rp.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, fresh ? 0 : 1);
        }
        TexDefer *e = td_pending ? td_get(t, false) : NULL;
        if (e && e->clear_on && e->clear_color) {
            rp.colorAttachments[i].loadAction = MTLLoadActionClear;
            rp.colorAttachments[i].clearColor = MTLClearColorMake(e->cc[0], e->cc[1], e->cc[2], e->cc[3]);
            e->clear_on = false;
            td_pending--;
            stat_clears_as_load++;
        }
        rp.colorAttachments[i].storeAction = MTLStoreActionStore;
        if (!w) renderer_texture_scale(t, &pass_sx, &pass_sy);
        w = (int)tex.width; h = (int)tex.height;
    }
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        id<MTLTexture> tex = view_texture(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, nil);
        TexDefer *e = td_pending ? td_get(st.rt.depth, false) : NULL;
        bool has_d = renderer_format_is_depth(st.rt.depth->format) && st.rt.depth->format != 0x31;
        bool has_s = renderer_format_has_stencil(st.rt.depth->format);
        bool clr_d = e && e->clear_on && e->clear_depth, clr_s = e && e->clear_on && e->clear_stencil;
        if (has_d) {
            rp.depthAttachment.texture = tex;
            rp.depthAttachment.loadAction = clr_d ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.clearDepth = clr_d ? e->dval : 1.0;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
        }
        if (has_s) {
            rp.stencilAttachment.texture = tex;
            rp.stencilAttachment.loadAction = clr_s ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.stencilAttachment.clearStencil = clr_s ? e->sval : 0;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        if (e && e->clear_on) { e->clear_on = false; td_pending--; stat_clears_as_load++; }
        if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = (int)tex.width; h = (int)tex.height; }
    }
    pass_rt_w = pass_rt_h = 0;
    if (!w) {
        pass_sx = pass_sy = 1;
        rp.renderTargetWidth = 16; rp.renderTargetHeight = 16;
        rp.defaultRasterSampleCount = 1;
    } else {
        // Dynamic resolution draws into the top-left part of screen-sized targets: the pass's render area is that part.
        // Apple GPUs load and store every tile of the render area (default: the attachments' whole size) at each pass
        // however little is drawn, so a 5120x1440 target cost about the same at 1/3 of it (1707x480) as at all of it.
        int cw = 0, ch = 0, mw = 0, mh = 0;
        for (int i = -1; i < st.rt.ncolor; i++) {
            NvnTexture *t = i < 0 ? st.rt.depth : st.rt.color[i];
            id<MTLTexture> tex = i < 0 ? rp.depthAttachment.texture ?: rp.stencilAttachment.texture : rp.colorAttachments[i].texture;
            if (!t || !tex) continue;
            float fx, fy, sx, sy;
            renderer_texture_fraction(t, &fx, &fy);
            renderer_texture_scale(t, &sx, &sy);
            // (as far as the scissor reaches: pass_size)
            int pw = (int)ceilf(fmaxf(tex.width * fx, lroundf(t->width * sx))), ph = (int)ceilf(fmaxf(tex.height * fy, lroundf(t->height * sy)));
            if (pw > cw) cw = pw;
            if (ph > ch) ch = ph;
            if (!mw || (int)tex.width < mw) mw = (int)tex.width;
            if (!mh || (int)tex.height < mh) mh = (int)tex.height;
        }
        static int whole = -1;  // LARECOMP_PASS_WHOLE=1: the attachments' whole size, as before (tests)
        if (whole < 0) whole = getenv("LARECOMP_PASS_WHOLE") && atoi(getenv("LARECOMP_PASS_WHOLE"));
        if (!whole && cw > 0 && ch > 0 && (cw < mw || ch < mh)) {
            pass_rt_w = cw < mw ? cw : mw, pass_rt_h = ch < mh ? ch : mh;
            rp.renderTargetWidth = (NSUInteger)pass_rt_w;
            rp.renderTargetHeight = (NSUInteger)pass_rt_h;
        }
    }
    // Commit the frame in chunks: the GPU starts on the first passes while the CPU still encodes the rest. The
    // game waits for its previous frame before it submits the next one, so with one command buffer per frame the
    // GPU sat idle during our encoding and a frame cost GPU + encoding time (5120x1440 with the GPU clocked down:
    // ~15.5 + a few ms -> missed vsync -> 30 fps). LARECOMP_CHUNK_PASSES=<n> (0 = one buffer per frame).
    static int chunk = -1;
    if (chunk < 0) { const char *e = ENV("LARECOMP_CHUNK_PASSES"); chunk = e ? atoi(e) : 8; }
    if (chunk > 0 && cmdbuf && ++g_passes_in_cb >= chunk) renderer_submit();
    enc = [cb() renderCommandEncoderWithDescriptor:rp];
    opening_pass = false;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

// Physical size of the current pass (guest size times the attachments' allocation scale).
static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static void apply_scissor(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return;
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;  // "unbounded" scissors stay unbounded
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (pass_rt_w && x + sw > pass_rt_w) sw = pass_rt_w - x;  // (inside the render area)
    if (pass_rt_h && y + sh > pass_rt_h) sh = pass_rt_h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    [enc setScissorRect:(MTLScissorRect){(NSUInteger)x, (NSUInteger)y, (NSUInteger)sw, (NSUInteger)sh}];
}

// ------------------------------------------------------------------ clears (full-screen triangle)
static const char *clear_src =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 pos [[position]]; };\n"
    "vertex V vs(uint vid [[vertex_id]], constant float4 &d [[buffer(0)]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p * 2.0 - 1.0, d.x, 1); return o; }\n"
    "struct OF { float4 c [[color(INDEX)]]; };\n"
    "fragment OF fs_f(V in [[stage_in]], constant float4 &c [[buffer(0)]]) { OF o; o.c = c; return o; }\n"
    "struct OI { int4 c [[color(INDEX)]]; };\n"
    "fragment OI fs_i(V in [[stage_in]], constant int4 &c [[buffer(0)]]) { OI o; o.c = c; return o; }\n"
    "struct OU { uint4 c [[color(INDEX)]]; };\n"
    "fragment OU fs_u(V in [[stage_in]], constant uint4 &c [[buffer(0)]]) { OU o; o.c = c; return o; }\n"
    "fragment void fs_none(V in [[stage_in]]) {}\n";

static id<MTLLibrary> clear_lib(int index) {
    static id<MTLLibrary> libs[8];
    if (!libs[index]) {
        NSString *src = [@(clear_src) stringByReplacingOccurrencesOfString:@"INDEX" withString:[NSString stringWithFormat:@"%d", index]];
        NSError *err;
        libs[index] = TIMED_COMPILE([dev newLibraryWithSource:src options:nil error:&err]);
        if (!libs[index]) FATAL("clear shader: %s\n", err.localizedDescription.UTF8String);
    }
    return libs[index];
}

static void attachment_formats(MTLRenderPipelineDescriptor *pd) {
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i]) pd.colorAttachments[i].pixelFormat = view_texture(st.rt.color[i], &st.rt.cview[i], st.rt.has_cview[i], true, nil).pixelFormat;
    if (st.rt.depth) {
        MTLPixelFormat pf = renderer_pixel_format(st.rt.depth->format);
        if (renderer_format_is_depth(st.rt.depth->format) && st.rt.depth->format != 0x31) pd.depthAttachmentPixelFormat = pf;
        if (renderer_format_has_stencil(st.rt.depth->format)) pd.stencilAttachmentPixelFormat = pf;
    }
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && st.rt.color[i]->samples > 1) pd.rasterSampleCount = st.rt.color[i]->samples;
}

// A full clear of texture t recorded for its next pass (an earlier one still recorded is replaced: it never ran).
static void td_record(NvnTexture *t, bool is_color, int kind, const void *color, bool depth, float dval, bool stencil, uint32_t sval) {
    renderer_mark_gpu_owned(t);  // (as the pass would: the guest copy is stale from here on)
    TexDefer *e = td_get(t, true);
    if (!e) return;
    if (!e->clear_on) { td_pending++; e->clear_color = e->clear_depth = e->clear_stencil = false; }
    e->clear_on = true;
    if (is_color) {
        e->clear_color = true;
        for (int k = 0; k < 4; k++) e->cc[k] = kind == 0 ? ((const float *)color)[k] : kind == 1 ? ((const int32_t *)color)[k] : ((const uint32_t *)color)[k];
    } else {
        if (depth) { e->clear_depth = true; e->dval = dval; }
        if (stencil) { e->clear_stencil = true; e->sval = sval; }
    }
}

// (shader preparation) attachment_formats from a variant record: the formats the draw's targets would have
static void attachment_formats_rec(MTLRenderPipelineDescriptor *pd, const ShaderRec *r) {
    bool any = false;
    for (int i = 0; i < r->ncolor && i < 8; i++)
        if (r->color[i]) { pd.colorAttachments[i].pixelFormat = renderer_pixel_format(r->cview[i] ? r->cview[i] : r->color[i]); any = true; }
    if (r->depth) {
        MTLPixelFormat pf = renderer_pixel_format(r->depth);
        if (renderer_format_is_depth(r->depth) && r->depth != 0x31) pd.depthAttachmentPixelFormat = pf;
        if (renderer_format_has_stencil(r->depth)) pd.stencilAttachmentPixelFormat = pf;
    }
    if (any && r->samples > 1) pd.rasterSampleCount = r->samples;
}

// A full clear (whole target, every channel) while no pass is open on the current targets: recorded on the texture for
// its next pass (see TexDefer). Partial clears, view clears and clears into an open pass stay quads.
static bool record_clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil,
                         uint32_t sval, uint32_t smask) {
    static int on = -1;
    if (on < 0) { const char *e = ENV("LARECOMP_MTL_LOAD_CLEAR"); on = !e || atoi(e); }
    if (!on || (enc && !st.rt_dirty)) return false;
    NvnTexture *t = index >= 0 ? (index < st.rt.ncolor ? st.rt.color[index] : NULL) : st.rt.depth;
    if (!t) return false;
    const int32_t *sc = st.scissor[0];
    if (sc[0] != 0 || sc[1] != 0 || sc[2] < (int32_t)t->width || sc[3] < (int32_t)t->height) return false;
    if (index >= 0 ? mask != 0xF : (!depth && !stencil) || (stencil && smask != 0xFF)) return false;
    // (a view of one level or layer: the quad clears just that; a format view covers the whole texture)
    const NvnTextureView *v = index >= 0 ? (st.rt.has_cview[index] ? &st.rt.cview[index] : NULL) : (st.rt.has_dview ? &st.rt.dview : NULL);
    if (v && (((v->setmask & 1) && v->base_level) || ((v->setmask & 2) && v->min_layer))) return false;
    td_record(t, index >= 0, kind, color, depth, dval, stencil, sval);
    if (g_debug_frame) LOG("[clear] recorded idx %d depth %d (%g) stencil %d on texture %p\n", index, depth, dval, stencil, (void *)t);
    return true;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil,
                  uint32_t sval, uint32_t smask) {
    if (record_clear(index, kind, color, mask, depth, dval, stencil, sval, smask)) return;
    begin_pass();
    if (g_debug_frame) {
        int pw, ph;
        pass_size(&pw, &ph);
        LOG("[clear] pass %d idx %d depth %d (%g) stencil %d | pass %dx%d scissor %d,%d %dx%d\n", debug_pass_no, index, depth,
            dval, stencil, pw, ph, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    id<MTLLibrary> lib = clear_lib(index < 0 ? 0 : index);
    pd.vertexFunction = [lib newFunctionWithName:@"vs"];
    static NSString *const fs[] = {@"fs_f", @"fs_i", @"fs_u"};
    pd.fragmentFunction = [lib newFunctionWithName:index >= 0 ? fs[kind] : @"fs_none"];
    attachment_formats(pd);
    for (int i = 0; i < 8; i++) pd.colorAttachments[i].writeMask = MTLColorWriteMaskNone;
    if (index >= 0) {
        MTLColorWriteMask m = 0;
        if (mask & 1) m |= MTLColorWriteMaskRed;
        if (mask & 2) m |= MTLColorWriteMaskGreen;
        if (mask & 4) m |= MTLColorWriteMaskBlue;
        if (mask & 8) m |= MTLColorWriteMaskAlpha;
        pd.colorAttachments[index].writeMask = m;
    }
    NSError *err;
    NSString *key = [NSString stringWithFormat:@"clear %d %d %u %d %d %lu %lu %lu", index, kind, mask, depth, stencil,
                     (unsigned long)pd.colorAttachments[index < 0 ? 0 : index].pixelFormat,
                     (unsigned long)pd.depthAttachmentPixelFormat, (unsigned long)pd.stencilAttachmentPixelFormat];
    id<MTLRenderPipelineState> pipe = cache[key];
    if (!pipe) {
        pipe = TIMED_COMPILE([dev newRenderPipelineStateWithDescriptor:pd error:&err]);
        if (!pipe) { LOG("[metal] clear pipeline: %s\n", err.localizedDescription.UTF8String); return; }
        cache[key] = pipe;
    }
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = depth;
    if (stencil) {
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = MTLCompareFunctionAlways;
        sd.depthStencilPassOperation = MTLStencilOperationReplace;
        sd.writeMask = smask;
        dd.frontFaceStencil = sd;
        dd.backFaceStencil = sd;
    }
    [enc setRenderPipelineState:pipe];
    [enc setDepthStencilState:[dev newDepthStencilStateWithDescriptor:dd]];
    [enc setStencilReferenceValue:sval];
    [enc setCullMode:MTLCullModeNone];
    int w, h;
    pass_size(&w, &h);
    [enc setViewport:(MTLViewport){0, 0, w, h, 0, 1}];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    apply_scissor();
    float d4[4] = {dval, 0, 0, 0};
    [enc setVertexBytes:d4 length:16 atIndex:0];
    if (index >= 0) [enc setFragmentBytes:color length:16 atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

// ------------------------------------------------------------------ shaders (debug dump)
static void dump_program(NvnProgram *p) {
    if (!getenv("LARECOMP_DUMP_SHADERS")) return;
    static int n;
    mkdir("build/shaders", 0755);
    for (int s = 0; s < 6; s++) {
        if (!(p->stage_mask & (1u << s))) continue;
        char path[256];
        snprintf(path, sizeof path, "build/shaders/%llx_s%d_code.bin", p->stages[s].code_addr, s);
        FILE *f = fopen(path, "wb");
        fwrite((void *)(uintptr_t)p->stages[s].code_addr, 1, 0x10000, f);
        fclose(f);
        snprintf(path, sizeof path, "build/shaders/%llx_s%d_ctrl.bin", p->stages[s].code_addr, s);
        f = fopen(path, "wb");
        fwrite(p->stages[s].control, 1, 0x4000, f);
        fclose(f);
    }
    n++;
}

// ------------------------------------------------------------------ draw support
uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
static int cbuf_base = 3, tex_base = 8;

@interface Prog : NSObject {
@public
    id<MTLFunction> vs, fs;
    ShaderInfo *vi, *fi;
    bool failed;
    _Atomic bool ready;  // functions compiled (async compile: draws are skipped until then)
    ShaderFp fp[2];      // (stale-program detector, shader_prep.h; render thread only)
}
@end
@implementation Prog
@end

static id<MTLBuffer> ring;
static uint64_t ring_off;
#define RING_SIZE (64u << 20)

static uint64_t ring_alloc(const void *data, uint64_t size) {
    if (!ring) ring = [dev newBufferWithLength:RING_SIZE options:MTLResourceStorageModeShared];
    uint64_t aligned = (ring_off + 255) & ~255ull;
    if (aligned + size > RING_SIZE) {
        // Start a fresh ring; command buffers in flight keep the old one alive until they complete.
        ring = [dev newBufferWithLength:(size > RING_SIZE ? size : RING_SIZE) options:MTLResourceStorageModeShared];
        aligned = 0;
    }
    memcpy((uint8_t *)ring.contents + aligned, data, size);
    ring_off = aligned + size;
    return aligned;
}

static void ring_reset(void) { ring_off = 0; }

// (debug) LARECOMP_LOG_BINDS=1: guest memory the GPU reads in place, per frame (bytes bound; distinct ranges once),
// and the ranges of the previous frame whose contents the game rewrote by the time this frame is translated: with
// the frame fence deferred (overlap) the GPU may still read them - the racing data.
static uint64_t range_hash(uint64_t addr, uint64_t size) {
    const uint64_t *w = (const uint64_t *)(uintptr_t)addr;
    uint64_t h = 1469598103934665603ull, n = size / 8;
    for (uint64_t i = 0; i < n; i++) h = (h ^ w[i]) * 1099511628211ull;
    return h;
}
static void bind_note(int kind, uint64_t addr, uint64_t size) {
    static int on = -1;
    if (on < 0) on = ENV("LARECOMP_LOG_BINDS") != NULL;
    if (!on) return;
    extern uint64_t renderer_present_count(void);
    extern int nvn_pool_flags_at(uint64_t addr);
    uint64_t g_cur_fs_hash = 0; (void)g_cur_fs_hash;
    enum { REC = 8192 };
    static struct { uint64_t addr, size, hash, fs; uint8_t kind; } rec[REC];
    static int nrec;
    static uint64_t frame, all[5], uniq[5], wr[5];
    static uint32_t n[5];
    uint64_t f = renderer_present_count();
    if (f != frame) {
        // the previous frame's ranges, rewritten since?
        uint64_t chg[5] = {0}, chgn[5] = {0};
        int shown = 0;
        for (int i = 0; i < nrec; i++) {
            if (range_hash(rec[i].addr, rec[i].size) == rec[i].hash) continue;
            chg[rec[i].kind] += rec[i].size; chgn[rec[i].kind]++;
            if (frame % 120 == 0 && shown++ < 12)
                LOG("[race] frame %llu %s %llx+%llu pool flags %#x fs %016llx\n", (unsigned long long)frame,
                    rec[i].kind == 0 ? "vertex" : rec[i].kind == 1 ? "index" : rec[i].kind == 2 ? "storage" : rec[i].kind == 3 ? "compute" : "gmem-ptr", (unsigned long long)rec[i].addr,
                    (unsigned long long)rec[i].size, nvn_pool_flags_at(rec[i].addr), (unsigned long long)rec[i].fs);
        }
        if (frame % 120 == 0 || chgn[0] + chgn[1] + chgn[2] + chgn[3] + chgn[4])
            LOG("[binds] frame %llu: vertex %llu KB (%llu KB distinct, %u binds), index %llu KB (%llu), storage %llu KB (%llu, %u), compute %llu KB (%llu), gmem-ptr %llu KB (%llu) | in CPU-writable pools: %llu/%llu/%llu/%llu/%llu KB | rewritten since: vertex %llu KB/%llu, index %llu KB/%llu, storage %llu KB/%llu, compute %llu KB/%llu, gmem-ptr %llu KB/%llu\n",
                (unsigned long long)frame, all[0] >> 10, uniq[0] >> 10, n[0], all[1] >> 10, uniq[1] >> 10, all[2] >> 10, uniq[2] >> 10, n[2], all[3] >> 10, uniq[3] >> 10, all[4] >> 10, uniq[4] >> 10,
                wr[0] >> 10, wr[1] >> 10, wr[2] >> 10, wr[3] >> 10, wr[4] >> 10,
                chg[0] >> 10, chgn[0], chg[1] >> 10, chgn[1], chg[2] >> 10, chgn[2], chg[3] >> 10, chgn[3], chg[4] >> 10, chgn[4]);
        nrec = 0; memset(all, 0, sizeof all); memset(uniq, 0, sizeof uniq); memset(n, 0, sizeof n); memset(wr, 0, sizeof wr);
        frame = f;
    }
    all[kind] += size; n[kind]++;
    {   // (LARECOMP_WATCH_ADDR) the frames in which the watched range is bound for the GPU
        extern uint64_t g_watch_lo, g_watch_hi;
        static uint64_t last;
        if (g_watch_lo && addr < g_watch_hi && addr + size > g_watch_lo && last != f) { last = f; LOG("[watch-bind] frame %llu kind %d %llx+%llu\n", (unsigned long long)f, kind, (unsigned long long)addr, (unsigned long long)size); }
    }
    for (int i = 0; i < nrec; i++) if (rec[i].addr == addr && rec[i].size == size) return;
    if (nrec == REC) return;
    rec[nrec].addr = addr; rec[nrec].size = size; rec[nrec].kind = (uint8_t)kind; rec[nrec].fs = g_cur_fs_hash;
    rec[nrec].hash = range_hash(addr, size);
    nrec++;
    uniq[kind] += size;
    int pf = nvn_pool_flags_at(addr);
    if (pf < 0 || !(pf & 1)) wr[kind] += size;  // (distinct bytes in pools the CPU can write: what a snapshot would copy)
}

static void snap_note_vertex_bind(uint64_t addr, uint64_t size);
extern uint64_t renderer_exec_frame(void);
// The game's per-frame geometry (effects, dust, UI quads, BowWow's chain links: 32-1024-byte ranges at changing
// addresses) lives in pools the CPU writes uncached and the GPU reads uncached (NVN flags CPU_UNCACHED | GPU_UNCACHED,
// 0x12 - a 64 MB and a 96 MB pool), a ring the game reuses once the frame's fence has passed. With the fence waited a
// frame late (overlap) the CPU rewrites those vertices while the previous frame's GPU may still read them: a one-frame
// flicker on the path by the Face Shrine (an M3 Max at any resolution; LARECOMP_LOG_BINDS "[race] ... vertex ... pool
// flags 0x12" every sample). So vertex and index ranges in such pools are copied at translation (the ring the uniforms
// use), as the storage snapshots are; static meshes live in cached pools and stay in place. The pool table is small
// (nvn_pool_flags_at), the ranges are; the stats line counts them ("vcopy"). LARECOMP_DYN_COPY=0: off (in place).
static bool dyn_copy(uint64_t addr, uint64_t size) {
    static int on = -1;
    if (on < 0) { const char *x = ENV("LARECOMP_DYN_COPY"); on = x && atoi(x) != 0 && overlap_on(); }  // (off by default: the rewrites are real but not the visible flicker; ~25 KB a frame when on)
    if (!on || size > (1u << 20)) return false;  // (a range over 1 MB isn't per-frame geometry: in place, as before)
    extern int nvn_pool_flags_at(uint64_t addr);
    int pf = nvn_pool_flags_at(addr);
    if ((pf & 0x13) != 0x12) return false;  // CPU_UNCACHED | GPU_UNCACHED, not CPU_NO_ACCESS
    stat_vcopy++; stat_vcopy_bytes += size;
    return true;
}
static void bind_guest(id<MTLRenderCommandEncoder> e, bool vertex_stage, uint64_t addr, uint64_t size, int index) {
    if (addr >= HEAP_BASE && addr + size <= HEAP_BASE + HEAP_MAX && (addr & 3) == 0) {
        bind_note(0, addr, size);  // (LARECOMP_LOG_BINDS: the race detector still reports the rewrites; copied, they are harmless)
        snap_note_vertex_bind(addr, size);
        if (!dyn_copy(addr, size)) {
            if (vertex_stage) [e setVertexBuffer:g_guest_buffer offset:addr - HEAP_BASE atIndex:index];
            else [e setFragmentBuffer:g_guest_buffer offset:addr - HEAP_BASE atIndex:index];
            return;
        }
    }
    uint64_t off = ring_alloc((void *)(uintptr_t)addr, size);
    if (vertex_stage) [e setVertexBuffer:ring offset:off atIndex:index];
    else [e setFragmentBuffer:ring offset:off atIndex:index];
}

static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static MTLVertexFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return MTLVertexFormatUCharNormalized;
    case 0x02: return MTLVertexFormatCharNormalized;
    case 0x03: return MTLVertexFormatUChar;
    case 0x04: return MTLVertexFormatChar;
    case 0x05: return MTLVertexFormatHalf;
    case 0x06: return MTLVertexFormatUShortNormalized;
    case 0x07: return MTLVertexFormatShortNormalized;
    case 0x08: return MTLVertexFormatUShort;
    case 0x09: return MTLVertexFormatShort;
    case 0x0A: return MTLVertexFormatFloat;
    case 0x0B: return MTLVertexFormatUInt;
    case 0x0C: return MTLVertexFormatInt;
    case 0x0D: return MTLVertexFormatUChar2Normalized;
    case 0x0E: return MTLVertexFormatChar2Normalized;
    case 0x0F: return MTLVertexFormatUChar2;
    case 0x10: return MTLVertexFormatChar2;
    case 0x11: return MTLVertexFormatHalf2;
    case 0x12: return MTLVertexFormatUShort2Normalized;
    case 0x13: return MTLVertexFormatShort2Normalized;
    case 0x14: return MTLVertexFormatUShort2;
    case 0x15: return MTLVertexFormatShort2;
    case 0x16: return MTLVertexFormatFloat2;
    case 0x17: return MTLVertexFormatUInt2;
    case 0x18: return MTLVertexFormatInt2;
    case 0x19: return MTLVertexFormatUChar3Normalized;
    case 0x1A: return MTLVertexFormatChar3Normalized;
    case 0x1B: return MTLVertexFormatUChar3;
    case 0x1C: return MTLVertexFormatChar3;
    case 0x1D: return MTLVertexFormatHalf3;
    case 0x1E: return MTLVertexFormatUShort3Normalized;
    case 0x1F: return MTLVertexFormatShort3Normalized;
    case 0x20: return MTLVertexFormatUShort3;
    case 0x21: return MTLVertexFormatShort3;
    case 0x22: return MTLVertexFormatFloat3;
    case 0x23: return MTLVertexFormatUInt3;
    case 0x24: return MTLVertexFormatInt3;
    case 0x25: return MTLVertexFormatUChar4Normalized;
    case 0x26: return MTLVertexFormatChar4Normalized;
    case 0x27: return MTLVertexFormatUChar4;
    case 0x28: return MTLVertexFormatChar4;
    case 0x29: return MTLVertexFormatHalf4;
    case 0x2A: return MTLVertexFormatUShort4Normalized;
    case 0x2B: return MTLVertexFormatShort4Normalized;
    case 0x2C: return MTLVertexFormatUShort4;
    case 0x2D: return MTLVertexFormatShort4;
    case 0x2E: return MTLVertexFormatFloat4;
    case 0x2F: return MTLVertexFormatUInt4;
    case 0x30: return MTLVertexFormatInt4;
    case 0x3D: return MTLVertexFormatUInt1010102Normalized;
    case 0x69: return MTLVertexFormatInt1010102Normalized;
    case 0x3F: return MTLVertexFormatFloatRG11B10;
    case 0x40: return MTLVertexFormatFloatRGB9E5;
    // integer-to-float ("I2F") formats: converted to float by the vertex fetch
    case 0x51: return MTLVertexFormatUChar;   // these read as raw integers converted in shader
    default:
        return MTLVertexFormatFloat4;
    }
}

static bool is_int_format(uint32_t f) { return attr_kind(f) != 0; }

static MTLBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 0x10: return MTLBlendFactorSource1Color;
    case 0x11: return MTLBlendFactorOneMinusSource1Color;
    case 0x12: return MTLBlendFactorSource1Alpha;
    case 0x13: return MTLBlendFactorOneMinusSource1Alpha;
    case 0x61: return MTLBlendFactorBlendColor;
    case 0x62: return MTLBlendFactorOneMinusBlendColor;
    case 0x63: return MTLBlendFactorBlendAlpha;
    case 0x64: return MTLBlendFactorOneMinusBlendAlpha;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[metal] unknown blend factor %#x\n", f);
        return MTLBlendFactorOne;
    }
    }
}

static MTLBlendOperation blend_op(uint8_t e) {
    switch (e) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

static MTLCompareFunction compare_fn(uint8_t f) {
    switch (f) {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

static MTLStencilOperation stencil_op(uint8_t o) {
    switch (o) {
    case 2: return MTLStencilOperationZero;
    case 3: return MTLStencilOperationReplace;
    case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp;
    case 6: return MTLStencilOperationInvert;
    case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

#define PROG_TAB 4096
static struct { uint64_t key; void *prog; } prog_tab[PROG_TAB];

// ------------------------------------------------------------------ binary archive (off; LARECOMP_METAL_ARCHIVE=1: on)
// Off by default (#52): on macOS 26 Metal's archive writer can crash in serializeToURL (dispatch_retain(NULL) in
// materializeAirNTFromOnlineForSlice, seen on an M4) when an entry it queued lacks a part. The preparation keeps macOS's
// own shader cache warm, so the archive only helped when that cache was cleared without a macOS update. Off, a file
// left by an earlier build is removed. (A check of Metal's queued entries before saving: branch metal-archive-crash.)
// cache/shaders.metalarchive: the GPU code of every pipeline built here (prepared or at first use), kept across runs
// (MTLBinaryArchive), used only with a matching cache/shaders.metalarchive.tag (shader_cache_tag: this build, the
// translator, the GPU and the macOS build; another one, or an archive that doesn't load: a new one, silently). Saved
// (temp file + rename) when the preparation ends and 10 s after the last pipeline added at first use.
// What it holds is the cheap half (pipelines from compiled functions: ~1 s of CPU for all 895 variants on an M3 Max);
// compiling the MSL source into functions (~300 s of CPU when macOS's shader cache lost them) still goes through
// macOS's cache. It loads in the background (3-6 s for 43 MB).
// The MSL text cache (cache/shaders/*.metal + a boot warm-up compile) is gone: the preparation on every start keeps
// macOS's cache warm for the listed variants; that folder is removed.
#define ARCHIVE_FILE "cache/shaders.metalarchive"
#define ARCHIVE_TAG ARCHIVE_FILE ".tag"
static id<MTLBinaryArchive> g_archive;
static _Atomic bool g_archive_ready;  // (loaded in the background: archive_open)
static id<MTLBinaryArchive> archive_get(void) { return g_archive_ready ? g_archive : nil; }
static pthread_rwlock_t g_archive_lock = PTHREAD_RWLOCK_INITIALIZER;  // adds: read side (any number); save: write side
static _Atomic int g_archive_dirty, g_archive_hits, g_archive_adds;
static _Atomic uint64_t g_pipe_us, g_lib_us, g_archive_open_us;  // (preparation's log: time in pipeline builds, library
                                                                  // compiles - summed over threads - and the archive's load)
static _Atomic uint64_t g_archive_gen;

static void archive_tag(char *out, size_t n) {
    char gpu[400];
    snprintf(gpu, sizeof gpu, "%s, %s", dev.name.UTF8String, NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);
    shader_cache_tag(out, n, gpu);
}

static void remove_old_text_cache(void) {  // (cache/shaders/*.metal: up to ~130 MB on players' machines)
    DIR *d = opendir("cache/shaders");
    if (!d) return;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[300];
        snprintf(path, sizeof path, "cache/shaders/%s", e->d_name);
        n += unlink(path) == 0;
    }
    closedir(d);
    rmdir("cache/shaders");
    LOG("[metal] removed the old shader text cache (%d files)\n", n);
}

static void archive_save(bool at_exit) {
    if (!archive_get() || !g_archive_dirty) return;
    if (at_exit ? pthread_rwlock_trywrlock(&g_archive_lock) : pthread_rwlock_wrlock(&g_archive_lock)) return;
    int adds = g_archive_dirty;
    g_archive_dirty = 0;
    double t0 = now_ms();
    NSError *err = nil;
    mkdir("cache", 0755);
    remove(ARCHIVE_FILE ".tmp");
    bool ok = [g_archive serializeToURL:[NSURL fileURLWithPath:@(ARCHIVE_FILE ".tmp")] error:&err];
    pthread_rwlock_unlock(&g_archive_lock);
    if (ok) ok = rename(ARCHIVE_FILE ".tmp", ARCHIVE_FILE) == 0;
    if (!ok) { LOG("[metal] shader archive not saved: %s\n", err ? err.localizedDescription.UTF8String : "rename"); return; }
    char tag[700];
    archive_tag(tag, sizeof tag);
    if (!shader_cache_tag_ok(ARCHIVE_TAG, tag)) shader_cache_tag_write(ARCHIVE_TAG, tag);
    struct stat sb;
    LOG("[metal] shader archive saved: %d new pipelines, %.1f MB, %.0f ms\n", adds,
        stat(ARCHIVE_FILE, &sb) == 0 ? sb.st_size / 1048576.0 : 0.0, now_ms() - t0);
}

static void archive_exit(void) { archive_save(true); }

// Loading a large archive takes seconds (3-5 s for ~900 pipelines, 43 MB, on an M3 Max) while macOS's own shader
// cache usually still has them (~0.5 s for all): it loads in the background, and pipelines built before it's there
// go through macOS's cache alone (the archive is for when that cache dropped them, ~20 s then).

static void archive_publish(id<MTLBinaryArchive> a, double t0, bool loaded) {
    g_archive = a;
    g_archive_ready = a != nil;
    g_archive_open_us = (uint64_t)((now_ms() - t0) * 1000);
    struct stat sb;
    if (loaded) LOG("[metal] shader archive loaded: %.1f MB in %.0f ms\n", stat(ARCHIVE_FILE, &sb) == 0 ? sb.st_size / 1048576.0 : 0.0,
                    g_archive_open_us / 1e3);
}

static id<MTLBinaryArchive> archive_new(void) {
    remove(ARCHIVE_FILE);
    remove(ARCHIVE_TAG);
    NSError *err = nil;
    id<MTLBinaryArchive> a = [dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&err];
    if (!a) LOG("[metal] no shader archive: %s\n", err.localizedDescription.UTF8String);
    return a;
}

static void archive_open(void) {  // (at the first pipeline / preparation)
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        double t0 = now_ms();
        remove_old_text_cache();
        remove(ARCHIVE_FILE ".tmp");  // (a save cut short: what it left - Metal writes <uuid>.metallib files beside it)
        DIR *cd = opendir("cache");
        if (cd) {
            struct dirent *e;
            while ((e = readdir(cd))) {
                size_t l = strlen(e->d_name);
                if (l == 36 + 9 && !strcmp(e->d_name + 36, ".metallib")) {
                    char path[300];
                    snprintf(path, sizeof path, "cache/%s", e->d_name);
                    unlink(path);
                }
            }
            closedir(cd);
        }
        const char *on = getenv("LARECOMP_METAL_ARCHIVE");
        if (!on || !atoi(on)) {
            if (remove(ARCHIVE_FILE) == 0) LOG("[metal] removed the shader archive (off)\n");
            remove(ARCHIVE_TAG);
            return;
        }
        atexit(archive_exit);
        char tag[700];
        archive_tag(tag, sizeof tag);
        bool have = access(ARCHIVE_FILE, F_OK) == 0;
        if (!have || !shader_cache_tag_ok(ARCHIVE_TAG, tag)) {
            if (have) LOG("[metal] shader archive from another build / GPU / macOS: a new one\n");
            archive_publish(archive_new(), t0, false);
            return;
        }
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new];
            d.url = [NSURL fileURLWithPath:@(ARCHIVE_FILE)];
            NSError *err = nil;
            id<MTLBinaryArchive> a = [dev newBinaryArchiveWithDescriptor:d error:&err];
            if (!a) LOG("[metal] shader archive unreadable (%s): a new one\n", err.localizedDescription.UTF8String);
            archive_publish(a ? a : archive_new(), t0, a != nil);
        });
    });
}

static void archive_added(void) {  // saved 10 s after the last add (first-use builds come in bursts)
    g_archive_dirty++, g_archive_adds++;
    uint64_t my = ++g_archive_gen;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (g_archive_gen == my) archive_save(false);
    });
}

// A render / compute pipeline through the archive: loaded from it when it's there, else compiled into it (saved
// later) and loaded. Any thread.
static id<MTLRenderPipelineState> archive_render_pipeline_(MTLRenderPipelineDescriptor *pd, NSError **err) {
    archive_open();
    id<MTLBinaryArchive> archive = archive_get();
    if (!archive) return [dev newRenderPipelineStateWithDescriptor:pd error:err];
    pd.binaryArchives = @[archive];
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                    reflection:nil error:nil];
    if (ps) { g_archive_hits++; slow_note_cache_hit(); return ps; }
    bool added = false;  // (not while a save runs: it can take a while - this one goes in a later run)
    if (!pthread_rwlock_tryrdlock(&g_archive_lock)) {
        added = [archive addRenderPipelineFunctionsWithDescriptor:pd error:nil];
        pthread_rwlock_unlock(&g_archive_lock);
    }
    if (added) archive_added();
    return [dev newRenderPipelineStateWithDescriptor:pd error:err];
}

static id<MTLRenderPipelineState> archive_render_pipeline(MTLRenderPipelineDescriptor *pd, NSError **err) {
    double t0 = now_ms();
    id<MTLRenderPipelineState> ps = archive_render_pipeline_(pd, err);
    g_pipe_us += (uint64_t)((now_ms() - t0) * 1000);
    return ps;
}

static id<MTLComputePipelineState> archive_compute_pipeline(id<MTLFunction> f, NSError **err) {
    archive_open();
    MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
    cd.computeFunction = f;
    id<MTLBinaryArchive> archive = archive_get();
    if (!archive) return [dev newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionNone reflection:nil error:err];
    cd.binaryArchives = @[archive];
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                                      reflection:nil error:nil];
    if (ps) { g_archive_hits++; slow_note_cache_hit(); return ps; }
    bool added = false;  // (not while a save runs: it can take a while - this one goes in a later run)
    if (!pthread_rwlock_tryrdlock(&g_archive_lock)) {
        added = [archive addComputePipelineFunctionsWithDescriptor:cd error:nil];
        pthread_rwlock_unlock(&g_archive_lock);
    }
    if (added) archive_added();
    return [dev newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionNone reflection:nil error:err];
}

static bool sync_shaders(void) {  // First-use draws may initialize persistent resources; never discard them by default.
    static int v = -1;
    if (v < 0) { const char *e=getenv("LARECOMP_SYNC_SHADERS"); v=!e || atoi(e)!=0; }
    return v;
}

static void compile_program(Prog *pr) {
    NSError *err = nil;
    static MTLCompileOptions *vopts;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ vopts = [MTLCompileOptions new]; vopts.preserveInvariance = YES; });
    double t0 = now_ms();
    // LARECOMP_SHADER_SALT=<text>: tag sources so Metal's disk cache misses (measure cold first-use compiles)
    NSString *salt = getenv("LARECOMP_SHADER_SALT") ? [NSString stringWithFormat:@"\n// %s\n", getenv("LARECOMP_SHADER_SALT")] : @"";
    id<MTLLibrary> vl = [dev newLibraryWithSource:[@(pr->vi->msl) stringByAppendingString:salt] options:vopts error:&err];
    if (!vl) { LOG("[shader] VS compile failed: %s\n%s\n", err.localizedDescription.UTF8String, pr->vi->msl); pr->failed = true; return; }
    id<MTLLibrary> fl = [dev newLibraryWithSource:[@(pr->fi->msl) stringByAppendingString:salt] options:nil error:&err];
    if (!fl) { LOG("[shader] FS compile failed: %s\n%s\n", err.localizedDescription.UTF8String, pr->fi->msl); pr->failed = true; return; }
    g_lib_us += (uint64_t)((now_ms() - t0) * 1000);
    pr->vs = [vl newFunctionWithName:@"main0"];
    pr->fs = [fl newFunctionWithName:@"main0"];
    __atomic_fetch_add(&stat_compiles, 1, __ATOMIC_RELAXED);
    double dt = now_ms() - t0;
    @synchronized(cache) { stat_compile_ms += dt; }
    slow_note_build("program", (uint64_t)(dt * 1e6));
    pr->ready = true;
}

// Translates a program's two stages into pr (false: failed, logged). vaddr / faddr: the code's guest addresses
// (debug names; 0 when preparing from the game files)
static bool translate_program(Prog *pr, const uint8_t *vcode, size_t vmax, const uint8_t *fcode, size_t fmax,
                              ShaderKey *vkey, ShaderKey *fkey, uint64_t vaddr, uint64_t faddr) {
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    pr->vi = slow_translate(vcode, vmax, STAGE_VERTEX, vkey);
    if (!pr->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", pr->vi->error, vaddr);
        if (vaddr) {
            char path[128];
            snprintf(path, sizeof path, "build/shaders/fail_%llx_s0_code.bin", vaddr);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(vcode, 1, 0x4000, f); fclose(f); }
        }
        pr->failed = true;
        return false;
    }
    fkey->vs_out_mask = pr->vi->out_mask;
    pr->fi = slow_translate(fcode, fmax, STAGE_FRAGMENT, fkey);
    if (!pr->fi->msl) { LOG("[shader] FS translate failed: %s\n", pr->fi->error); pr->failed = true; return false; }
    if (getenv("LARECOMP_SAVE_MSL") && vaddr) {
        mkdir("build/msl", 0755);
        char path[256];
        snprintf(path, sizeof path, "build/msl/%llx_vs.metal", vaddr);
        FILE *f = fopen(path, "w"); fputs(pr->vi->msl, f); fclose(f);
        snprintf(path, sizeof path, "build/msl/%llx_fs.metal", faddr);
        f = fopen(path, "w"); fputs(pr->fi->msl, f); fclose(f);
    }
    return true;
}

// ------------------------------------------------------------------ prepared objects (shader_prep.h)
// Built before the game from the shipped variant list; found by content (code hashes + keys) at first use, as code
// addresses differ from run to run. Kept for the whole process (reboots included). g_prep_lock guards the tables.
static pthread_mutex_t g_prep_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSNumber *, Prog *> *g_prep_progs;  // program content key -> Prog
static NSMutableDictionary<NSString *, id> *g_prep_pipes;       // pipeline key (pipeline_key) -> pipeline state
static NSMutableDictionary<NSNumber *, NSValue *> *g_prep_cs;   // compute content key -> CsProg
static _Atomic int g_prep_pending;                              // builds still running
static dispatch_queue_t g_prep_queue;

static uint64_t prog_content_key(uint64_t vh, uint64_t fh, const ShaderKey *vkey, const ShaderKey *fkey) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint64_t c[2] = {vh, fh};
    const uint8_t *b = (const uint8_t *)c;
    for (size_t i = 0; i < sizeof c; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    b = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    return h;
}

static Prog *prep_find_prog(uint64_t k) {
    if (!title_is_las() || !g_prep_progs) return nil;
    pthread_mutex_lock(&g_prep_lock);
    Prog *pr = g_prep_progs[@(k)];
    pthread_mutex_unlock(&g_prep_lock);
    return pr;
}

static void prog_fp_check(Prog *pr, NvnProgram *p) {  // (the lookups below are by code address: shader_prep.h)
    shader_fp_check(&pr->fp[0], &p->stages[0], "vs");
    shader_fp_check(&pr->fp[1], &p->stages[1], "fs");
}

static Prog *get_program(NvnProgram *p, ShaderKey *vkey, ShaderKey *fkey) {
    if (!(p->stage_mask & 1) || !(p->stage_mask & 2)) return nil;
    // fast path: hash of the code addresses and keys
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) { uint64_t a = p->stages[i].code_addr; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) {
        Prog *fp = (__bridge Prog *)prog_tab[slot].prog;
        prog_fp_check(fp, p);
        return fp->failed ? nil : fp;
    }
    // (every byte of the keys, in hex: NSData's description shows only the first 16 and last 8 bytes of the 52-byte
    // key, so variants differing in attr_kind[16..], depth_minus_one, flip_y or a later field shared one entry -
    // whichever variant came first served the others)
    char kb[2 * (sizeof *vkey + 8) + 1], *kp = kb;
    for (size_t i = 0; i < sizeof *vkey; i++) kp += snprintf(kp, 3, "%02x", vb[i]);
    for (int i = 0; i < 8; i++) kp += snprintf(kp, 3, "%02x", fkey->rt_kind[i]);
    NSString *key = [NSString stringWithFormat:@"%llx/%llx/%s", p->stages[0].code_addr, p->stages[1].code_addr, kb];
    Prog *pr = cache[key];
    if (!pr) {  // prepared before the game (the same code and keys)
        uint64_t vh = shader_stage_hash(&p->stages[0]), fh = shader_stage_hash(&p->stages[1]), ck = prog_content_key(vh, fh, vkey, fkey);
        pr = vh && fh ? prep_find_prog(ck) : nil;
        if (pr) cache[key] = pr;
        else shader_first_visit("program", vh, fh, ck);
    }
    if (pr) {
        prog_tab[slot].key = h;
        prog_tab[slot].prog = (__bridge void *)pr;
        prog_fp_check(pr, p);
        return pr->failed ? nil : pr;
    }
    pr = [Prog new];
    prog_fp_check(pr, p);  // (takes the fingerprint)
    cache[key] = pr;
    prog_tab[slot].key = h;
    prog_tab[slot].prog = (__bridge void *)pr;  // retained by the cache dictionary
    dump_program(p);
    {
        // LARECOMP_DBG_FS=<fs code addr hex>:<pc hex>:<reg>
        static int parsed;
        static unsigned long long dbg_addr;
        static unsigned dbg_pc;
        static int dbg_reg;
        if (!parsed) { parsed = 1; const char *e = getenv("LARECOMP_DBG_FS"); if (e) sscanf(e, "%llx:%x:%d", &dbg_addr, &dbg_pc, &dbg_reg); }
        maxwell_dbg_code = dbg_addr ? (const uint8_t *)(uintptr_t)dbg_addr : NULL;
        static unsigned long long dbg_vs;
        static int vparsed;
        if (!vparsed) { vparsed = 1; const char *e = getenv("LARECOMP_DBG_VS"); if (e) dbg_vs = strtoull(e, NULL, 16); }
        maxwell_dbg_vs = dbg_vs ? (const uint8_t *)(uintptr_t)dbg_vs : NULL;
        maxwell_dbg_pc = dbg_pc;
        maxwell_dbg_reg = dbg_reg;
    }
    // the compiler constants follow the code, so they bound the instruction scan
    size_t vmax = p->stages[0].const_offset ? p->stages[0].const_offset : 0x10000;
    size_t fmax = p->stages[1].const_offset ? p->stages[1].const_offset : 0x10000;
    if (!translate_program(pr, (const uint8_t *)(uintptr_t)p->stages[0].code_addr, vmax,
                           (const uint8_t *)(uintptr_t)p->stages[1].code_addr, fmax, vkey, fkey,
                           p->stages[0].code_addr, p->stages[1].code_addr))
        return nil;
    if (sync_shaders()) {
        compile_program(pr);
        return pr->failed ? nil : pr;
    }
    // First-time compiles take ~40 ms each on Metal: build off the render thread; draws using this program are
    // skipped until it's ready (the object appears a few frames later instead of the game stalling).
    static dispatch_queue_t q;
    if (!q) q = dispatch_queue_create("larecomp.shaders", DISPATCH_QUEUE_CONCURRENT);
    dispatch_async(q, ^{ compile_program(pr); });
    return pr;
}

static id<MTLSamplerState> sampler_for(NvnSamplerDesc *d) {
    if (d->backend) return (__bridge id<MTLSamplerState>)d->backend;
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.magFilter = d->mag_filter == 1 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    uint32_t mf = d->min_filter;
    sd.minFilter = (mf == 1 || mf == 3 || mf == 5) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.mipFilter = mf <= 1 ? MTLSamplerMipFilterNotMipmapped : (mf == 2 || mf == 3) ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    MTLSamplerAddressMode modes[3];
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        case 0: case 7: modes[i] = MTLSamplerAddressModeClampToEdge; break;
        case 1: modes[i] = MTLSamplerAddressModeRepeat; break;
        case 2: case 3: modes[i] = MTLSamplerAddressModeMirrorClampToEdge; break;
        case 4: case 5: modes[i] = MTLSamplerAddressModeClampToBorderColor; break;
        case 6: modes[i] = MTLSamplerAddressModeMirrorRepeat; break;
        default: modes[i] = MTLSamplerAddressModeRepeat;
        }
    }
    sd.sAddressMode = modes[0]; sd.tAddressMode = modes[1]; sd.rAddressMode = modes[2];
    sd.lodMinClamp = d->lod_min;
    sd.lodMaxClamp = d->lod_max > 1000 ? FLT_MAX : d->lod_max;
    if (d->max_aniso > 1) sd.maxAnisotropy = (NSUInteger)fminf(d->max_aniso, 16);
    if (d->lod_bias != 0 && getenv("LARECOMP_LOG_BIAS"))
        LOG("[sampler] lod bias %g (min filter %u, aniso %g)\n", d->lod_bias, mf, d->max_aniso);
    static float lodmin = -1;  // LARECOMP_LOD_MIN=<n>: skip the n sharpest mips everywhere (aliasing experiments)
    if (lodmin < 0) lodmin = getenv("LARECOMP_LOD_MIN") ? (float)atof(getenv("LARECOMP_LOD_MIN")) : 0;
    if (lodmin > 0 && mf > 1) sd.lodMinClamp = fmaxf(sd.lodMinClamp, lodmin);
    static int stest = -1;  // LARECOMP_SAMPLER_TEST: full mip range + 16x aniso on mipmapped samplers (moire test)
    if (stest < 0) stest = getenv("LARECOMP_SAMPLER_TEST") != NULL;
    if (stest && mf > 1) { sd.lodMinClamp = 0; sd.lodMaxClamp = FLT_MAX; sd.maxAnisotropy = 16; }
    if (getenv("LARECOMP_LOG_SAMPLERS"))
        LOG("[sampler] min %u mag %u lod %g..%g aniso %g wrap %u %u\n", mf, d->mag_filter, d->lod_min, d->lod_max, d->max_aniso,
            d->wrap[0], d->wrap[1]);
    if (d->compare_mode) sd.compareFunction = getenv("LARECOMP_NO_SHADOW") ? MTLCompareFunctionAlways : compare_fn((uint8_t)d->compare_func);
    sd.supportArgumentBuffers = NO;
    id<MTLSamplerState> s = [dev newSamplerStateWithDescriptor:sd];
    d->backend = (void *)CFBridgingRetain(s);
    return s;
}

static id<MTLSamplerState> default_sampler(void) {
    static id<MTLSamplerState> s;
    if (!s) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        s = [dev newSamplerStateWithDescriptor:sd];
    }
    return s;
}

static id<MTLTexture> dummy_texture(int type) {
    static id<MTLTexture> t[10];
    if (!t[type]) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.pixelFormat = MTLPixelFormatRGBA8Unorm;
        d.width = d.height = 1;
        d.textureType = type == TEXTYPE_CUBE ? MTLTextureTypeCube : type == TEXTYPE_3D ? MTLTextureType3D
                      : type == TEXTYPE_2D_ARRAY ? MTLTextureType2DArray : type == TEXTYPE_1D ? MTLTextureType1D
                      : MTLTextureType2D;
        t[type] = [dev newTextureWithDescriptor:d];
    }
    return t[type];
}

static MTLTextureType mtl_type_for(int t) {
    switch (t) {
    case TEXTYPE_1D: return MTLTextureType1D;
    case TEXTYPE_3D: return MTLTextureType3D;
    case TEXTYPE_CUBE: return MTLTextureTypeCube;
    case TEXTYPE_2D_ARRAY: return MTLTextureType2DArray;
    case TEXTYPE_CUBE_ARRAY: return MTLTextureTypeCubeArray;
    case TEXTYPE_1D_ARRAY: return MTLTextureType1DArray;
    default: return MTLTextureType2D;
    }
}

// Shadow slots (depth2d, sample_compare) need a depth texture: with nothing usable bound they get a 1x1 Depth32Float
// cleared to 1.0 (lit under the usual less / less-equal compare, as an empty shadow map) instead of the RGBA8 dummy,
// which compared as 0 (in shadow) and is not a depth texture. The Vulkan side does the same (exec.c shadow_slot_check).
static id<MTLTexture> dummy_depth_texture(int type) {
    static id<MTLTexture> t[10];
    if (type < 0 || type >= 10) type = TEXTYPE_2D;
    if (type != TEXTYPE_2D && type != TEXTYPE_2D_ARRAY && type != TEXTYPE_CUBE && type != TEXTYPE_CUBE_ARRAY)
        return dummy_texture(type);  // (3D has no shadow form; 1D depth textures don't exist)
    if (!t[type]) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.pixelFormat = MTLPixelFormatDepth32Float;
        d.width = d.height = 1;
        d.textureType = mtl_type_for(type);
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:d];
        if (!tex) return dummy_texture(type);
        NSUInteger slices = type == TEXTYPE_CUBE || type == TEXTYPE_CUBE_ARRAY ? 6 : 1;
        id<MTLCommandBuffer> cb = [renderer_queue() commandBuffer];  // (committed now: runs before the current one)
        for (NSUInteger s = 0; s < slices; s++) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.depthAttachment.texture = tex;
            rp.depthAttachment.slice = s;
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.depthAttachment.clearDepth = 1.0;
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        }
        [cb commit];
        t[type] = tex;
    }
    return t[type];
}

static bool mtl_depth_comparable(id<MTLTexture> t) {
    switch (t.pixelFormat) {
    case MTLPixelFormatDepth16Unorm: case MTLPixelFormatDepth32Float: case MTLPixelFormatDepth32Float_Stencil8:
        return true;  // (depth targets are never D24S8 here)
    default: return false;
    }
}

// The texture for a shadow slot: tex if it is a comparable depth texture, else the depth dummy (logged once per cause).
static id<MTLTexture> shadow_slot_texture(id<MTLTexture> tex, const ShaderTexture *st_, int i, uint64_t h) {
    if (tex && mtl_depth_comparable(tex) && tex.textureType != MTLTextureType2DMultisample) return tex;
    static bool logged[3];
    int why = !h ? 0 : !tex ? 1 : 2;
    if (!logged[why]) {
        logged[why] = true;
        static const char *names[3] = {"handle 0 (nothing bound)", "no texture (unregistered / finalized / type mismatch)",
                                       "not a comparable depth texture"};
        LOG("[mtl] shadow slot t%d (type %d): %s - handle %#llx, pixel format %lu: 1x1 depth dummy instead (once per cause)\n",
            i, st_->type, names[why], (unsigned long long)h, tex ? (unsigned long)tex.pixelFormat : 0ul);
    }
    return dummy_depth_texture(st_->type);
}

// NVN texture/view component swizzle (same enum values as MTLTextureSwizzle), applied as a view.
static id<MTLTexture> apply_swizzle(id<MTLTexture> tex, NvnTexture *t, const NvnTextureView *v, bool has_view) {
    const uint8_t *sw = (has_view && (v->setmask & 8)) ? v->swizzle : t->swizzle;
    uint8_t c[4];
    for (int i = 0; i < 4; i++) c[i] = sw[i] <= 5 ? sw[i] : (uint8_t)(2 + i);
    if (!c[0] && !c[1] && !c[2] && !c[3]) return tex;  // never set: identity
    if (renderer_format_alpha_one(t->format))
        for (int i = 0; i < 4; i++) if (c[i] == MTLTextureSwizzleAlpha) c[i] = MTLTextureSwizzleOne;
    if (c[0] == 2 && c[1] == 3 && c[2] == 4 && c[3] == (renderer_format_alpha_one(t->format) ? 1 : 5)) return tex;
    if (tex.sampleCount > 1 || renderer_format_is_depth(t->format)) return tex;
    uint32_t swz = c[0] | c[1] << 8 | c[2] << 16 | (uint32_t)c[3] << 24;
    MTLTextureSwizzleChannels chans = MTLTextureSwizzleChannelsMake(c[0], c[1], c[2], c[3]);
    id<MTLTexture> view = view_cached(tex, 0x5000000000000000ull | swz, ^{
        NSUInteger slices = tex.textureType == MTLTextureType3D ? 1 : tex.arrayLength * (tex.textureType == MTLTextureTypeCube || tex.textureType == MTLTextureTypeCubeArray ? 6 : 1);
        return [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:tex.textureType
                                           levels:NSMakeRange(0, tex.mipmapLevelCount) slices:NSMakeRange(0, slices)
                                          swizzle:chans];
    });
    return view;
}

// Upload helper: a separate command buffer, committed now so it runs before the current pass.
static id<MTLTexture> sampled_texture(NvnTexture *t, const NvnTextureView *v, bool has_view, int want_type) {
    // uploads (if the CPU-side contents changed) go into their own command buffer, committed before
    // the current one so they land first
    id<MTLTexture> tex = view_texture(t, v, has_view, false, nil);
    tex = apply_swizzle(tex, t, v, has_view);
    MTLTextureType want = mtl_type_for(want_type);
    if (tex.textureType != want && tex.textureType != MTLTextureType2DMultisample) {
        id<MTLTexture> src = tex;
        if (want == MTLTextureType2D && (tex.textureType == MTLTextureType2DArray || tex.textureType == MTLTextureTypeCube))
            tex = view_cached(src, 0x6000000000000000ull | 1, ^{ return [src newTextureViewWithPixelFormat:src.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, src.mipmapLevelCount) slices:NSMakeRange(0, 1)]; });
        else if (want == MTLTextureType2DArray && tex.textureType == MTLTextureType2D)
            tex = view_cached(src, 0x6000000000000000ull | 2, ^{ return [src newTextureViewWithPixelFormat:src.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0, src.mipmapLevelCount) slices:NSMakeRange(0, 1)]; });
        else if (want == MTLTextureTypeCube && tex.textureType == MTLTextureType2DArray && tex.arrayLength >= 6)
            tex = view_cached(src, 0x6000000000000000ull | 3, ^{ return [src newTextureViewWithPixelFormat:src.pixelFormat textureType:MTLTextureTypeCube levels:NSMakeRange(0, src.mipmapLevelCount) slices:NSMakeRange(0, 6)]; });
        else {
            static int nlog;
            if (nlog++ < 30) LOG("[tex] type mismatch: want %lu have %lu (%ux%ux%u target %u fmt %#x arr %lu)\n", (unsigned long)want,
                                 (unsigned long)tex.textureType, t->width, t->height, t->depth, t->target, t->format, (unsigned long)tex.arrayLength);
            return dummy_texture(want_type);
        }
    }
    return tex;
}


// ---- render-target feedback: a draw that samples one of the pass's own attachments (e.g. depth
// bound for testing while the shader reads it) gets a snapshot copy, as sampling an attachment of the
// current Metal pass is undefined.
static NvnTexture *feedback_tex[4];
static id<MTLTexture> feedback_copy[4];
static int nfeedback;
// Snapshots stay valid until their texture is written again (a draw writing that attachment, a clear, copy or
// compute). Soft particles (grass-cut leaves, smoke) sample the depth buffer they're tested against but don't
// write it, so a burst of them shares one snapshot instead of a pass break + full-screen copy per draw.
static uint64_t g_cur_fs_hash;
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {  // NULL = all
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            if (!g_snap_copies) g_snap_copies = [NSMutableDictionary new];
            NSMutableDictionary *copies = g_snap_copies;
            NSValue *key = [NSValue valueWithPointer:t];
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("LARECOMP_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            // A draw that writes the same depth buffer it samples can't depend on what it reads (that is undefined on the
            // Switch too): seen in the shadow-map pass, whose material shader still declares the shadow map from the
            // main pass. Any earlier snapshot does; no copy, no pass break.
            bool self_write = t == st.rt.depth && st.ds[0] && st.ds[1];
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && copies[key]) {  // unchanged since the last snapshot
                feedback_tex[nfeedback] = t;
                feedback_copy[nfeedback++] = copies[key];
                continue;
            }
            stat_feedback_copies++;
            static int logfb = -1;
            if (logfb < 0) logfb = getenv("LARECOMP_LOG_FEEDBACK") != NULL;
            if (logfb && stat_feedback_copies % 50 == 1)
                LOG("[feedback] copy %s %ux%u fmt %#x stage %d fs %016llx (last invalidated by %s) ds %d/%d/%d stencil %d ncolor %d c0 %ux%u fmt %#x mask %x\n", t == st.rt.depth ? "DEPTH" : "COLOR",
                    t->width, t->height, t->format, sg, (unsigned long long)g_cur_fs_hash, si >= 0 ? snap_why[si] : "new",
                    st.ds[0], st.ds[1], st.ds[2], st.ds[3], st.rt.ncolor, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0,
                    st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->format : 0, st.channel_mask);
            end_pass();
            id<MTLTexture> src = renderer_texture(t, nil, true);
            id<MTLTexture> dst = copies[key];
            if (!dst || dst.width != src.width || dst.height != src.height || dst.pixelFormat != src.pixelFormat) {
                MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:src.width
                                                                                           height:src.height mipmapped:NO];
                d.textureType = src.textureType == MTLTextureType2DMultisample ? MTLTextureType2DMultisample : MTLTextureType2D;
                d.sampleCount = src.sampleCount;
                d.usage = MTLTextureUsageShaderRead;  // (only same-format views of it: compressible)
                d.storageMode = MTLStorageModePrivate;
                dst = [dev newTextureWithDescriptor:d];
                copies[key] = dst;
            }
            id<MTLBlitCommandEncoder> b = [cb() blitCommandEncoder];
            [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(src.width, src.height, 1) toTexture:dst destinationSlice:0 destinationLevel:0
             destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
            feedback_tex[nfeedback] = t;
            feedback_copy[nfeedback++] = dst;
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;  // the pass resumes (loading) after the copy
        }
}

// Shaders with more textures than Metal's sampler slots (MSL_MAX_SAMPLERS; issue #215: a fragment shader of the Ukuku
// Prairie cave with 17) take their samplers as an array of the slots, and a table of each texture's slot
// (maxwell/translate.c). Each distinct sampler state the draw uses goes in a slot of its own - textures sharing a state
// share its slot - so every texture keeps its own filtering, wrapping and compare mode. smp[i] / desc[i]: texture i's
// state and its descriptor (NULL: the default sampler); fills slot[] and map[], returns the slots used.
static int sampler_slots(int n, __strong id<MTLSamplerState> *smp, NvnSamplerDesc *const *desc,
                         __strong id<MTLSamplerState> *slot, uint32_t *map) {
    const NvnSamplerDesc *sdesc[MSL_MAX_SAMPLERS] = {0};
    int ns = 0;
    for (int i = 0; i < n; i++) {
        int k = 0;
        for (; k < ns; k++)  // (one state: the same object, or descriptors alike apart from the cached object)
            if (slot[k] == smp[i] || (desc[i] && sdesc[k] && !memcmp(desc[i], sdesc[k], offsetof(NvnSamplerDesc, backend))))
                break;
        if (k == ns && ns < MSL_MAX_SAMPLERS) { slot[ns] = smp[i]; sdesc[ns++] = desc[i]; }
        else if (k == ns) {  // more distinct states than slots (not seen in the game): the nearest slot - one with the
            static int warned;  // same compare mode, else the last
            if (!warned++) LOG("[metal] a shader with %d textures uses more than %d sampler states: some share one\n", n, MSL_MAX_SAMPLERS);
            uint32_t cmp = desc[i] ? desc[i]->compare_mode : 0;
            for (k = 0; k < ns - 1; k++)
                if ((sdesc[k] ? sdesc[k]->compare_mode : 0) == cmp) break;
        }
        map[i] = (uint32_t)k;
    }
    return ns;
}

static void bind_textures(int stage, ShaderInfo *si) {
    if (stage == 1) {
        draw_bottle = draw_heart = draw_recolour[0] = draw_recolour[1] = 0, draw_item_cursor = false;
        draw_recolour_names[0] = 0;
    }
    bool mapped = si->ntex > MSL_MAX_SAMPLERS;  // (fragment only: the translator refuses such vertex shaders)
    __strong id<MTLSamplerState> msmp[64];
    NvnSamplerDesc *mdesc[64];
    for (int i = 0; i < si->ntex; i++) {
        ShaderTexture *st_ = &si->tex[i];
        int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
        uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
        id<MTLTexture> tex = nil;
        id<MTLSamplerState> smp = nil;
        uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
        tex_sc[stage][i][0] = tex_sc[stage][i][1] = 1;
        if (i < 64) { tex_wh[stage][i][0] = tex_wh[stage][i][1] = 0; tex_fr[stage][i][0] = tex_fr[stage][i][1] = 1; }
        if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
            NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
            if (e->tex) tex = sampled_texture(e->tex, &e->view, e->has_view, st_->type);
            else if (e->dead_format) nvn_dead_slot_sampled(e, tic);
            if (e->tex && stage == 1) {
                extern int renderer_texpatch_of(NvnTexture *t);
                extern int texpatch_bottle_kind(int id);
                extern int texpatch_heart_kind(int id);
                extern bool texpatch_is_item_cursor(int id);
                int id = renderer_texpatch_of(e->tex);
                if (!draw_bottle) draw_bottle = texpatch_bottle_kind(id);
                if (texpatch_bottle_kind(id) == 1) {  // the Hint Sheet: the dungeon map's icon in place (hint_sheet.c)
                    extern bool hint_sheet_icon_draw(const float *ui, uint32_t ui_size);
                    extern NvnTexture *texpatch_hint_icon(void);
                    NvnTexture *ht;
                    if (hint_sheet_icon_draw((const float *)(uintptr_t)st.ubuf[0][0].addr, (uint32_t)st.ubuf[0][0].size) &&
                        (ht = texpatch_hint_icon())) {
                        static NvnTextureView none;
                        tex = sampled_texture(ht, &none, false, st_->type);
                    }
                }
                if (!draw_heart) draw_heart = texpatch_heart_kind(id);
                recolour_add(draw_recolour, recolour_class_of(id));
                if (recolour_name(id)) {
                    size_t n = strlen(draw_recolour_names);
                    snprintf(draw_recolour_names + n, sizeof draw_recolour_names - n, " t%d=%s(%d)", i, recolour_name(id),
                             recolour_class_of(id));
                }
                draw_item_cursor |= texpatch_is_item_cursor(id);
            }
            if (e->tex && i < 64) {
                renderer_texture_scale(e->tex, &tex_sc[stage][i][0], &tex_sc[stage][i][1]);
                renderer_texture_fraction(e->tex, &tex_fr[stage][i][0], &tex_fr[stage][i][1]);
                tex_wh[stage][i][0] = e->tex->width; tex_wh[stage][i][1] = e->tex->height;
            }
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k]) {
                    id<MTLTexture> fc = feedback_copy[k];
                    MTLTextureType want = mtl_type_for(st_->type);
                    if (fc.textureType != want && want == MTLTextureType2DArray)
                        fc = [fc newTextureViewWithPixelFormat:fc.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
                    tex = fc;
                }
            if (g_debug_frame && e->tex && e->tex->addr) {
                uint64_t total = tegra_total_size(&e->tex->layout), nz = 0;
                const uint8_t *p = (const uint8_t *)(uintptr_t)e->tex->addr;
                for (uint64_t k = 0; k < total; k++) nz += p[k] != 0;
                if (e->tex->width == 1024 && e->tex->height == 2048) {
                    static int once;
                    if (!once++ || g_debug_frame) {
                        id<MTLTexture> base = renderer_texture(e->tex, nil, false);
                        LOG("[dtex] glyph cache target %u depth %u layers %u levels %u stride %u flags %#x base type %lu arrlen %lu\n",
                            e->tex->target, e->tex->depth, e->tex->layout.layers, e->tex->levels, e->tex->stride, e->tex->flags,
                            (unsigned long)base.textureType, (unsigned long)base.arrayLength);
                        LOG("[dtex] glyph view: has %d mask %#x levels %u+%u layers %u+%u fmt %#x target %u\n", e->has_view, e->view.setmask,
                            e->view.base_level, e->view.num_levels, e->view.min_layer, e->view.num_layers, e->view.format, e->view.target);
                        LOG("[dtex] glyph cache: mtl fmt %lu type %lu swizzle tex %d %d %d %d view(%d) %d %d %d %d, tex.swz %08x\n",
                            (unsigned long)base.pixelFormat, (unsigned long)tex.textureType, e->tex->swizzle[0], e->tex->swizzle[1],
                            e->tex->swizzle[2], e->tex->swizzle[3], e->has_view && (e->view.setmask & 8), e->view.swizzle[0],
                            e->view.swizzle[1], e->view.swizzle[2], e->view.swizzle[3],
                            (unsigned)(tex.swizzle.red | tex.swizzle.green << 8 | tex.swizzle.blue << 16 | tex.swizzle.alpha << 24));
                        renderer_save_texture(base, "build/glyph_gpu.png");
                        NvnSamplerDesc *sd = (h && st.spool && tsc < (uint32_t)st.spool->count) ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
                        if (sd) { const uint32_t *w = (const uint32_t *)sd; LOG("[dtex] glyph sampler words %08x %08x %08x %08x %08x %08x %08x %08x\n", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]); }
                    }
                }
                if (e->tex->width == 2 && e->tex->height == 2 && e->tex->depth == 6) {
                    static int once2;
                    if (!once2++) {
                        FILE *rf = fopen("build/raw_cube2.bin", "wb");
                        if (rf) { fwrite(p, 1, total, rf); fclose(rf); }
                        LOG("[dtex] cube2 levels %u layers %u bh %u layer_size %llu\n", e->tex->levels, e->tex->layout.layers,
                            e->tex->layout.block_height_log2, tegra_layer_size(&e->tex->layout));
                    }
                }
                if ((e->tex->flags & 0x10) && !getenv("NO_RAW")) {
                    static int nraw;
                    if (nraw++ < 2) {
                        char rp[64];
                        snprintf(rp, sizeof rp, "build/raw_%llx.bin", e->tex->addr);
                        FILE *rf = fopen(rp, "wb");
                        if (rf) { fwrite(p, 1, total, rf); fclose(rf); }
                        LOG("[dtex] linear texture stride %u -> %s\n", e->tex->stride, rp);
                    }
                }
                LOG("[dtex] slot %d tex %ux%u f%#x addr %llx size %llu nonzero %llu gpu_owned %d\n", i, e->tex->width,
                    e->tex->height, e->tex->format, e->tex->addr, total, nz, renderer_is_gpu_owned(e->tex));
            }
        }
        NvnSamplerDesc *sd = h && st.spool && tsc < (uint32_t)st.spool->count ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
        if (sd) smp = sampler_for(sd);
        if (st_->shadow) tex = shadow_slot_texture(tex, st_, i, h);
        if (!tex) tex = dummy_texture(st_->type);
        if (!smp) smp = default_sampler();
        if (g_debug_frame && tex.textureType == MTLTextureType2D && stage == 1) {
            static int dn;
            char path[128], apath[128];
            snprintf(path, sizeof path, "build/dtex_%03d_%d.png", dn++, i);
            snprintf(apath, sizeof apath, "build/dtex_%03d_%d_a.png", dn - 1, i);
            NvnTexture *gt = h && st.tpool && tic < (uint32_t)st.tpool->count ? ((NvnTexEntry *)st.tpool->entries)[tic].tex : NULL;
            if (gt) renderer_save_game_texture(gt, tex, path, apath);
            else { renderer_save_texture(tex, path); renderer_save_texture_alpha(tex, apath); }
        }
        if (stage == 0) { [enc setVertexTexture:tex atIndex:i]; [enc setVertexSamplerState:smp atIndex:i]; }
        else if (mapped) { [enc setFragmentTexture:tex atIndex:i]; msmp[i] = smp; mdesc[i] = sd; }
        else { [enc setFragmentTexture:tex atIndex:i]; [enc setFragmentSamplerState:smp atIndex:i]; }
    }
    if (mapped && stage == 1) {
        __strong id<MTLSamplerState> slot[MSL_MAX_SAMPLERS];
        uint32_t map[64];
        int ns = sampler_slots(si->ntex, msmp, mdesc, slot, map);
        for (int k = 0; k < MSL_MAX_SAMPLERS; k++) [enc setFragmentSamplerState:k < ns ? slot[k] : default_sampler() atIndex:k];
        [enc setFragmentBytes:map length:(NSUInteger)si->ntex * 4 atIndex:SMAP_BUFFER_INDEX];
    }
}

// ---- storage buffers the game rewrites one frame ahead (overlap)
// The game's tiled light / exposure lists: each frame its CPU memcpys the list for the frame after this one into
// the buffer the frame being drawn reads (7281e33500+32640 and 7281e3bd00+2040, alternating by frame; f_7000fe5960
// found with LARECOMP_WATCH_ADDR), which the Switch's fence made safe. With the fence deferred that copy lands while
// the GPU still reads the list: screen-aligned tiles without shadow, a dark saturated picture when the exposure
// program (72a8da6000) reads it - worse the slower the GPU. Other storage data (the skinning program 72a8da0000's
// outputs in slots 6-9, read in place by the exposure program and by draws binding them as vertex data from an
// offset into the pool) is only ever written by the GPU and must stay where the game put it.
// Which is which is decided per memory range, with no wait and no probe of the programs. A range a draw reads as
// storage is shader-consumed game data (the lists): COPY for good - every binder of it, the compute dispatches that
// read or modify it too, works on a copy taken at translation (in a ring at the top of the guest heap range:
// shaders address storage through the heap buffer, and the game's heap never reaches there), refreshed whenever
// its data changes (hash) and otherwise kept, so the GPU's own modifications stay. The CPU's rewrite can't be told
// from the data: standing still it rewrites the same bytes, and the boxes come from the GPU reading the list while
// it is rewritten (an M6 at 4x in the woods), so no hash decides this case. A range only compute slots bind gets a
// one-frame PROBE on its first use in a level (all binders on the copy, the GPU can't touch the original) and its
// next use looks at the original: changed, the CPU rewrites it - COPY; unchanged, a GPU output or static (the
// skinning outputs, which draws then bind as vertex data from an offset into the pool) - IN PLACE for good, probed
// again when the level changes or after 10 s unused. With the write watch below (the default) a compute-only range
// starts in place and is copied from the first CPU store into it instead - the probe is the fallback.
// ~100 KB copied and hashed per frame in the field.
// LARECOMP_OVERLAP_SNAP=0: off (everything in place).
#define SNAP_RING (16u << 20)
enum { RS_PROBE, RS_COPY, RS_PLACE };
typedef struct { uint64_t addr, size, copy, pristine, hash, frame; uint32_t gen, level; uint8_t mode; bool cs_bound, draw_bound; } RangeState;
static RangeState rs_tab[512];
static int nrs;
static uint64_t snap_ring_base, snap_ring_off;
static uint32_t snap_gen = 1;  // (bumped when the table starts over)
static uint32_t rs_level_gen = 1;
static bool snap_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = ENV("LARECOMP_OVERLAP_SNAP");
        on = (!e || atoi(e) != 0) && overlap_on();
        if (on) {
            extern uint64_t kernel_heap_size(void);
            snap_ring_base = HEAP_BASE + HEAP_MAX - SNAP_RING;
            if (kernel_heap_size() > HEAP_MAX - SNAP_RING) { on = 0; LOG("[metal] storage snapshots off: the game's heap fills the range\n"); }
            else LOG("[metal] storage buffers the CPU rewrites: copies at %llx (%u MB ring)\n", (unsigned long long)snap_ring_base, SNAP_RING >> 20);
        }
    }
    return on;
}
static void rs_level_tick(void) {  // (a new level: every range is probed again on its next use)
    extern const char *game_level_name(void);
    static char last[64];
    const char *lv = game_level_name();
    if (lv && strncmp(lv, last, sizeof last - 1)) { snprintf(last, sizeof last, "%s", lv); rs_level_gen++; }
}
static void rs_log(RangeState *e, const char *what, bool cs_slot, uint64_t f) {  // (rare: a new range, a decision)
    static int n;
    if (e->size >= 2040 ? n < 100000 : n++ < 200)  // (the small per-object ranges are many: capped)
        LOG("[snap] %llx+%llu %s (%s bind, frame %llu)\n", (unsigned long long)e->addr, (unsigned long long)e->size, what, cs_slot ? "compute" : "draw", (unsigned long long)f);
}
// A copy lives until its entry remakes it (the original changed) or releases it (unused for 10 s): the ring
// allocator walks past live copies, so a ring wrap no longer remakes every copy from its original (which lost the
// GPU's own modifications of a copy - the exposure program's writes into the lists - for a frame). A slot freed by
// a remake is reused only after the ring comes round (16 MB of copies later), long after the GPU work that read it.
// `need` bytes of the ring (256-aligned), past the live copies (`skip`: the entry being remade may reuse its own
// slot's space only after the merge below read it, so it is skipped too); 0 when the ring is all live copies.
static uint64_t ring_take(uint64_t need, const RangeState *skip) {
    (void)skip;
    uint64_t at = (snap_ring_off + 255) & ~255ull;
    for (int pass = 0; pass < 2; pass++) {
        if (at + need > SNAP_RING) at = 0;
        bool moved = true;
        while (moved && at + need <= SNAP_RING) {
            moved = false;
            for (int i = 0; i < nrs; i++) {
                const RangeState *o = &rs_tab[i];
                if (!o->copy) continue;
                uint64_t olo = o->copy - snap_ring_base, ohi = (olo + o->size * (o->pristine ? 2 : 1) + 255) & ~255ull;
                if (at < ohi && at + need > olo) { at = ohi; moved = true; }
            }
        }
        if (at + need <= SNAP_RING) break;
        at = 0;  // (ran off the end skipping live copies: once more from the start)
    }
    if (at + need > SNAP_RING) return 0;
    snap_ring_off = at + need;
    return snap_ring_base + at;
}
// A copy a compute dispatch binds carries the GPU's own writes into it (the exposure program keeps its adaptation
// state in the 2040-byte exposure list it reads and writes; the lists' tile data beside it is the CPU's). When the
// CPU changes the original the copy is remade from it - and the GPU's state would go back to whatever the original
// held when the copy was first taken: the screen at half brightness for one frame (the path by the Face Shrine, an
// M3 Max at any resolution; two frames three apart, one per list buffer). So such a copy keeps a pristine image of
// what it was copied from right behind it, and a remake merges: a byte the GPU changed (copy != pristine) that the
// CPU didn't (original == pristine) keeps the GPU's value; everything else is the original's, the CPU's write winning
// where both changed (its write is the later one, as in place). Draw-only copies have no GPU state and no image.
static uint64_t rs_copy_make(RangeState *e, uint64_t h) {
    bool keep = e->cs_bound;
    uint64_t need = (e->size * (keep ? 2 : 1) + 255) & ~255ull;
    uint64_t old = e->copy, old_p = e->pristine;
    uint64_t at = ring_take(need, e);
    if (!at) {  // (the ring is all live copies: not in practice - 512 entries, most far smaller)
        static int warned; if (warned++ < 3) LOG("[snap] copy ring full: %llx+%llu in place\n", (unsigned long long)e->addr, (unsigned long long)e->size);
        e->copy = e->pristine = 0; e->hash = h;
        return e->addr;
    }
    uint8_t *dst = (uint8_t *)(uintptr_t)at;
    const uint8_t *src = (const uint8_t *)(uintptr_t)e->addr;
    memcpy(dst, src, e->size);
    if (keep && old && old_p) {
        const uint8_t *a = (const uint8_t *)(uintptr_t)old, *b = (const uint8_t *)(uintptr_t)old_p;
        uint64_t kept = 0;
        for (uint64_t i = 0; i < e->size; i++) if (a[i] != b[i] && src[i] == b[i]) { dst[i] = a[i]; kept++; }
        if (kept) { static int n; if (n++ < 20 || ENV("LARECOMP_LOG_BINDS")) LOG("[snap] %llx+%llu remade: %llu bytes of the GPU's kept (frame %llu)\n", (unsigned long long)e->addr, (unsigned long long)e->size, (unsigned long long)kept, (unsigned long long)renderer_exec_frame()); }
    }
    e->copy = at;
    if (keep) { e->pristine = at + e->size; memcpy((void *)(uintptr_t)e->pristine, src, e->size); }
    else e->pristine = 0;
    e->hash = h, e->gen = snap_gen;
    return e->copy;
}
static void rs_release_unused(uint64_t f) {  // (every 600 frames) copies of entries unbound for 10 s go back to the ring
    static uint64_t last;
    if (f - last < 600) return;
    last = f;
    for (int i = 0; i < nrs; i++) if (rs_tab[i].copy && f - rs_tab[i].frame > 600) rs_tab[i].copy = rs_tab[i].pristine = 0;
}
// Write watch (compute-only ranges). The CPU's rewrite of a range can't be seen in its bytes (the light lists are
// rewritten with the same bytes while the view stands still, and the race is the GPU reading them mid-rewrite), so it
// is caught as it happens: the pages of a range bound only by compute slots are write-protected (plat_protect) when
// the range is first bound in a frame; the first CPU store into them faults (the platform's handler calls the hook
// registered with rt_fault_watch_hook, renderer_watch_fault), the handler opens the page again and, if the store hit inside a watched range, marks that
// range CPU-written: COPY from then on. A store to a watched page outside any range only opens the page. A GPU
// output is never stored to by the CPU and stays in place. A few pages and at most a few faults a frame.
// LARECOMP_OVERLAP_WATCH=0: off (compute-only ranges stay in place, as before).
#define WATCH_PAGES 64
static struct { uint64_t page; volatile int armed; } watch_pages[WATCH_PAGES];
static int nwatch_pages;
static volatile int rs_cpu_wrote[512];  // (by entry index, set by the fault handler: a store hit the range's page)
static uint8_t rs_hit_inside[512];      // (a store was ever seen inside the range itself, not just on its page)
static uint64_t rs_store_frame[512];    // the frame of the previous counted store (a one-off initialisation store of
                                        // a GPU output - BowWow's skinning buffers at his spawn - must not copy it)
bool renderer_watch_fault(uint64_t fa);
static bool watch_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = ENV("LARECOMP_OVERLAP_WATCH");
        on = !e || atoi(e) != 0;
        if (on) { extern void rt_fault_watch_hook(bool (*hook)(uint64_t fa)); rt_fault_watch_hook(renderer_watch_fault); }  // (dispatch.c: faults come here first)
    }
    return on;
}
static uint64_t watch_stat_ns, watch_stat_arms, watch_stat_faults, watch_stat_frame;  // (diagnostics: the cost per frame)
static void watch_stat_tick(uint64_t f) {
    if (f == watch_stat_frame) return;
    if (watch_stat_ns > 2000000 || (ENV("LARECOMP_LOG_BINDS") && f % 120 == 0))
        LOG("[watch] frame %llu: %llu arms, %llu faults, %.2f ms in mprotect\n", (unsigned long long)watch_stat_frame, (unsigned long long)watch_stat_arms,
            (unsigned long long)watch_stat_faults, watch_stat_ns / 1e6);
    watch_stat_frame = f; watch_stat_ns = watch_stat_arms = watch_stat_faults = 0;
}
static void watch_arm_range(const RangeState *e) {
    uint64_t lo = e->addr & ~(PLAT_PAGE - 1), hi = (e->addr + e->size + PLAT_PAGE - 1) & ~(PLAT_PAGE - 1);
    for (uint64_t pg = lo; pg < hi; pg += PLAT_PAGE) {
        int i = 0;
        for (; i < nwatch_pages; i++) if (watch_pages[i].page == pg) break;
        if (i == nwatch_pages) { if (nwatch_pages == WATCH_PAGES) return; watch_pages[nwatch_pages++].page = pg; watch_pages[i].armed = 0; }
        if (!watch_pages[i].armed) {
            uint64_t t0 = plat_now_ns();
            if (plat_protect(pg, PLAT_PAGE, PLAT_PROT_READ) == 0) watch_pages[i].armed = 1;
            watch_stat_ns += plat_now_ns() - t0; watch_stat_arms++;
        }
    }
}
// The platform's fault handler: a store to a watched page (any thread). Opens the page; true when it was ours.
bool renderer_watch_fault(uint64_t fa) {
    uint64_t pg = fa & ~(PLAT_PAGE - 1);
    int i = 0;
    for (; i < nwatch_pages; i++) if (watch_pages[i].page == pg) break;
    if (i == nwatch_pages) return false;
    uint64_t t0 = plat_now_ns();
    plat_protect(pg, PLAT_PAGE, PLAT_PROT_RW);
    watch_stat_ns += plat_now_ns() - t0; watch_stat_faults++;
    watch_pages[i].armed = 0;
    {   // (debug) LARECOMP_LOG_FAULTS=<from>,<to>: every store fault in those frames: address, the range it is in, the storing function
        static long lf = -1, lt; static int nl;
        if (lf < 0) { const char *x = ENV("LARECOMP_LOG_FAULTS"); lf = 0; if (x) sscanf(x, "%ld,%ld", &lf, &lt); }
        uint64_t f = renderer_exec_frame();
        if (lf && (long)f >= lf && (long)f <= lt && nl++ < 400) {
            extern uint64_t g_fault_lr; extern const char *rt_host_sym(uint64_t pc, uint64_t *off);
            uint64_t lo = 0; const char *ls = rt_host_sym(g_fault_lr, &lo);
            int in = -1; for (int k = 0; k < nrs; k++) if (fa >= rs_tab[k].addr && fa < rs_tab[k].addr + rs_tab[k].size) in = k;
            LOG("[fault] frame %llu store at %llx (page %llx) %s %llx+%llu by %s+%llx\n", (unsigned long long)f, (unsigned long long)fa, (unsigned long long)pg,
                in >= 0 ? "inside" : "outside every range; nearest?", in >= 0 ? (unsigned long long)rs_tab[in].addr : 0ull, in >= 0 ? (unsigned long long)rs_tab[in].size : 0ull, ls ? ls : "?", (unsigned long long)lo);
        }
    }
    // The range holding the address is marked. The page stays open until it is armed again, so a store into a
    // neighbour range on it goes unseen: the three light / exposure lists are one contiguous allocation the CPU
    // memcpys in one go and share a page (Kanalet Castle, interiors: the big list's store opened the page, the two
    // small lists' stores were never seen), so a store inside a list marks every watched range on its page too.
    // Not so a store into a small parameter block (the skinning dispatch's 12-byte slot 5, rewritten every frame):
    // it says nothing about the buffers beside it, and marking them copied BowWow's skinning output, which the
    // CPU reads back every frame (f_700102d0f4: the body's chain) - read from the frozen original once the dispatch
    // wrote the copy, the body came apart into spikes as a companion indoors. A store outside every range (per-frame
    // constants next to a buffer) only opens the page. LARECOMP_LOG_BINDS: "[snap] ... never inside" shows what
    // this marked by neighbourhood.
    int x = -1;
    for (int k = 0; k < nrs; k++) if (fa >= rs_tab[k].addr && fa < rs_tab[k].addr + rs_tab[k].size) { x = k; break; }
    if (x >= 0) {
        rs_hit_inside[x] = 1;
        rs_cpu_wrote[x] = 2;  // (the store was inside the range itself)
        if (rs_tab[x].size >= 256)
            for (int k = 0; k < nrs; k++)
                if (k != x && rs_tab[k].addr < pg + PLAT_PAGE && pg < rs_tab[k].addr + rs_tab[k].size && !rs_cpu_wrote[k]) rs_cpu_wrote[k] = 1;
    }
    return true;
}

static void snap_note_vertex_bind(uint64_t addr, uint64_t size) {  // (LARECOMP_LOG_BINDS) reads the original, not the copy
    static int on = -1, n;
    if (on < 0) on = ENV("LARECOMP_LOG_BINDS") != NULL;
    if (!on || n >= 40) return;
    for (int i = 0; i < nrs; i++)
        if (rs_tab[i].mode == RS_COPY && rs_tab[i].copy && addr < rs_tab[i].addr + rs_tab[i].size && addr + size > rs_tab[i].addr) {
            n++; LOG("[snap] vertex bind %llx+%llu overlaps copied %llx+%llu (%s, frame %llu)\n", (unsigned long long)addr, (unsigned long long)size, (unsigned long long)rs_tab[i].addr, (unsigned long long)rs_tab[i].size, addr < rs_tab[i].addr ? "from outside" : "inside", (unsigned long long)renderer_exec_frame());
        }
}
// The address a binder of [addr, addr+size) uses: the original or its copy (cs_slot: bound by a compute dispatch)
static uint64_t snap_storage(uint64_t addr, uint64_t size, bool cs_slot) {
    if (!snap_on() || !addr || !size || size > SNAP_RING / 8 || addr < HEAP_BASE || addr + size > snap_ring_base) return addr;
    extern int nvn_pool_flags_at(uint64_t addr);
    int pf = nvn_pool_flags_at(addr);
    if (pf < 0 || (pf & 1)) return addr;  // (CPU_NO_ACCESS: static data, nothing to race with)
    rs_level_tick();
    extern uint64_t renderer_exec_frame(void);
    uint64_t f = renderer_exec_frame();
    rs_release_unused(f);
    watch_stat_tick(f);
    // A draw's storage read from the game's per-frame pools (CPU_UNCACHED | GPU_UNCACHED, 0x12: the 64-byte per-object
    // blocks at changing addresses, hundreds a frame) is copied without an entry: a fresh range every frame filled the
    // table in a few minutes, and the table starting over remade every copy from its original - the exposure list's
    // GPU state with it (one dark frame per list buffer, see rs_copy_make).
    if (!cs_slot && (pf & 0x13) == 0x12 && size <= 65536) {
        uint64_t at = ring_take((size + 255) & ~255ull, NULL);
        if (!at) return addr;
        memcpy((void *)(uintptr_t)at, (const void *)(uintptr_t)addr, size);
        return at;
    }
    RangeState *e = NULL;
    for (int i = 0; i < nrs; i++) if (rs_tab[i].addr == addr && rs_tab[i].size == size) { e = &rs_tab[i]; break; }
    if (!e) {
        if (nrs == 512) {  // table full: the entry bound longest ago makes room (never the whole table at once)
            int k = 0;
            for (int i = 1; i < nrs; i++) if (rs_tab[i].frame < rs_tab[k].frame) k = i;
            rs_log(&rs_tab[k], "evicted (table full, bound longest ago)", cs_slot, f);
            rs_tab[k].copy = rs_tab[k].pristine = 0;
            e = &rs_tab[k];
        } else e = &rs_tab[nrs++];
        memset(e, 0, sizeof *e);
        // (stamped with this frame: a second binder in the probe frame - the exposure dispatch and the draws read the
        // list the first dispatch wrote - must share the copy, not rehash an original the CPU hasn't had a frame to
        // touch and settle it in place for good; that left the race on the beach at 4x on an M6)
        e->addr = addr, e->size = size, e->mode = RS_PROBE, e->level = rs_level_gen, e->frame = f;
        e->cs_bound = cs_slot;
        rs_cpu_wrote[e - rs_tab] = 0, rs_store_frame[e - rs_tab] = 0, rs_hit_inside[e - rs_tab] = 0;
        {   // (debug) LARECOMP_WATCH_SIZE=<size>,<frame>,<page>,<frames>: the data watchpoint (dispatch.c, any access, the
            // GPU stalls on the protected page) on page <page> of the first range of that size bound from <frame>, for <frames> frames
            static long ws = -1, wf, wp, wn; static bool armed; static uint64_t at;
            if (ws < 0) { const char *x = ENV("LARECOMP_WATCH_SIZE"); ws = 0; if (x) sscanf(x, "%ld,%ld,%ld,%ld", &ws, &wf, &wp, &wn); }
            if (ws && !armed && (long)size == ws && (long)f >= wf) {
                extern uint64_t g_watch_lo, g_watch_hi; extern void watch_arm(void);
                armed = true; at = f; g_watch_lo = addr + wp * PLAT_PAGE; g_watch_hi = addr + size; watch_arm();
                LOG("[snap] data watch on %llx+%llu from %llx (frame %llu)\n", (unsigned long long)addr, (unsigned long long)size, (unsigned long long)g_watch_lo, (unsigned long long)f);
            }
            extern volatile int g_watch_stop;
            if (armed && !g_watch_stop && f > at + wn) { g_watch_stop = 1; LOG("[snap] data watch off (frame %llu)\n", (unsigned long long)f); }
        }
        if (!cs_slot) { e->mode = RS_COPY; e->draw_bound = true; rs_log(e, "new: copy (a draw's storage read)", cs_slot, f); return rs_copy_make(e, range_hash(addr, size)); }
        if (watch_on()) { e->mode = RS_PLACE; watch_arm_range(e); rs_log(e, "new: in place, watched for CPU stores", cs_slot, f); return addr; }
        rs_log(e, "new: probe", cs_slot, f);
        return rs_copy_make(e, range_hash(addr, size));  // the probe frame
    }
    if (cs_slot) e->cs_bound = true; else e->draw_bound = true;
    if (e->mode != RS_COPY && rs_cpu_wrote[e - rs_tab]) {  // a CPU store hit its page since the last bind
        int k = (int)(e - rs_tab);
        int inside = rs_cpu_wrote[k] == 2;
        rs_cpu_wrote[k] = 0;
        if (rs_store_frame[k] && f - rs_store_frame[k] <= 8) {  // stores keep coming: the CPU rewrites it - copied from now on
            e->mode = RS_COPY;
            rs_log(e, inside ? "-> copy (CPU stores keep hitting it)" : rs_hit_inside[k] ? "-> copy (CPU stores keep hitting its page; one was inside)" : "-> copy (CPU stores keep hitting its page, never inside)", cs_slot, f);
            e->frame = f;
            return rs_copy_make(e, range_hash(addr, size));
        }
        rs_store_frame[k] = f;  // (one store so far: an initialisation, or the first of many - the next one decides)
    }
    if (e->frame == f) return e->mode == RS_PLACE ? addr : (e->copy ? e->copy : rs_copy_make(e, e->hash));  // (bound again this frame)
    // probe again on a new level or after 10 s unused - only the hash fallback: a copied range stays copied (a
    // draw's storage read, a CPU store seen), a watched one just gets its pages armed again
    bool again = !watch_on() && e->mode == RS_PLACE && (e->level != rs_level_gen || f - e->frame > 600);
    e->level = rs_level_gen;
    e->frame = f;
    if (!cs_slot && e->mode != RS_COPY) {  // a draw reads it as storage: copied for good (see above)
        e->mode = RS_COPY;
        rs_log(e, "-> copy (a draw's storage read)", cs_slot, f);
        return rs_copy_make(e, range_hash(addr, size));
    }
    if (e->mode == RS_PLACE && e->cs_bound && watch_on()) { watch_arm_range(e); return addr; }  // (watched: a store decides, no hash)
    if (e->mode == RS_PLACE && !again && e->cs_bound) return addr;
    uint64_t h = range_hash(addr, size);
    if (again) { e->mode = RS_PROBE; rs_log(e, "probe again (level / unused)", cs_slot, f); return rs_copy_make(e, h); }
    switch (e->mode) {
    case RS_PLACE:  // (never a compute slot's: a change can only be the CPU's)
        if (h == e->hash) return addr;
        e->mode = RS_COPY;
        rs_log(e, "in place -> copy (changed later)", cs_slot, f);
        return rs_copy_make(e, h);
    case RS_PROBE:  // the use after the probe: did the CPU touch the original meanwhile?
        if (h == e->hash) { e->mode = RS_PLACE; e->copy = e->pristine = 0; rs_log(e, "probe -> in place (unchanged)", cs_slot, f); return addr; }
        e->mode = RS_COPY;
        rs_log(e, "probe -> copy (the CPU rewrote it)", cs_slot, f);
        return rs_copy_make(e, h);
    default:  // RS_COPY
        {   static int on = -1, n; if (on < 0) on = ENV("LARECOMP_LOG_BINDS") != NULL;
            if (on && n < 4000 && size >= 2040) {
                uint64_t hc = e->copy ? range_hash(e->copy, size) : 0;
                static int quiet, gw;  // (every remake of a GPU-written copy - the GPU's state in it is lost; 200 of the GPU-written binds; 40 of the rest)
                if ((e->copy && hc != e->hash && (h != e->hash || gw++ < 200)) || quiet++ < 40) { n++;
                LOG("[snap] copied %llx+%llu at frame %llu: original %s, copy %s (%s bind)\n", (unsigned long long)addr, (unsigned long long)size, (unsigned long long)f,
                    h == e->hash ? "unchanged" : "CHANGED", !e->copy ? "released" : hc == e->hash ? "unchanged" : "GPU-written", cs_slot ? "compute" : "draw"); } }
        }
        if (h == e->hash && e->copy) return e->copy;  // unchanged: the copy (maybe GPU-written: its state) is current
        return rs_copy_make(e, h);
    }
}

static void bind_cbufs(int stage, ShaderInfo *si) {
    for (int k = 0; k < 18; k++) {
        if (!(si->cbuf_mask & (1u << k))) continue;
        if (k == 0) {
            // driver constants: storage buffer bindings {addr_lo, addr_hi, size, 0} in a 0x100-byte table
            // per hardware stage at 0x110 + 0x100 * stage (vertex 0, fragment 4)
            uint32_t drv[0x800 / 4] = {0};
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                uint64_t sa = st.sbuf[stage][i].addr;
                if (sa && (si->ssbo_read & (1u << i))) { bind_note(2, sa, st.sbuf[stage][i].size); sa = snap_storage(sa, st.sbuf[stage][i].size, false); }
                e[0] = (uint32_t)sa;
                e[1] = (uint32_t)(sa >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
            uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
            uint64_t off = ring_alloc(drv, size);
            if (stage == 0) [enc setVertexBuffer:ring offset:off atIndex:0];
            else [enc setFragmentBuffer:ring offset:off atIndex:0];
            continue;
        }
        if (k == 1) {  // compiler constants stored after the shader code
            NvnShaderStage *ss = &st.program->stages[stage];
            uint64_t size = ss->const_size ? ss->const_size : 16;
            if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
            uint64_t off = ring_alloc((void *)(uintptr_t)(ss->code_addr + ss->const_offset), size);
            if (stage == 0) [enc setVertexBuffer:ring offset:off atIndex:1];
            else [enc setFragmentBuffer:ring offset:off atIndex:1];
            continue;
        }
        int idx = k - cbuf_base;
        Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
        uint64_t size = si->cbuf_size[k];
        if (!r.addr) {
            static const uint8_t zeros[0x10000];
            uint64_t off = ring_alloc(zeros, size ? size : 256);
            if (stage == 0) [enc setVertexBuffer:ring offset:off atIndex:k];
            else [enc setFragmentBuffer:ring offset:off atIndex:k];
            continue;
        }
        if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;  // indexed: whole buffer
        if (size > r.size && r.size) size = r.size;
        if (size < 16) size = 16;
        uint64_t off = ring_alloc((void *)(uintptr_t)r.addr, size);  // snapshot, as later CPU writes must not leak in
        if (stage == 0) [enc setVertexBuffer:ring offset:off atIndex:k];
        else [enc setFragmentBuffer:ring offset:off atIndex:k];
    }
}


// ------------------------------------------------------------------ compute
// Compute driver constants (c0): 0x200 work group counts, 0x210 + 16*i uniform buffers (compute has
// few hardware cbuf slots, so the compiler reads the rest through global memory), 0x310 + 16*i
// storage buffers; each table entry is {addr_lo, addr_hi, size, 0}.
typedef struct {
    id<MTLComputePipelineState> pipe;
    ShaderInfo *si;
    uint32_t local[3];
    uint64_t code_addr;
    uint32_t shared_memory;
    bool failed;
    _Atomic bool ready;  // pipeline built (async compile: dispatches are skipped until then, as draws are)
    ShaderFp fp;         // (stale-program detector, shader_prep.h; render thread only)
} CsProg;
static NSMutableDictionary *g_cs_progs;  // code address -> CsProg

static void compile_compute(CsProg *cp) {
    NSError *err = nil;
    double t0 = now_ms();
    NSString *salt = getenv("LARECOMP_SHADER_SALT") ? [NSString stringWithFormat:@"\n// %s\n", getenv("LARECOMP_SHADER_SALT")] : @"";
    id<MTLLibrary> lib = [dev newLibraryWithSource:[@(cp->si->msl) stringByAppendingString:salt] options:nil error:&err];
    id<MTLFunction> f = lib ? [lib newFunctionWithName:@"main0"] : nil;
    id<MTLComputePipelineState> pipe = f ? archive_compute_pipeline(f, &err) : nil;
    @synchronized(cache) { stat_compile_ms += now_ms() - t0; }
    slow_note_build("pipeline cs", (uint64_t)((now_ms() - t0) * 1e6));
    __atomic_fetch_add(&stat_compiles, 1, __ATOMIC_RELAXED);
    if (!pipe) {
        LOG("[compute] compile failed: %s\n%s\n", err.localizedDescription.UTF8String, cp->si->msl);
        cp->failed = true;
        return;
    }
    cp->pipe = pipe;
    if (cp->code_addr) LOG("[compute] program %llx: local size %ux%ux%u, cbufs mask %#x\n", cp->code_addr, cp->local[0], cp->local[1],
        cp->local[2], cp->si->cbuf_mask);
    cp->ready = true;
}

static uint64_t cs_content_key(uint64_t code_hash, const uint32_t local[3], uint32_t shared_memory) {
    uint64_t h = code_hash ^ shared_memory;
    for (int i = 0; i < 3; i++) { h ^= local[i]; h *= 0x100000001B3ull; }
    return h;
}

static bool translate_compute(CsProg *cp, const uint8_t *code, size_t max) {
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    ShaderKey k = {.shared_memory = cp->shared_memory};
    cp->si = slow_translate(code, max, STAGE_COMPUTE, &k);
    if (!cp->si->msl) { LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, cp->code_addr); cp->failed = true; return false; }
    return true;
}

static CsProg *compute_program(NvnProgram *p) {
    NvnShaderStage *ss = &p->stages[5];
    if (!ss->code_addr) return NULL;
    struct { uint64_t addr; uint32_t config[4]; } cache_key = {ss->code_addr, {1, 1, 1, 0}};
    if (ss->control) memcpy(cache_key.config, ss->control + 0x748, sizeof cache_key.config);
    for (int i = 0; i < 3; i++) if (!cache_key.config[i]) cache_key.config[i] = 1;
    NSData *key = [NSData dataWithBytes:&cache_key length:sizeof cache_key];
    NSMutableDictionary *progs = g_cs_progs;
    if (!progs) progs = g_cs_progs = [NSMutableDictionary new];
    NSValue *v = progs[key];
    if (v) {
        CsProg *cp = v.pointerValue;
        shader_fp_check(&cp->fp, ss, "cs");  // (keyed by code address: shader_prep.h)
        return cp->failed ? NULL : cp;
    }
    uint32_t local[3];
    for (int i = 0; i < 3; i++) {
        local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!local[i]) local[i] = 1;
    }
    if (shader_rec_recording()) {
        ShaderRec rec = {.kind = 'C', .vs = shader_stage_hash(ss), .local = {local[0], local[1], local[2]}};
        shader_rec_note(&rec);
    }
    {   // prepared before the game (the same code and local size)
        uint64_t ch = shader_stage_hash(ss);
        CsProg *pc = NULL;
        if (title_is_las() && ch && g_prep_cs) {
            pthread_mutex_lock(&g_prep_lock);
            pc = [g_prep_cs[@(cs_content_key(ch, local, cache_key.config[3]))] pointerValue];
            pthread_mutex_unlock(&g_prep_lock);
        }
        if (pc) {
            progs[key] = [NSValue valueWithPointer:pc];
            shader_fp_check(&pc->fp, ss, "cs");
            return pc->failed ? NULL : pc;
        }
        shader_first_visit("compute", ch, 0, cs_content_key(ch, local, cache_key.config[3]));
    }
    CsProg *cp = calloc(1, sizeof *cp);
    progs[key] = [NSValue valueWithPointer:cp];
    shader_fp_check(&cp->fp, ss, "cs");  // (takes the fingerprint)
    cp->code_addr = ss->code_addr;
    cp->shared_memory = cache_key.config[3];
    memcpy(cp->local, local, sizeof local);
    if (!translate_compute(cp, (const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000)) return NULL;
    if (getenv("LARECOMP_SAVE_MSL") && cp->si && cp->si->msl) {  // (debug, as the draw programs': build/msl/<code addr>_cs.metal)
        mkdir("build/msl", 0755);
        char path[256];
        snprintf(path, sizeof path, "build/msl/%llx_cs.metal", (unsigned long long)ss->code_addr);
        FILE *f = fopen(path, "w"); if (f) { fputs(cp->si->msl, f); fclose(f); }
    }
    if (sync_shaders()) {
        compile_compute(cp);
        return cp->failed ? NULL : cp;
    }
    // compiled off the render thread, as graphics programs are (get_program): the first dispatches are skipped
    static dispatch_queue_t q;
    if (!q) q = dispatch_queue_create("larecomp.compute", DISPATCH_QUEUE_CONCURRENT);
    dispatch_async(q, ^{ compile_compute(cp); });
    return cp;
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate(NULL);
    {   // (debug) LARECOMP_WATCH_ADDR: the data watchpoint (dispatch.c) armed once the game draws
        static bool armed;
        if (!armed) { armed = true; if (ENV("LARECOMP_WATCH_ADDR")) { extern void watch_arm(void); watch_arm(); } }
    }
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (!cp) return;
    if (!cp->ready) { stat_skipped++; return; }
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_TRACE_CS") != NULL;
#define CST(x) do { if (trace) { fprintf(stderr, "[cs] %s\n", x); fflush(stderr); } } while (0)
    CST("end_pass");
    end_pass();
    td_flush_shader_textures(5, cp->si);
    CST("encoder");
    id<MTLComputeCommandEncoder> ce = [cb() computeCommandEncoder];
    CST("pipeline");
    [ce setComputePipelineState:cp->pipe];
    ShaderInfo *si = cp->si;
    for (int k = 0; k < 18; k++) {
        if (!(si->cbuf_mask & (1u << k))) continue;
        uint64_t off;
        if (k == 0) {
            uint32_t drv[0x500 / 4] = {0};  // tables end at 0x310 + 16 * 16
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                if (i < 18) { u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size; }
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                // every slot goes through the snapshot table: inputs the CPU rewrites are read from a copy; a slot the
                // program writes (BowWow's soft-body state, LARECOMP_SSBO_PROBE=1 shows which) is copied only when its
                // pages are shared with CPU-written data, and then its copy holds the GPU's state for good (rs_copy_make)
                uint64_t sa = st.sbuf[5][i].addr;
                if (sa) {
                    static int on = -1, n; if (on < 0) on = ENV("LARECOMP_LOG_BINDS") != NULL;
                    static struct { uint64_t cs, size; int slot; } seen[64]; static int nseen;
                    if (on && n < 64) {
                        uint64_t cs = st.program->stages[5].code_addr; int j = 0;
                        for (; j < nseen; j++) if (seen[j].cs == cs && seen[j].slot == i && seen[j].size == st.sbuf[5][i].size) break;
                        if (j == nseen) { seen[nseen].cs = cs; seen[nseen].slot = i; seen[nseen++].size = st.sbuf[5][i].size; n++;
                            LOG("[snap] compute %llx slot %d %llx+%llu gmem %d store %d (frame %llu)\n", (unsigned long long)cs, i, (unsigned long long)sa, (unsigned long long)st.sbuf[5][i].size, si->uses_gmem, si->uses_gmem_store, (unsigned long long)renderer_exec_frame()); }
                    }
                    sa = snap_storage(sa, st.sbuf[5][i].size, true);
                }
                e[0] = (uint32_t)sa; e[1] = (uint32_t)(sa >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
            off = ring_alloc(drv, sizeof drv);
        } else if (k == 1) {
            NvnShaderStage *ss = &st.program->stages[5];
            uint64_t size = ss->const_size ? ss->const_size : 16;
            if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
            off = ring_alloc((void *)(uintptr_t)(ss->code_addr + ss->const_offset), size);
        } else {
            int idx = k - cbuf_base;
            Range r = (idx >= 0 && idx < 18) ? st.ubuf[5][idx] : (Range){0, 0};
            uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
            if (r.addr) {
                if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
                if (size > r.size && r.size) size = r.size;
                if (size < 16) size = 16;
                off = ring_alloc((void *)(uintptr_t)r.addr, size);
            } else {
                static const uint8_t zeros[0x10000];
                off = ring_alloc(zeros, size);
            }
        }
        [ce setBuffer:ring offset:off atIndex:k];
    }
    CST("gmem");
    if (si->uses_gmem) [ce setBuffer:g_guest_buffer offset:0 atIndex:GMEM_BUFFER_INDEX];
    CST("textures");
    bool cmapped = si->ntex > MSL_MAX_SAMPLERS;
    __strong id<MTLSamplerState> msmp[64];
    NvnSamplerDesc *mdesc[64];
    for (int i = 0; i < si->ntex; i++) {
        ShaderTexture *st_ = &si->tex[i];
        int slot = ((int)st_->handle_offset - 0x20) / 8;
        uint64_t h = (slot >= 0 && slot < 32) ? st.tex[5][slot] : 0;
        id<MTLTexture> tex = nil;
        id<MTLSamplerState> smp = nil;
        uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
        tex_sc[0][i][0] = tex_sc[0][i][1] = 1;
        if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
            NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
            if (e->tex) tex = sampled_texture(e->tex, &e->view, e->has_view, st_->type);
            else if (e->dead_format) nvn_dead_slot_sampled(e, tic);
            if (e->tex && i < 64) { renderer_texture_scale(e->tex, &tex_sc[0][i][0], &tex_sc[0][i][1]); renderer_texture_fraction(e->tex, &tex_fr[0][i][0], &tex_fr[0][i][1]); }
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k]) {
                    id<MTLTexture> fc = feedback_copy[k];
                    MTLTextureType want = mtl_type_for(st_->type);
                    if (fc.textureType != want && want == MTLTextureType2DArray)
                        fc = [fc newTextureViewWithPixelFormat:fc.pixelFormat textureType:MTLTextureType2DArray levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
                    tex = fc;
                }
        }
        NvnSamplerDesc *sd = h && st.spool && tsc < (uint32_t)st.spool->count ? &((NvnSamplerDesc *)st.spool->entries)[tsc] : NULL;
        if (sd) smp = sampler_for(sd);
        if (!smp) smp = default_sampler();
        if (st_->shadow) tex = shadow_slot_texture(tex, st_, i, h);
        [ce setTexture:tex ? tex : dummy_texture(st_->type) atIndex:i];
        if (cmapped) { msmp[i] = smp; mdesc[i] = sd; }
        else [ce setSamplerState:smp atIndex:i];
    }
    if (cmapped) {  // (more textures than sampler slots: as bind_textures)
        __strong id<MTLSamplerState> slot[MSL_MAX_SAMPLERS];
        uint32_t map[64];
        int ns = sampler_slots(si->ntex, msmp, mdesc, slot, map);
        for (int k = 0; k < MSL_MAX_SAMPLERS; k++) [ce setSamplerState:k < ns ? slot[k] : default_sampler() atIndex:k];
        [ce setBytes:map length:(NSUInteger)si->ntex * 4 atIndex:SMAP_BUFFER_INDEX];
    }
    if (si->uses_rscale) {
        float rs[65][4] = {{1, 1, 0, 0}};
        for (int i = 0; i < si->ntex && i < 64; i++) {
            rs[1 + i][0] = tex_sc[0][i][0]; rs[1 + i][1] = tex_sc[0][i][1]; rs[1 + i][2] = tex_fr[0][i][0]; rs[1 + i][3] = tex_fr[0][i][1];
        }
        [ce setBytes:rs length:(NSUInteger)(1 + si->ntex) * 16 atIndex:RSCALE_BUFFER_INDEX];
    }
    CST("dispatch");
    extern bool g_debug_frame;
    if (g_debug_frame) {
        LOG("[dispatch] cs=%llx groups %ux%ux%u ssbo:", st.program->stages[5].code_addr, gx, gy, gz);
        for (int i = 0; i < 16; i++) if (st.sbuf[5][i].addr) LOG(" %d(%llx,%llu)", i, st.sbuf[5][i].addr, st.sbuf[5][i].size);
        LOG(" ubo:");
        for (int i = 0; i < 18; i++) if (st.ubuf[5][i].addr) LOG(" %d(%llx,%llu)", i, st.ubuf[5][i].addr, st.ubuf[5][i].size);
        LOG(" gmem %d\n", si->uses_gmem);
        for (int i = 0; i < 16; i++) if (st.sbuf[5][i].addr && st.sbuf[5][i].size <= 64) {
            const uint32_t *u = (const uint32_t *)(uintptr_t)st.sbuf[5][i].addr; LOG("[dispatch]   ssbo %d words:", i);
            for (uint64_t k = 0; k < st.sbuf[5][i].size / 4; k++) LOG(" %08x", u[k]); LOG("\n");
        }
        for (int i = 0; i < 18; i++) if (st.ubuf[5][i].addr && st.ubuf[5][i].size <= 64) {
            const uint32_t *u = (const uint32_t *)(uintptr_t)st.ubuf[5][i].addr; LOG("[dispatch]   ubo %d words:", i);
            for (uint64_t k = 0; k < st.ubuf[5][i].size / 4; k++) LOG(" %08x", u[k]); LOG("\n");
        }
    }
    // (debug) LARECOMP_SSBO_PROBE=1: which storage slots a compute program writes - run synchronously, slot ranges
    // (and the 64 KB after each) hashed before and after; the slots read by it are noted for the CPU-rewrite check
    static int probe = -1;
    if (probe < 0) probe = ENV("LARECOMP_SSBO_PROBE") != NULL;
    uint64_t before[16][2];
    if (probe) {
        for (int i = 0; i < 16; i++) {
            Range r = st.sbuf[5][i];
            before[i][0] = r.addr ? range_hash(r.addr, r.size) : 0;
            before[i][1] = r.addr && r.addr + r.size + 0x10000 <= HEAP_BASE + HEAP_MAX ? range_hash(r.addr + r.size, 0x10000) : 0;
            if (r.addr) bind_note(3, r.addr, r.size);
        }
    }
    [ce dispatchThreadgroups:MTLSizeMake(gx, gy, gz) threadsPerThreadgroup:MTLSizeMake(cp->local[0], cp->local[1], cp->local[2])];
    [ce endEncoding];
    if (probe) {
        extern int nvn_pool_flags_at(uint64_t addr);
        static struct { uint64_t cs; uint32_t read, written, after, flags_in, flags_w; } seen[64];
        static int nseen;
        renderer_flush();
        uint64_t cs = st.program->stages[5].code_addr;
        int k = -1;
        for (int i = 0; i < nseen; i++) if (seen[i].cs == cs) k = i;
        if (k < 0 && nseen < 64) { k = nseen++; memset(&seen[k], 0, sizeof seen[k]); seen[k].cs = cs; }
        if (k >= 0) {
            uint32_t written = seen[k].written, after = seen[k].after, read = seen[k].read;
            for (int i = 0; i < 16; i++) {
                Range r = st.sbuf[5][i];
                if (!r.addr) continue;
                read |= 1u << i;
                int pf = nvn_pool_flags_at(r.addr);
                if (range_hash(r.addr, r.size) != before[i][0]) { written |= 1u << i; seen[k].flags_w |= pf > 0 ? pf : 0x80000000; }
                else seen[k].flags_in |= pf > 0 ? pf : 0x80000000;
                if (r.addr + r.size + 0x10000 <= HEAP_BASE + HEAP_MAX && range_hash(r.addr + r.size, 0x10000) != before[i][1]) after |= 1u << i;
            }
            if (written != seen[k].written || after != seen[k].after || read != seen[k].read) {
                seen[k].written = written; seen[k].after = after; seen[k].read = read;
                LOG("[probe] cs %llx (gmem store %d): slots %#x written %#x after %#x; pool flags of unwritten %#x written %#x; e.g.", (unsigned long long)cs,
                    cp->si->uses_gmem_store, read, written, after, seen[k].flags_in, seen[k].flags_w);
                for (int i = 0; i < 16; i++) if (st.sbuf[5][i].addr) LOG(" %d=%llx/%llu", i, (unsigned long long)st.sbuf[5][i].addr, (unsigned long long)st.sbuf[5][i].size);
                LOG("\n");
            }
        }
    }
    CST("done");
    stat_dispatches++;
}

static MTLPrimitiveType prim_type(uint32_t mode, bool *ok) {
    *ok = true;
    switch (mode) {
    case 0: return MTLPrimitiveTypePoint;
    case 1: return MTLPrimitiveTypeLine;
    case 3: return MTLPrimitiveTypeLineStrip;
    case 4: return MTLPrimitiveTypeTriangle;
    case 5: return MTLPrimitiveTypeTriangleStrip;
    default: *ok = false; return MTLPrimitiveTypeTriangle;
    }
}

// Expand quads/fans/polygons into a triangle index list (32-bit) referencing the original vertices.
static id<MTLBuffer> expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first,
                                    uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {  // quads
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {  // fan / polygon
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    id<MTLBuffer> b = [dev newBufferWithBytes:out length:n * 4 + 4 options:MTLResourceStorageModeShared];
    free(out);
    return b;
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    static uint64_t dump_fs = 1;
    if (dump_fs == 1) { const char *e = getenv("LARECOMP_DUMP_UBO"); dump_fs = e ? strtoull(e, NULL, 16) : 0; }
    if (dump_fs && st.program && st.program->stages[1].code_addr == dump_fs) {
        for (int i = 0; i < 18; i++) {
            if (!st.ubuf[1][i].addr) continue;
            const float *f = (const float *)(uintptr_t)st.ubuf[1][i].addr;
            LOG("[ubo] f%d (%llu):", i, st.ubuf[1][i].size);
            for (uint64_t k = 0; k < st.ubuf[1][i].size / 4 && k < 1200; k++) LOG(" %g", f[k]);
            LOG("\n");
        }
    }
    static int dump_vs = -1;  // LARECOMP_DUMP_VSUBO: every vertex-stage UBO (first 48 floats) of debug-frame draws
    if (dump_vs < 0) dump_vs = getenv("LARECOMP_DUMP_VSUBO") != NULL;
    for (int i = 0; dump_vs && i < 18; i++) {
        if (!st.ubuf[0][i].addr) continue;
        const float *f = (const float *)(uintptr_t)st.ubuf[0][i].addr;
        LOG("[vsubo] v%d (%llu):", i, st.ubuf[0][i].size);
        for (uint64_t k = 0; k < st.ubuf[0][i].size / 4 && k < 48; k++) LOG(" %g", f[k]);
        LOG("\n");
    }
    if (st.sbuf[0][0].addr && st.sbuf[0][0].size >= 64 && st.sbuf[0][0].size <= 8192) {
        int recw = (st.sbuf[0][0].size % 96 == 0 && st.sbuf[0][0].size % 64 != 0) ? 24 : 16;
        const float *f = (const float *)(uintptr_t)st.sbuf[0][0].addr;
        const uint32_t *u = (const uint32_t *)f;
        for (int g = 0; g < (int)(st.sbuf[0][0].size / (recw * 4)) && g < 6; g++) {
            LOG("[ssbo] rec%d:", g);
            for (int k = 0; k < recw; k++) LOG(" %g(%08x)", f[g * recw + k], u[g * recw + k]);
            LOG("\n");
        }
    }
    LOG("[draw] %s inst %llu", indexed ? "e" : "a", indexed ? r->x[6] : r->x[4]);
    LOG(" %s mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | ubufs:",
        indexed ? "elems" : "arrays", r->x[0], r->x[2], st.program->stages[0].code_addr, st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask);
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 18; i++)
            if (st.ubuf[s][i].addr) LOG(" %c%d(%llx,%llu)", s ? 'f' : 'v', i, st.ubuf[s][i].addr, st.ubuf[s][i].size);
    LOG(" | fubo0:");
    for (int i = 0; i < 6; i++)
        if (st.ubuf[1][i].addr) { const float *f = (const float *)(uintptr_t)st.ubuf[1][i].addr; LOG(" f%d=[%g %g %g %g]", i, f[0], f[1], f[2], f[3]); }
    LOG(" | ssbo:");
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < 16; i++)
            if (st.sbuf[sg][i].addr) LOG(" %c%d(%llx,%llu)", sg ? 'f' : 'v', i, st.sbuf[sg][i].addr, st.sbuf[sg][i].size);
    LOG(" | tex:");
    for (int i = 0; i < pr->fi->ntex; i++) LOG(" off%u", pr->fi->tex[i].handle_offset);
    LOG(" bound:");
    for (int i = 0; i < 32; i++) {
        if (!st.tex[1][i]) continue;
        uint32_t tic = st.tex[1][i] & 0xFFFFF;
        NvnTexture *t = st.tpool && tic < (uint32_t)st.tpool->count ? ((NvnTexEntry *)st.tpool->entries)[tic].tex : NULL;
        if (t) LOG(" %d=[%ux%ux%u t%u f%#x fl%#x %s]", i, t->width, t->height, t->depth, t->target, t->format, t->flags, t->label);
        else LOG(" %d=%llx", i, st.tex[1][i]);
    }
    LOG(" | rt %d", st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x", st.rt.depth->format);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x streams:", st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask);
    for (int i = 0; i < st.nattrib; i++) LOG(" a%d(f%#x s%u o%u)", i, st.attrib[i] & 0xFF, (st.attrib[i] >> 8) & 0x1F, st.attrib[i] >> 13);
    for (int i = 0; i < 12; i++) if (st.vbuf[i].addr) LOG(" vb%d(%llx,%llu,stride %u div %u)", i, st.vbuf[i].addr, st.vbuf[i].size, st.stream[i][0], st.stream[i][1]);
    if (st.ubuf[0][0].addr && st.ubuf[0][0].size == 560) {
        const float *f = (const float *)(uintptr_t)st.ubuf[0][0].addr;
        LOG("\n  vs ubo0:");
        for (int i = 0; i < 36; i++) LOG("%s[%d] %g %g %g %g", i % 2 ? " " : "\n   ", i, f[i*4], f[i*4+1], f[i*4+2], f[i*4+3]);
    }
    LOG(" blend %#x ds %02x%02x%02x poly %02x%02x scissor %d,%d %dx%d chmask %08x stencil %02x%02x%02x%02x%02x ref %u/%u wm %x vm %x bf %d,%d,%d,%d,%d,%d\n", st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], st.channel_mask, st.ds[3], st.ds[4], st.ds[5], st.ds[6],
        st.ds[7], st.stencil_ref[0], st.stencil_ref[1], st.stencil_mask[0], st.stencil_vmask[0], st.blend[0][1], st.blend[0][2],
        st.blend[0][3], st.blend[0][4], st.blend[0][5], st.blend[0][6]);
}


// ---- pipeline cache keyed by a hash of the raw state that shapes the pipeline (avoids building a
// descriptor and a string key per draw)
// Pipelines are keyed by their shader functions' addresses: a reboot frees the old functions, so keys carry the
// boot generation (a build still in flight can't land on a new function at a reused address).
static unsigned g_boot_gen;
static NSMutableDictionary *g_ready_pipes;

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}
#define PIPE_TAB 8192
static struct { uint64_t key; void *pipe; } pipe_tab[PIPE_TAB];

static uint64_t pipeline_state_hash(Prog *pr) {
    uint64_t h = 0xcbf29ce484222325ull;
    h = hmix(h, &pr, sizeof pr);
    h = hmix(h, &st.rt.ncolor, sizeof st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor && i < 8; i++) {
        NvnTexture *t = st.rt.color[i];
        uint32_t v[4] = {t ? t->format : 0, t ? t->samples : 0, st.rt.has_cview[i], st.rt.has_cview[i] ? st.rt.cview[i].format | (st.rt.cview[i].setmask << 16) : 0};
        h = hmix(h, v, sizeof v);
        h = hmix(h, st.blend[i], 8);
    }
    uint32_t dfmt = st.rt.depth ? st.rt.depth->format : 0;
    h = hmix(h, &dfmt, 4);
    h = hmix(h, &st.nattrib, sizeof st.nattrib);
    h = hmix(h, st.attrib, sizeof st.attrib[0] * (size_t)(st.nattrib < 32 ? st.nattrib : 32));
    h = hmix(h, &st.nstream, sizeof st.nstream);
    h = hmix(h, st.stream, sizeof st.stream[0] * (size_t)(st.nstream < 16 ? st.nstream : 16));
    h = hmix(h, &st.channel_mask, sizeof st.channel_mask);
    h = hmix(h, &st.color_state[0], sizeof st.color_state[0]);
    h = hmix(h, &st.ms[0xC], 1);
    return h ? h : 1;
}

// The draw's state as a variant record (shader_prep.h): what its program and pipeline depend on, in NVN terms
static void rec_from_state(ShaderRec *r, bool flip_x, uint32_t mode) {
    memset(r, 0, sizeof *r);
    r->kind = 'G';
    r->vs = shader_stage_hash(&st.program->stages[0]);
    r->fs = shader_stage_hash(&st.program->stages[1]);
    r->flags = (nvn_window_origin() == 0 ? SR_FLIP_Y : 0) | (nvn_depth_mode() == 0 ? SR_DEPTH_MINUS_ONE : 0) | (flip_x ? SR_FLIP_X : 0);
    r->nattrib = (uint8_t)(st.nattrib < 16 ? st.nattrib : 16);
    memcpy(r->attrib, st.attrib, sizeof r->attrib[0] * r->nattrib);
    r->nstream = (uint8_t)(st.nstream < 16 ? st.nstream : 16);
    memcpy(r->stream, st.stream, sizeof r->stream[0] * r->nstream);
    r->ncolor = (uint8_t)(st.rt.ncolor < 8 ? st.rt.ncolor : 8);
    r->samples = 1;
    for (int i = 0; i < r->ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        if (!t) continue;
        r->color[i] = t->format;
        if (st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4)) r->cview[i] = st.rt.cview[i].format;
        if (t->samples > 1) r->samples = t->samples;
        r->channel_mask |= st.channel_mask & (0xFu << (i * 4));
        if (((st.color_state[0] >> i) & 1) && !rt_kind(t->format)) {
            r->blend_on |= (uint8_t)(1u << i);
            memcpy(r->blend[i], st.blend[i], 8);
        }
    }
    if (st.rt.depth) {
        r->depth = st.rt.depth->format;
        if (!r->ncolor && st.rt.depth->samples > 1) r->samples = st.rt.depth->samples;
    }
    r->a2c = st.ms[0xC] != 0;
    r->topo = shader_topo_class(mode);
    r->poly_fill = st.poly[2] != 1;
}

// The pipeline a draw needs: everything but the attachment formats (attachment_formats / attachment_formats_rec),
// from the variant record of the draw's state (rec_from_state) or of the shipped list (renderer_prepare)
static void pipeline_desc(MTLRenderPipelineDescriptor *pd, Prog *pr, const ShaderRec *r) {
    pd.vertexFunction = pr->vs;
    pd.fragmentFunction = pr->fs;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    uint32_t used_streams = 0;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        uint32_t a = i < r->nattrib ? r->attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= r->nattrib || fmt == 0) {
            // unbound attribute: read zeros from a constant stream
            vd.attributes[i].format = MTLVertexFormatFloat4;
            vd.attributes[i].offset = 0;
            vd.attributes[i].bufferIndex = 30;
            vd.layouts[30].stepFunction = MTLVertexStepFunctionConstant;
            vd.layouts[30].stepRate = 0;
            vd.layouts[30].stride = 16;
            used_streams |= 1u << 31;
            continue;
        }
        vd.attributes[i].format = vertex_format(fmt);
        vd.attributes[i].offset = off;
        vd.attributes[i].bufferIndex = 18 + stream;
        used_streams |= 1u << stream;
    }
    for (int s = 0; s < 12; s++) {
        if (!(used_streams & (1u << s))) continue;
        uint32_t stride = s < r->nstream ? r->stream[s][0] : 0, div = s < r->nstream ? r->stream[s][1] : 0;
        if (stride == 0) { vd.layouts[18 + s].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[18 + s].stepRate = 0; vd.layouts[18 + s].stride = 16; }
        else {
            vd.layouts[18 + s].stride = (stride + 3) & ~3u;
            if (div) { vd.layouts[18 + s].stepFunction = MTLVertexStepFunctionPerInstance; vd.layouts[18 + s].stepRate = div; }
        }
    }
    pd.vertexDescriptor = vd;
    for (int i = 0; i < r->ncolor; i++) {
        if (!r->color[i]) continue;
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
        uint32_t m = (r->channel_mask >> (i * 4)) & 0xF;
        ca.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) |
                       (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
        if ((r->blend_on >> i) & 1) {  // (the record leaves out blending on integer targets)
            const uint8_t *b = r->blend[i];
            ca.blendingEnabled = YES;
            ca.sourceRGBBlendFactor = blend_factor(b[1]);
            ca.destinationRGBBlendFactor = blend_factor(b[2]);
            ca.sourceAlphaBlendFactor = blend_factor(b[3]);
            ca.destinationAlphaBlendFactor = blend_factor(b[4]);
            ca.rgbBlendOperation = blend_op(b[5]);
            ca.alphaBlendOperation = blend_op(b[6]);
        }
    }
    pd.alphaToCoverageEnabled = r->a2c != 0;
}

// What makes a pipeline (the descriptor's contents): the key of the pipeline caches. (Without the boot generation:
// draws prefix it, prepared pipelines' functions live as long as the process.)
static NSString *pipeline_key(MTLRenderPipelineDescriptor *pd, Prog *pr) {
    NSMutableString *pk = [NSMutableString stringWithFormat:@"%p %p %lu %lu", pr->vs, pr->fs,
                           (unsigned long)pd.depthAttachmentPixelFormat, (unsigned long)pd.stencilAttachmentPixelFormat];
    for (int i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
        [pk appendFormat:@" %lu/%lu/%d/%lu/%lu/%lu/%lu/%lu/%lu", (unsigned long)ca.pixelFormat, (unsigned long)ca.writeMask,
                         ca.blendingEnabled, (unsigned long)ca.sourceRGBBlendFactor, (unsigned long)ca.destinationRGBBlendFactor,
                         (unsigned long)ca.sourceAlphaBlendFactor, (unsigned long)ca.destinationAlphaBlendFactor,
                         (unsigned long)ca.rgbBlendOperation, (unsigned long)ca.alphaBlendOperation];
    }
    MTLVertexDescriptor *vd = pd.vertexDescriptor;
    for (int i = 0; i < 32; i++)
        if (pr->vi->attr_mask & (1u << i))
            [pk appendFormat:@" a%d:%lu/%lu/%lu", i, (unsigned long)vd.attributes[i].format, (unsigned long)vd.attributes[i].offset, (unsigned long)vd.attributes[i].bufferIndex];
    for (int s = 18; s < 31; s++)
        [pk appendFormat:@" l%lu/%lu/%lu", (unsigned long)vd.layouts[s].stride, (unsigned long)vd.layouts[s].stepFunction, (unsigned long)vd.layouts[s].stepRate];
    [pk appendFormat:@" s%lu a%d", (unsigned long)pd.rasterSampleCount, pd.alphaToCoverageEnabled];
    return pk;
}

static id prep_find_pipe(NSString *body) {
    if (!title_is_las() || !g_prep_pipes) return nil;
    pthread_mutex_lock(&g_prep_lock);
    id p = g_prep_pipes[body];
    pthread_mutex_unlock(&g_prep_lock);
    return p;
}

static void draw(CmdRec *r, bool indexed) {
    if (!st.program) { stat_skipped++; return; }
    static bool logged_modes;
    if (!logged_modes) {
        logged_modes = true;
        const char *cb = getenv("LARECOMP_CBUF_BASE"), *tb = getenv("LARECOMP_TEX_BASE");
        if (cb) cbuf_base = atoi(cb);
        if (tb) tex_base = atoi(tb);
    }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    Prog *pr = get_program(st.program, &vkey, &fkey);
    if (!pr || !pr->ready) { stat_skipped++; return; }
    {   // mirror mode (runtime/hle/mirror.c): the scene's composite into the window image is drawn mirrored - the
        // unblended full-screen draw sampling the scene image (the window images also serve as scratch
        // targets earlier in the frame: those draws sample much smaller images; the HUD after it blends)
        extern bool mirror_draw(bool composite);
        extern bool mirror_scene_size(uint32_t sw, uint32_t sh, uint32_t w, uint32_t h);
        bool composite = false;
        if (st.rt.ncolor && st.rt.color[0] && renderer_texture_presented(st.rt.color[0]) && !(st.color_state[0] & 1) &&
            r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
            st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height && pr->fi->ntex) {
            NvnTexture *t0 = shader_texture_object(1, &pr->fi->tex[0]);
            composite = t0 && mirror_scene_size(t0->width, t0->height, st.rt.color[0]->width, st.rt.color[0]->height);
            extern void scene_size_log(bool composite, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h);
            if (t0) scene_size_log(composite, t0->width, t0->height, st.rt.color[0]->width, st.rt.color[0]->height);
        }
        if (photo_mode_active()) {
            bool photo_composite = composite;
            if (st.rt.ncolor && st.rt.color[0] && pr->fi->ntex) {
                NvnTexture *source = shader_texture_object(1, &pr->fi->tex[0]);
                bool full = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                            st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
                photo_composite |= source && photo_scene_composite(st.rt.color[0]->width, st.rt.color[0]->height,
                    source->width, source->height, full, st.color_state[0] & 1, pr->fi->ntex);
            }
            if (photo_mode_skip_draw(renderer_present_count(), st.rt.ncolor && st.rt.color[0] &&
                                     renderer_texture_presented(st.rt.color[0]), photo_composite)) { stat_skipped++; return; }
        }
        if (mirror_draw(composite)) {  // (the vertex flip mirrors a composite drawn with uvs, the fragment one a
            vkey.flip_x = fkey.flip_x = true;  // composite reading pixel positions; get_program keys on vkey)
            pr = get_program(st.program, &vkey, &fkey);
            if (!pr || !pr->ready) { stat_skipped++; return; }
        }
    }
    {   // Interiors/dungeons draw a 16:9 backdrop quad over the still-rendered overworld: frames that
        // draw it are presented cropped to 16:9 (see backend_present).
        static uint64_t last_fs, last_hash;
        uint64_t fs = st.program->stages[1].code_addr;
        if (fs != last_fs) {
            last_fs = fs;
            last_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
        }
        static uint64_t log_hash = 1;
        if (log_hash == 1) log_hash = getenv("LARECOMP_LOG_FSHASH") ? 0 : 2;
        if (!log_hash && g_debug_frame) LOG("[fshash] fs=%llx hash %016llx\n", fs, last_hash);
        {   // LARECOMP_SKIP_FSHASH=h1,h2,...: drop draws using these fragment shaders (find which draw makes what)
            static uint64_t skip[16];
            static int nskip = -1;
            if (nskip < 0) {
                nskip = 0;
                for (const char *p = getenv("LARECOMP_SKIP_FSHASH"); p && *p && nskip < 16;) {
                    skip[nskip++] = strtoull(p, (char **)&p, 16);
                    while (*p == ',') p++;
                }
            }
            for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { stat_skipped++; return; }
            extern bool pot_level_active(void);
            if (last_hash == 0xa2177dbb72197bd9ull && pot_level_active()) { stat_skipped++; return; }  // item twinkle
        }
        g_cur_fs_hash = last_hash;
        extern void widescreen_backdrop_seen(uint64_t hash);
        if (last_hash) widescreen_backdrop_seen(last_hash);
        extern void widescreen_scene_ubo(const float *u, uint64_t size);  // (the camera's view-projection)
        if (st.ubuf[0][5].addr) widescreen_scene_ubo((const float *)(uintptr_t)st.ubuf[0][5].addr, st.ubuf[0][5].size);
        if (last_hash == 0x71fe31bbf03c3515ull && getenv("LARECOMP_LOG_INDOOR")) {
            static uint64_t lf;
            extern uint64_t renderer_present_count(void);
            if (renderer_present_count() != lf) {
                lf = renderer_present_count();
                LOG("[backdrop] frame %llu count %llu rt %ux%u vp %.0f %.0f %.0f %.0f scissor %d %d %d %d\n", (unsigned long long)lf, r->x[2],
                    st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->width : 0, st.rt.ncolor && st.rt.color[0] ? st.rt.color[0]->height : 0,
                    st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3], st.scissor[0][0], st.scissor[0][1],
                    st.scissor[0][2], st.scissor[0][3]);
            }
        }
    }
    static int log_rt_w = -1, log_rt_h;
    if (log_rt_w < 0) { const char *e = getenv("LARECOMP_LOG_RT"); log_rt_w = 0; if (e) sscanf(e, "%dx%d", &log_rt_w, &log_rt_h); }
    static uint64_t log_rt_addr = 1;
    if (log_rt_addr == 1) { const char *e = getenv("LARECOMP_LOG_RT_ADDR"); log_rt_addr = e ? strtoull(e, NULL, 16) : 0; }
    bool addr_hit = false;
    for (int i = 0; log_rt_addr && i < st.rt.ncolor; i++)
        if (st.rt.color[i] && st.rt.color[i]->addr <= log_rt_addr + 0x40000 && st.rt.color[i]->addr + 0x40000 >= log_rt_addr) addr_hit = true;
    if (addr_hit) LOG("[rtaddr] rt0 addr %llx\n", st.rt.color[0] ? st.rt.color[0]->addr : 0);
    if (g_debug_frame || addr_hit || (log_rt_w && st.rt.ncolor && st.rt.color[0] && (int)st.rt.color[0]->width == log_rt_w &&
                          (int)st.rt.color[0]->height == log_rt_h))
        debug_draw(pr, r, indexed);
    if (td_pending) { td_flush_shader_textures(0, pr->vi); td_flush_shader_textures(1, pr->fi); }
    resolve_feedback(pr);
    // this draw's writes make snapshots of its attachments stale
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);  // stencil-only writes: shaders sample depth
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    begin_pass();

    // vertex streams this draw reads (also computed inside the pipeline build on a cache miss)
    uint32_t used_streams = 0;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        if (i >= st.nattrib || (a & 0xFF) == 0) used_streams |= 1u << 31;
        else used_streams |= 1u << ((a >> 8) & 0x1F);
    }
    // pipeline
    uint64_t phash = pipeline_state_hash(pr);
    uint32_t pslot = (uint32_t)(phash ^ (phash >> 29)) & (PIPE_TAB - 1);
    while (pipe_tab[pslot].key && pipe_tab[pslot].key != phash) pslot = (pslot + 1) & (PIPE_TAB - 1);
    id<MTLRenderPipelineState> pipe = pipe_tab[pslot].key ? (__bridge id<MTLRenderPipelineState>)pipe_tab[pslot].pipe : nil;
    if (!pipe) {
    ShaderRec rec;
    rec_from_state(&rec, vkey.flip_x, (uint32_t)r->x[0]);
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    attachment_formats(pd);
    pipeline_desc(pd, pr, &rec);
    NSString *body = pipeline_key(pd, pr);
    NSString *pk = [NSString stringWithFormat:@"%u %@", g_boot_gen, body];
    id pobj = cache[pk];
    NSMutableDictionary *ready_pipes = g_ready_pipes;  // async pipeline builds finished on Metal's threads
    static id pending;
    if (!ready_pipes) { ready_pipes = g_ready_pipes = [NSMutableDictionary new]; pending = [NSObject new]; }
    if (pobj == pending) {
        @synchronized(ready_pipes) {
            id done = ready_pipes[pk];
            if (done) { [ready_pipes removeObjectForKey:pk]; cache[pk] = done; pobj = done; }
        }
        if (pobj == pending) { stat_skipped++; return; }
    }
    if (!pobj) {
        if (shader_rec_recording()) shader_rec_note(&rec);
        pobj = prep_find_pipe(body);  // prepared before the game
        if (pobj) cache[pk] = pobj;
        else shader_first_visit("pipeline", rec.vs, rec.fs, shader_code_hash((const uint8_t *)body.UTF8String, body.length));
    }
    if (!pobj) {
        if (sync_shaders()) {
            NSError *err;
            pobj = TIMED_COMPILE(archive_render_pipeline(pd, &err));
            if (!pobj) {
                static int n;
                if (n++ < 10) LOG("[metal] pipeline failed: %s\n", err.localizedDescription.UTF8String);
                pobj = [NSNull null];
            }
            cache[pk] = pobj;
        } else {
            cache[pk] = pending;
            NSString *k = [pk copy];
            double t0 = now_ms();
            static dispatch_queue_t pq;  // (off the render thread; through the binary archive)
            if (!pq) pq = dispatch_queue_create("larecomp.pipelines", DISPATCH_QUEUE_CONCURRENT);
            dispatch_async(pq, ^{
                NSError *e = nil;
                id<MTLRenderPipelineState> ps = archive_render_pipeline(pd, &e);
                if (!ps) { static int n; if (n++ < 10) LOG("[metal] pipeline failed: %s\n", e.localizedDescription.UTF8String); }
                @synchronized(ready_pipes) { ready_pipes[k] = ps ? (id)ps : (id)[NSNull null]; }
                @synchronized(cache) { stat_compile_ms += now_ms() - t0; }
                slow_note_build("pipeline", (uint64_t)((now_ms() - t0) * 1e6));
                __atomic_fetch_add(&stat_compiles, 1, __ATOMIC_RELAXED);
            });
            stat_skipped++;
            return;
        }
    }
    if (pobj == [NSNull null]) { stat_skipped++; return; }
    pipe = pobj;
    pipe_tab[pslot].key = phash;
    pipe_tab[pslot].pipe = (__bridge void *)pipe;  // retained by the cache dictionary
    }
    [enc setRenderPipelineState:pipe];

    // Decal colour passes (issue #52): the game draws floor decals twice with the same negative polygon offset - a
    // depth pre-pass, then the colour pass with depth test EQUAL and no depth writes. An M4 (macOS 26.4) lands the two
    // passes' biased depths a hair apart, so EQUAL failed: black decals. Those colour passes run as LESS_EQUAL with
    // LARECOMP_BIAS_HW_SCALE times the game's offset (default 2) plus LARECOMP_BIAS_HW_EPS units (default 4): on or in
    // front of their pre-pass. The margin is what the decal's colour may show through: anything in front of the decal
    // by less than it. Key Cavern's entrance threshold (a slab just above the floor's border decal) showed it from
    // 64 units / 4x the offset on; 256 units (the first try) drew the border over the slab. SCALE=1 EPS=0: as before.
    static int nobias = -1;  // LARECOMP_NO_DEPTH_BIAS: ignore the game's polygon offset (A/B tests)
    if (nobias < 0) nobias = getenv("LARECOMP_NO_DEPTH_BIAS") != NULL;
    static float hw_scale = -1, hw_eps;
    if (hw_scale < 0) {
        hw_scale = getenv("LARECOMP_BIAS_HW_SCALE") ? fmaxf(1, (float)atof(getenv("LARECOMP_BIAS_HW_SCALE"))) : 2;
        hw_eps = getenv("LARECOMP_BIAS_HW_EPS") ? fmaxf(0, (float)atof(getenv("LARECOMP_BIAS_HW_EPS"))) : 4;
    }
    bool decal_le = title_is_las() && (hw_scale > 1 || hw_eps > 0) && (st.poly[3] & 4) && !nobias &&
                    (st.poly_offset[0] < 0 || st.poly_offset[1] < 0) && st.ds[0] && !st.ds[1] && st.ds[2] == 3;
    // depth / stencil
    static uint64_t last_dsk;
    static id<MTLDepthStencilState> last_dss;
    uint64_t dsk = hmix(hmix(hmix(0xcbf29ce484222325ull, st.ds, 8), st.stencil_mask, sizeof st.stencil_mask), st.stencil_vmask, sizeof st.stencil_vmask) ^ decal_le;
    id<MTLDepthStencilState> dss = dsk == last_dsk ? last_dss : nil;
    NSString *dk = nil;
    if (!dss) {
        dk = [NSString stringWithFormat:@"ds %02x%02x%02x%02x%02x%02x%02x%02x %x %x %x %x%s", st.ds[0], st.ds[1], st.ds[2], st.ds[3],
                    st.ds[4], st.ds[5], st.ds[6], st.ds[7], st.stencil_mask[0], st.stencil_vmask[0], st.stencil_mask[1], st.stencil_vmask[1],
                    decal_le ? " le" : ""];
        dss = cache[dk];
    }
    if (!dss) {
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        static int nodt = -1;
        if (nodt < 0) nodt = getenv("LARECOMP_NO_DEPTH_TEST") != NULL;
        if (st.ds[0] && !nodt) {
            dd.depthCompareFunction = decal_le ? MTLCompareFunctionLessEqual : compare_fn(st.ds[2]);
            dd.depthWriteEnabled = st.ds[1] != 0;
        }
        else { dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = NO; }
        if (st.ds[3]) {
            uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
            for (int f = 0; f < 2; f++) {
                MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
                sd.stencilCompareFunction = compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF);
                uint32_t o = f ? ops >> 12 : ops;
                sd.stencilFailureOperation = stencil_op(o & 0xF);
                sd.depthFailureOperation = stencil_op((o >> 4) & 0xF);
                sd.depthStencilPassOperation = stencil_op((o >> 8) & 0xF);
                sd.readMask = st.stencil_vmask[f] & 0xFF;
                sd.writeMask = st.stencil_mask[f] & 0xFF;
                if (f) dd.backFaceStencil = sd; else dd.frontFaceStencil = sd;
            }
        }
        dss = [dev newDepthStencilStateWithDescriptor:dd];
        cache[dk] = dss;
    }
    [enc setDepthStencilState:dss];
    last_dsk = dsk;
    last_dss = dss;
    [enc setStencilFrontReferenceValue:st.stencil_ref[0] backReferenceValue:st.stencil_ref[1]];
    [enc setBlendColorRed:st.blend_color[0] green:st.blend_color[1] blue:st.blend_color[2] alpha:st.blend_color[3]];

    // rasterizer
    uint8_t cull = st.poly[0];
    if (vkey.flip_x) cull = 0;  // (mirror mode's composite: one full-screen quad, either winding)
    [enc setCullMode:cull == 1 ? MTLCullModeFront : cull == 2 ? MTLCullModeBack : MTLCullModeNone];
    bool ccw = st.poly[1] == 1;
    [enc setFrontFacingWinding:(ccw != vkey.flip_y) ? MTLWindingCounterClockwise : MTLWindingClockwise];
    [enc setTriangleFillMode:st.poly[2] == 1 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    {   // polygon offset (decals on floors: without it they z-fight). Enables: bit 2 = fill (NVN PolygonOffsetEnable)
        static float last[3] = {0, 0, 0};
        float f[3] = {0, 0, 0};
        if ((st.poly[3] & 4) && !nobias) memcpy(f, st.poly_offset, sizeof f);
        if (decal_le) {  // (EPS in float units, 2^-24 at depth 0.5..1; a D16 buffer's unit is 2^-16)
            f[0] *= hw_scale;
            f[1] = f[1] * hw_scale - (st.rt.depth && st.rt.depth->format == 0x32 ? hw_eps / 256 : hw_eps);
            f[2] *= hw_scale;
        }
        static int nlog;
        if ((st.poly[3] & 4) && nlog < 3 && getenv("LARECOMP_LOG_BIAS")) { nlog++; LOG("[bias] factor %g units %g clamp %g\n", st.poly_offset[0], st.poly_offset[1], st.poly_offset[2]); }
        if (memcmp(f, last, sizeof f) || enc != last_bias_enc) {
            [enc setDepthBias:f[1] slopeScale:f[0] clamp:f[2]];
            memcpy(last, f, sizeof f);
            last_bias_enc = enc;
        }
    }
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];

    // resources
    bind_cbufs(0, pr->vi);
    bind_cbufs(1, pr->fi);
    // (debug, LARECOMP_LOG_BINDS) shaders loading global memory read through pointers in their uniform buffers: note
    // 4 KB at each heap pointer found in the bound uniforms, to see whether the game rewrites that data per frame
    if (pr->vi->uses_gmem || pr->fi->uses_gmem) {
        static int on = -1;
        if (on < 0) on = ENV("LARECOMP_LOG_BINDS") != NULL;
        if (on) {
            for (int stage = 0; stage < 2; stage++) {
                ShaderInfo *si = stage ? pr->fi : pr->vi;
                if (!si->uses_gmem) continue;
                for (int k = 2; k < 18; k++) {
                    Range r = st.ubuf[stage][k];
                    if (!r.addr || !(si->cbuf_mask & (1u << k))) continue;
                    uint64_t n = (r.size ? r.size : 256) / 8;
                    if (n > 2048) n = 2048;
                    const uint64_t *w = (const uint64_t *)(uintptr_t)r.addr;
                    for (uint64_t i = 0; i < n; i++)
                        if (w[i] >= HEAP_BASE && w[i] + 4096 <= HEAP_BASE + HEAP_MAX && !(w[i] & 15)) bind_note(4, w[i], 4096);
                }
            }
        }
    }
    if (pr->vi->uses_gmem) [enc setVertexBuffer:g_guest_buffer offset:0 atIndex:GMEM_BUFFER_INDEX];
    static id<MTLBuffer> dbgvs;
    bool dbg_this = maxwell_dbg_vs && (const uint8_t *)(uintptr_t)st.program->stages[0].code_addr == maxwell_dbg_vs;
    if (dbg_this) {
        if (!dbgvs) dbgvs = [dev newBufferWithLength:2 * 64 * 8 * 16 options:MTLResourceStorageModeShared];
        memset(dbgvs.contents, 0xff, 2 * 64 * 8 * 16);
        [enc setVertexBuffer:dbgvs offset:0 atIndex:DBG_VS_BUFFER_INDEX];
    }
    if (pr->fi->uses_gmem) [enc setFragmentBuffer:g_guest_buffer offset:0 atIndex:GMEM_BUFFER_INDEX];
    bind_textures(0, pr->vi);
    bind_textures(1, pr->fi);
    extern bool pets_hide_item_cursor(void);  // (the cursor is on the companion card: runtime/hle/pets.c)
    if (draw_item_cursor && pets_hide_item_cursor()) { stat_skipped++; return; }
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image: draws that sample the full-size reshaped scene (the final
        // composite, blurred menu backgrounds) and untextured full-screen fades fill the window; everything
        // else (HUD, text, menus, movie frames) goes into a centred 16:9 box.
        bool reads_scene = false;  // samples a reshaped texture the size of the framebuffer (the scene itself)
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        // Full-screen overlays without textures (fades, flashes) cover the whole window too.
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        extern bool widescreen_ui_pane_partial(const float *u, uint64_t size);  // (a smaller layout pane: boxed UI)
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad &&
                               !widescreen_ui_pane_partial((const float *)(uintptr_t)st.ubuf[0][0].addr, st.ubuf[0][0].size);
        // A full-screen quad sampling a reshaped (screen-shaped) texture is a scene background too, even a
        // downscaled one: the blurred scene behind the pause menu reads a 192x108 copy.
        // (single-texture draws only: the menu's frosted panels sample the same blurred copy but are UI pieces)
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1))
            reads_scene = true;
        extern uint64_t renderer_present_count(void);
        static uint64_t wide_frame = ~0ull;  // the latest draw this frame filled the window (nothing boxed since)
        extern uint64_t renderer_exec_frame(void);  // (the frame being translated)
        if (reads_scene) wide_frame = renderer_exec_frame();
        // Black or white fills are fades and flashes: always the whole window. Coloured ones (the file-select
        // tint) stay boxed unless they directly follow the composite. Colour = first fragment cbuf (rgb).
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_exec_frame());
        if (!composite) wide_frame = ~0ull;
        static int log_fills = -1;  // LARECOMP_LOG_FILLS: every full-screen untextured fill (fs hash, colour cbuf, decision)
        if (log_fills < 0) log_fills = getenv("LARECOMP_LOG_FILLS") != NULL;
        if (log_fills && fullscreen_fill) {
            static uint64_t lastf; static int nthis;
            uint64_t f = renderer_present_count();
            if (f != lastf) { lastf = f; nthis = 0; }
            if (nthis++ < 4 && f % 6 == 0) {
                const float *c = st.ubuf[1][0].addr ? (const float *)(uintptr_t)st.ubuf[1][0].addr : NULL;
                LOG("[fill] frame %llu fs=%llx composite %d cb0 %g %g %g %g | %g %g %g %g\n", (unsigned long long)f,
                    st.program->stages[1].code_addr, composite, c ? c[0] : -1, c ? c[1] : -1, c ? c[2] : -1, c ? c[3] : -1,
                    c ? c[4] : -1, c ? c[5] : -1, c ? c[6] : -1, c ? c[7] : -1);
            }
        }
        if (g_debug_frame && getenv("LARECOMP_LOG_UI")) {
            LOG("[ui2] fs=%llx count %llu ntex %d reads_scene %d fill %d quad %d -> %d |", st.program->stages[1].code_addr, r->x[2],
                pr->fi->ntex, reads_scene, fullscreen_fill, fullscreen_quad, composite);
            for (int i = 0; i < pr->fi->ntex && i < 4; i++)
                LOG(" t%d %dx%d sc %.2f,%.2f", i, tex_wh[1][i][0], tex_wh[1][i][1], tex_sc[1][i][0], tex_sc[1][i][1]);
            LOG(" | rt %ux%u vp %.0f,%.0f %.0fx%.0f\n", st.rt.color[0]->width, st.rt.color[0]->height, st.viewport[0][0],
                st.viewport[0][1], st.viewport[0][2], st.viewport[0][3]);
        }
        if (!composite) {  // centred 16:9 region with a uniform scale
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    {   // camera-attached effects sized for 16:9 are widened with the view (runtime/hle/widescreen.c)
        extern float widescreen_effect_stretch(uint64_t fs_hash);
        float k = widescreen_effect_stretch(g_cur_fs_hash);
        double vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        if (k > 1.0f) { vx -= vw * (k - 1) / 2; vw *= k; }
        [enc setViewport:(MTLViewport){vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf}];
    }
    apply_scissor();
    if (pr->fi->uses_rscale) {  // [0] = render target scale, [1 + i] = scale of texture i
        float rs[66][4] = {{vp_sx, vp_sy, vp_ox, vp_oy}};  // [1 + i] = scale x, y, content part x, y of texture i
        for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
            rs[1 + i][0] = tex_sc[1][i][0]; rs[1 + i][1] = tex_sc[1][i][1]; rs[1 + i][2] = tex_fr[1][i][0]; rs[1 + i][3] = tex_fr[1][i][1];
        }
        if (fkey.flip_x) rs[65][0] = (float)st.rt.color[0]->width;  // (mirror mode: the composite's width, guest pixels)
        [enc setFragmentBytes:rs length:(NSUInteger)(fkey.flip_x ? 66 : 1 + pr->fi->ntex) * 16 atIndex:RSCALE_BUFFER_INDEX];
    }
    // (uniform buffer 2 = the model matrix - only when the vertex shader reads it: a binding left from an earlier draw
    // is another object's matrix)
    bool model_ubo = (pr->vi->cbuf_mask >> (cbuf_base + 2)) & 1;
    if (model_ubo && st.ubuf[0][2].addr && st.ubuf[0][2].size >= 48 && !(st.ubuf[0][2].addr & 3)) {  // cut bushes' items
        extern void pot_item_draw(float *m);                                                 // (runtime/hle/pot_glow.c)
        pot_item_draw((float *)(uintptr_t)st.ubuf[0][2].addr);
    }
    if (ENV("LARECOMP_POT_PROBE") && st.ubuf[0][2].addr && st.ubuf[0][2].size >= 48) {  // (debug: bush / pot draws)
        extern void pot_probe(const float *m, bool tintable, uint64_t mesh, uint32_t inst);
        uint32_t ninst = indexed ? (uint32_t)r->x[6] : (uint32_t)r->x[4];
        pot_probe((const float *)(uintptr_t)st.ubuf[0][2].addr, pr->fi->uses_tint, st.vbuf[0].addr, ninst);
        extern uint64_t renderer_present_count(void);
        static uint64_t pf; static int pn;
        if (renderer_present_count() != pf) { pf = renderer_present_count(); pn = 0; }
        if (ninst > 1 && pf % 300 == 0 && pn++ < 40) {
            const float *m = (const float *)(uintptr_t)st.ubuf[0][2].addr;
            LOG("[potprobe] instanced x%u count %u tint %d ubuf sizes %u %u %u %u %u %u  m2 t (%.2f %.2f %.2f)\n", ninst,
                (uint32_t)r->x[2], pr->fi->uses_tint, (unsigned)st.ubuf[0][0].size, (unsigned)st.ubuf[0][1].size,
                (unsigned)st.ubuf[0][2].size, (unsigned)st.ubuf[0][3].size, (unsigned)st.ubuf[0][4].size,
                (unsigned)st.ubuf[0][5].size, m[3], m[7], m[11]);
            for (int i = 0; i < st.nattrib; i++) LOG(" a%d(f%#x s%u o%u)", i, st.attrib[i] & 0xFF, (st.attrib[i] >> 8) & 0x1F, st.attrib[i] >> 13);
            for (int i = 0; i < 12; i++)
                if (st.vbuf[i].addr && i < st.nstream && st.stream[i][1]) {
                    const float *v = (const float *)(uintptr_t)st.vbuf[i].addr;
                    LOG(" vb%d(size %llu stride %u div %u: %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f)", i,
                        st.vbuf[i].size, st.stream[i][0], st.stream[i][1], v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
                }
            LOG("\n");
            for (int i = 0; i < 4; i++)
                if (st.sbuf[0][i].addr) {
                    const float *f = (const float *)(uintptr_t)st.sbuf[0][i].addr;
                    LOG("   ssbo%d size %llu:", i, st.sbuf[0][i].size);
                    for (int k = 0; k < 48 && k * 4 < (int)st.sbuf[0][i].size; k++) LOG(" %.2f", f[k]);
                    LOG("\n");
                }
        }
    }
    {  // LARECOMP_LOG_RECOLOUR_AT=<frame>[,<frame>...] (test): each draw of those frames sampling a recognised
       // texture (LARECOMP_RECOLOUR_NAMES=1: any of the overworld's), and the class it is tinted with
        static const char *at;
        static int init;
        if (!init) init = 1, at = getenv("LARECOMP_LOG_RECOLOUR_AT");
        extern uint64_t renderer_exec_frame(void);
        if (at && draw_recolour_names[0]) {
            long f = (long)renderer_exec_frame();
            for (const char *p = at; p && *p; p = strchr(p, ','), p = p ? p + 1 : NULL)
                if (atol(p) == f) {
                    LOG("[recolour-draw] f%ld class %d/%d tint %d:%s\n", f, draw_recolour[0], draw_recolour[1], pr->fi->uses_tint,
                        draw_recolour_names);
                    break;
                }
        }
    }
    if (pr->fi->uses_tint) {  // per-draw highlight tint (golden unchecked pots, runtime/hle/pot_glow.c)
        float tint[RECOLOUR_TINT_FLOATS] = {0};
        recolour_tint_draw(draw_recolour, tint);  // (Cosmetics; the highlights below win)
        {   // LARECOMP_RECOLOUR_FLASH=<text> (test, Metal): draws sampling a recognised texture whose name has <text>
            // (LARECOMP_RECOLOUR_NAMES=1: any overworld map) turn magenta - which objects a texture is on
            static const char *fl;
            static int init;
            if (!init) init = 1, fl = getenv("LARECOMP_RECOLOUR_FLASH");
            if (fl && strstr(draw_recolour_names, fl)) {
                memset(tint, 0, sizeof tint);
                recolour_pack(tint, 0.83f, 4.0f, 1.0f, 0.0f, 0.5f, 0.0f, 0.0f);
            }
        }
        extern bool pot_tint(const float *m, float tint[8], uint64_t mesh);
        extern bool pot_glow_active(void);
        if (model_ubo && st.ubuf[0][2].addr && st.ubuf[0][2].size >= 48 && pot_glow_active())
            pot_tint((const float *)(uintptr_t)st.ubuf[0][2].addr, tint, st.vbuf[0].addr);
        if (draw_bottle) {  // the gold Escape Bottle (runtime/hle/escape.c): its item icon / held-up model
            extern void escape_bottle_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]);
            int u = draw_bottle == 3 || draw_bottle == 6 ? 3 : 0;  // (fairy / Piece of Power models: their bone matrices, for where they stand - souls.c, triforce.c)
            escape_bottle_tint(draw_bottle, (const float *)(uintptr_t)st.ubuf[0][u].addr, (uint32_t)st.ubuf[0][u].size, tint);
        }
        if (draw_heart) { extern void texpatch_heart_tint(int kind, float tint[8]); texpatch_heart_tint(draw_heart, tint); }
        [enc setFragmentBytes:tint length:sizeof tint atIndex:19];
    }
    for (int s = 0; s < 12; s++) {
        if (!(used_streams & (1u << s))) continue;
        Range v = st.vbuf[s];
        if (v.addr) bind_guest(enc, true, v.addr, v.size ? v.size : 16, 18 + s);
        else { static const float zero[4]; [enc setVertexBytes:zero length:16 atIndex:18 + s]; }
    }
    if (used_streams & (1u << 31)) { static const float zero[4] = {0, 0, 0, 1}; [enc setVertexBytes:zero length:16 atIndex:30]; }

    // Instanced draws with item spots among their instances (bushsanity: bushes are drawn in batches, each instance's
    // model matrix in vertex storage buffer 0, indexed by instance id): drawn as runs of instances, gold runs tinted.
    // (Metal's instance_id includes the base instance, so a run [b + k, b + k + n) draws exactly those instances.)
    // Only for a vertex shader that reads storage buffer 0: the binding stays from the last draw that set it, so any
    // other instanced draw (particle effects: leaves, dust, sparkles) took a bush batch's matrices for its own
    // instances and was tinted wherever that batch had an unchecked bush at the same index - and a batch of a single
    // bush was never looked at.
    uint32_t inst_all = indexed ? (uint32_t)r->x[6] : (uint32_t)r->x[4], base_all = indexed ? (uint32_t)r->x[5] : (uint32_t)r->x[3];
    if (!inst_all) inst_all = 1;
    enum { MAX_RUNS = 64 };
    uint32_t run_at[MAX_RUNS], run_n[MAX_RUNS];
    bool run_gold[MAX_RUNS];
    int nruns = 0;
    if (pr->fi->uses_tint && (pr->vi->ssbo_read & 1) && st.sbuf[0][0].addr) {
        extern bool pot_glow_active(void);
        extern bool pot_instance_gold(const float *m);
        uint64_t recs = st.sbuf[0][0].size / 64;
        if (pot_glow_active() && (uint64_t)base_all + inst_all <= recs) {
            const float *m = (const float *)(uintptr_t)st.sbuf[0][0].addr;
            for (uint32_t k = 0; k < inst_all; k++) {
                bool g = pot_instance_gold(m + (size_t)(base_all + k) * 16);
                if (nruns && run_gold[nruns - 1] == g) { run_n[nruns - 1]++; continue; }
                if (nruns == MAX_RUNS) { nruns = 0; break; }  // (too fragmented: draw as is)
                run_at[nruns] = k; run_n[nruns] = 1; run_gold[nruns] = g; nruns++;
            }
            if (nruns == 1 && !run_gold[0]) nruns = 0;
        }
    }
    if (!nruns) { run_at[0] = 0; run_n[0] = inst_all; nruns = 1; run_gold[0] = false; }
    for (int ri = 0; ri < nruns; ri++) {
    if (nruns > 1 || run_gold[0]) {
        float tint[RECOLOUR_TINT_FLOATS] = {0};
        extern void pot_gold_tint(float tint[8]);
        if (run_gold[ri]) pot_gold_tint(tint);
        else recolour_tint_draw(draw_recolour, tint);
        [enc setFragmentBytes:tint length:sizeof tint atIndex:19];
    }
    // draw
    bool native;
    uint32_t mode = (uint32_t)r->x[0];
    MTLPrimitiveType pt = prim_type(mode, &native);
    if (!indexed) {
        uint32_t first = (uint32_t)r->x[1], count = (uint32_t)r->x[2], base_inst = base_all + run_at[ri], inst = run_n[ri];
        if (native) {
            [enc drawPrimitives:pt vertexStart:first vertexCount:count instanceCount:inst baseInstance:base_inst];
        } else if (mode == 6 || mode == 7 || mode == 9) {
            uint32_t n;
            id<MTLBuffer> ib = expand_indices(mode, count, NULL, 0, first, &n);
            [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:ib
                     indexBufferOffset:0 instanceCount:inst baseVertex:0 baseInstance:base_inst];
        } else { stat_skipped++; return; }
    } else {
        uint32_t itype = (uint32_t)r->x[1], count = (uint32_t)r->x[2];
        uint64_t iaddr = r->x[3];
        int32_t base_vertex = (int32_t)r->x[4];
        uint32_t base_inst = base_all + run_at[ri], inst = run_n[ri];
        if (native && itype != 0) {
            MTLIndexType it = itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
            uint32_t isz = itype == 1 ? 2 : 4;
            id<MTLBuffer> ib;
            uint64_t ioff;
            bool in_heap = iaddr >= HEAP_BASE && iaddr + (uint64_t)count * isz <= HEAP_BASE + HEAP_MAX && (iaddr % isz) == 0;
            if (in_heap) { bind_note(1, iaddr, (uint64_t)count * isz); snap_note_vertex_bind(iaddr, (uint64_t)count * isz); }
            if (in_heap && !dyn_copy(iaddr, (uint64_t)count * isz)) {
                ib = g_guest_buffer; ioff = iaddr - HEAP_BASE;
            } else {
                ioff = ring_alloc((void *)(uintptr_t)iaddr, (uint64_t)count * isz); ib = ring;
            }
            [enc drawIndexedPrimitives:pt indexCount:count indexType:it indexBuffer:ib indexBufferOffset:ioff
                         instanceCount:inst baseVertex:base_vertex baseInstance:base_inst];
        } else {
            uint32_t n;
            id<MTLBuffer> ib;
            if (native) {  // 8-bit indices: widen
                uint32_t *w = malloc(count * 4 + 4);
                for (uint32_t i = 0; i < count; i++) w[i] = ((const uint8_t *)(uintptr_t)iaddr)[i];
                ib = [dev newBufferWithBytes:w length:count * 4 + 4 options:MTLResourceStorageModeShared];
                free(w);
                n = count;
            } else {
                ib = expand_indices(mode, count, (const void *)(uintptr_t)iaddr, (int)itype, 0, &n);
                pt = MTLPrimitiveTypeTriangle;
            }
            [enc drawIndexedPrimitives:pt indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:0
                         instanceCount:inst baseVertex:base_vertex baseInstance:base_inst];
        }
    }
    }  // runs
    stat_draws++;
    if (dbg_this) {
        static int nlog;
        if (nlog++ < 4) {
            renderer_flush();
            const float *f = dbgvs.contents;
            LOG("[dbgvs] draw %d:", nlog);
            for (int i = 0; i < 2; i++)
                for (int v = 0; v < 4; v++) LOG(" i%d v%d (%g %g %g %g) uv(%g %g %g)", i, v, f[(i * 8 + v) * 4], f[(i * 8 + v) * 4 + 1], f[(i * 8 + v) * 4 + 2], f[(i * 8 + v) * 4 + 3],
                                                f[(512 + i * 8 + v) * 4], f[(512 + i * 8 + v) * 4 + 1], f[(512 + i * 8 + v) * 4 + 2]);
            LOG("\n");
        }
    }
}

// ------------------------------------------------------------------ texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static const char *copy_src =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V vs(uint vid [[vertex_id]], constant float4 &r [[buffer(0)]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2); V o; o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);\n"
    "  o.uv = r.xy + p * r.zw; return o; }\n"
    "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return t.sample(s, in.uv); }\n"
    "struct D { float d [[depth(any)]]; };\n"
    "fragment D fs_depth(V in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  D o; o.d = t.sample(s, in.uv); return o; }\n";

// Bytes per block of the Metal texture backing an NVN format (RGB8 is expanded to RGBA8 on the host).
static uint64_t renderer_pixel_format_bytes(MTLPixelFormat pf, uint64_t nvn_bpb) {
    (void)pf;
    return nvn_bpb == 3 ? 4 : nvn_bpb;
}

// Linear guest memory <-> texture region. Row/image strides of 0 mean tightly packed.
static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    td_flush(t);  // (a recorded clear happens before the upload)
    end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    static int nlog;
    if (nlog++ < 20) LOG("[copy] buf %llx -> tex %ux%u f%#x region %d,%d,%d %dx%dx%d row %llu lvl %d\n", src, t->width, t->height, t->format, rg[0], rg[1], rg[2], rg[3], rg[4], rg[5], row, level);
    id<MTLBlitCommandEncoder> b = [cb() blitCommandEncoder];
    id<MTLTexture> tex = renderer_texture(t, b, false);  // make sure prior CPU contents are uploaded first
    renderer_mark_gpu_owned(t);
    bool array = tex.textureType == MTLTextureType2DArray || tex.textureType == MTLTextureTypeCube;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint64_t mtl_bpb = renderer_pixel_format_bytes(tex.pixelFormat, fi->bpb);
    for (int z = 0; z < (array ? rg[5] : 1); z++) {
        const uint8_t *s = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img);
        uint64_t out_row = (uint64_t)wb * mtl_bpb;
        uint8_t *data = malloc(out_row * hb * (array ? 1 : rg[5]));
        for (int d = 0; d < (array ? 1 : rg[5]); d++)
            for (uint32_t y = 0; y < hb; y++) {
                const uint8_t *sr = s + (uint64_t)d * img + (uint64_t)y * row;
                uint8_t *dr = data + ((uint64_t)d * hb + y) * out_row;
                if (mtl_bpb == fi->bpb) memcpy(dr, sr, out_row);
                else if (fi->bpb == 3 && mtl_bpb == 4)
                    for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
                else memset(dr, 0, out_row);
            }
        id<MTLBuffer> staging = [dev newBufferWithBytes:data length:out_row * hb * (array ? 1 : rg[5])
                                                options:MTLResourceStorageModeShared];
        free(data);
        [b copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:out_row sourceBytesPerImage:out_row * hb
               sourceSize:MTLSizeMake(rg[3], rg[4], array ? 1 : rg[5]) toTexture:tex
         destinationSlice:array ? rg[2] + z : 0 destinationLevel:level
        destinationOrigin:MTLOriginMake(rg[0], rg[1], array ? 0 : rg[2])];
    }
    [b endEncoding];
}

// Color copy with scaling: draw src rect into dst rect (physical texels).
static id<MTLTexture> copy_raw_view(id<MTLTexture> t) {
    MTLPixelFormat pf = t.pixelFormat;
    MTLPixelFormat raw = pf == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm
                       : pf == MTLPixelFormatBGRA8Unorm_sRGB ? MTLPixelFormatBGRA8Unorm : pf;
    if (raw == pf) return t;
    id<MTLTexture> v = [t newTextureViewWithPixelFormat:raw];
    return v ? v : t;
}

static void draw_copy(id<MTLTexture> s, int sx0, int sy0, int sw, int sh, id<MTLTexture> d, int dx0, int dy0, int dw, int dh,
                      bool linear) {
    static int raw = -1;  // LARECOMP_COPY_RAW_SRGB: copies move values unchanged (no sRGB decode/encode on either side)
    if (raw < 0) raw = getenv("LARECOMP_COPY_RAW_SRGB") != NULL;
    if (raw) { s = copy_raw_view(s); d = copy_raw_view(d); }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = d;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb() renderCommandEncoderWithDescriptor:rp];
    NSString *key = [NSString stringWithFormat:@"copy %lu", (unsigned long)d.pixelFormat];
    id<MTLRenderPipelineState> pipe = cache[key];
    if (!pipe) {
        static id<MTLLibrary> lib;
        if (!lib) lib = TIMED_COMPILE([dev newLibraryWithSource:@(copy_src) options:nil error:nil]);
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = d.pixelFormat;
        pipe = TIMED_COMPILE([dev newRenderPipelineStateWithDescriptor:pd error:nil]);
        cache[key] = pipe;
    }
    static id<MTLSamplerState> lin, near;
    if (!lin) {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        near = [dev newSamplerStateWithDescriptor:sd];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        lin = [dev newSamplerStateWithDescriptor:sd];
    }
    float r[4] = {(float)sx0 / s.width, (float)sy0 / s.height, (float)sw / s.width, (float)sh / s.height};
    [e setRenderPipelineState:pipe];
    [e setViewport:(MTLViewport){dx0, dy0, dw, dh, 0, 1}];
    [e setVertexBytes:r length:16 atIndex:0];
    [e setFragmentTexture:s atIndex:0];
    [e setFragmentSamplerState:linear ? lin : near atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    stat_passes++;
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    td_flush(t);
    end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    {
        static int nlog;
        if (nlog++ < 12 || getenv("LARECOMP_LOG_READBACK"))
            LOG("[readback] tex %ux%u f%#x level %d region %d,%d %dx%d -> %llx row %llu\n", t->width, t->height,
                t->format, level, rg[0], rg[1], rg[3], rg[4], dst, row);
    }
    id<MTLTexture> tex = renderer_texture(t, nil, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:tex.pixelFormat width:t->width
                                                                                      height:t->height mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> small = [dev newTextureWithDescriptor:td];
        draw_copy(tex, 0, 0, (int)lroundf(t->width * tsx), (int)lroundf(t->height * tsy), small, 0, 0, t->width, t->height, true);
        tex = small;
    }
    id<MTLBlitCommandEncoder> b = [cb() blitCommandEncoder];
    if (renderer_pixel_format_bytes(tex.pixelFormat, fi->bpb) != fi->bpb) { [b endEncoding]; return; }
    bool array = tex.textureType == MTLTextureType2DArray || tex.textureType == MTLTextureTypeCube;
    for (int z = 0; z < (array ? rg[5] : 1); z++)
        [b copyFromTexture:tex sourceSlice:array ? rg[2] + z : 0 sourceLevel:level
              sourceOrigin:MTLOriginMake(rg[0], rg[1], array ? 0 : rg[2])
                sourceSize:MTLSizeMake(rg[3], rg[4], array ? 1 : rg[5]) toBuffer:g_guest_buffer
         destinationOffset:dst - HEAP_BASE + (uint64_t)z * img destinationBytesPerRow:row destinationBytesPerImage:img];
    [b endEncoding];
    renderer_flush();  // the CPU may read the result right after the fence
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    td_flush(st_); td_flush(dt);  // (recorded clears happen before the copy)
    end_pass();
    id<MTLTexture> s = sampled_texture(st_, &src->view, src->has_view, TEXTYPE_2D);
    renderer_mark_gpu_owned(dt);
    id<MTLTexture> d = view_texture(dt, &dst->view, dst->has_view, true, nil);
    // a full copy into a swapchain image: the source is the frame's final framebuffer (UI is drawn there)
    extern void renderer_texture_mark_presented(NvnTexture *t);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height)
        renderer_texture_mark_presented(st_);
    extern bool renderer_texture_first_write(NvnTexture *t);
    renderer_texture_first_write(dt);  // a copy fills the image: no clear afterwards this frame
    extern bool renderer_texture_level_fresh(NvnTexture *t);
    renderer_texture_level_fresh(dt);  // nor a level-change clear
    const int32_t *sr = src->region, *dr = dst->region;
    if (g_debug_frame) {
        static int nl;
        uint64_t total = tegra_total_size(&st_->layout), nz = 0;
        for (uint64_t k = 0; k < total && st_->addr; k++) nz += ((const uint8_t *)(uintptr_t)st_->addr)[k] != 0;
        if (nl < 400) LOG("[copytex] src addr %llx size %llu nonzero %llu tflags %#x stride %u bh %u\n", st_->addr, total, nz, st_->flags, st_->stride, st_->layout.block_height_log2);
        if (nl++ < 400) LOG("[copytex] %ux%u f%#x (%d,%d %dx%d) -> %ux%u f%#x (%d,%d %dx%d) flags %llx src_gpu %d\n", st_->width, st_->height, st_->format,
            sr[0], sr[1], sr[3], sr[4], dt->width, dt->height, dt->format, dr[0], dr[1], dr[3], dr[4], flags, renderer_is_gpu_owned(st_));
    }
    // regions are in guest texels; scaled allocations (output resolution) need physical texels
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)lroundf(st_->width * ssx), sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)lroundf(st_->height * ssy);
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)lroundf(dt->width * dsx), dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)lroundf(dt->height * dsy);
    static int no_depth_scale = -1;
    if (no_depth_scale < 0) no_depth_scale = getenv("LARECOMP_NO_DEPTH_SCALE") != NULL;
    if (!no_depth_scale && renderer_format_is_depth(dt->format) && (sw != dw || sh != dh) && s.sampleCount == 1 && dt->format != 0x31) {
        // scaled depth copy (the game builds its depth pyramid this way): draw, writing fragment depth
        MTLRenderPassDescriptor *drp = [MTLRenderPassDescriptor renderPassDescriptor];
        drp.depthAttachment.texture = d;
        drp.depthAttachment.loadAction = MTLLoadActionLoad;
        drp.depthAttachment.storeAction = MTLStoreActionStore;
        if (renderer_format_has_stencil(dt->format)) {
            drp.stencilAttachment.texture = d;
            drp.stencilAttachment.loadAction = MTLLoadActionLoad;
            drp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        id<MTLRenderCommandEncoder> e = [cb() renderCommandEncoderWithDescriptor:drp];
        NSString *key = [NSString stringWithFormat:@"copydepth %lu", (unsigned long)d.pixelFormat];
        id<MTLRenderPipelineState> pipe = cache[key];
        if (!pipe) {
            static id<MTLLibrary> dlib;
            NSError *derr = nil;
            if (!dlib) dlib = TIMED_COMPILE([dev newLibraryWithSource:@(copy_src) options:nil error:&derr]);
            if (!dlib) LOG("[copy] depth copy library: %s\n", derr.localizedDescription.UTF8String);
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [dlib newFunctionWithName:@"vs"];
            pd.fragmentFunction = [dlib newFunctionWithName:@"fs_depth"];
            pd.depthAttachmentPixelFormat = d.pixelFormat;
            if (renderer_format_has_stencil(dt->format)) pd.stencilAttachmentPixelFormat = d.pixelFormat;
            NSError *perr = nil;
            pipe = TIMED_COMPILE([dev newRenderPipelineStateWithDescriptor:pd error:&perr]);
            if (!pipe) { LOG("[copy] depth copy pipeline: %s\n", perr.localizedDescription.UTF8String); return; }
            cache[key] = pipe;
        }
        static id<MTLDepthStencilState> always;
        if (!always) {
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            dd.depthCompareFunction = MTLCompareFunctionAlways;
            dd.depthWriteEnabled = YES;
            always = [dev newDepthStencilStateWithDescriptor:dd];
        }
        // An upsampling copy (a source of lower resolution, as the guest-sized AO depth gets from the scaled pyramid
        // below 0.5x: runtime/nvn/metal.m tex_guest_sized_ao) interpolates: nearest made stairs of the ground's
        // slope that the ambient occlusion shaded as dark horizontal lines (issue #356, 25% in a window).
        // Downsampling stays nearest (the game's own choice of samples). LARECOMP_DEPTH_UP_NEAREST=1 (A/B): nearest.
        static id<MTLSamplerState> dnear, dlin;
        static int up_nearest = -1;
        if (up_nearest < 0) up_nearest = getenv("LARECOMP_DEPTH_UP_NEAREST") != NULL;
        if (!dnear) {
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            dnear = [dev newSamplerStateWithDescriptor:sd];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
            dlin = [dev newSamplerStateWithDescriptor:sd];
        }
        bool up = title_is_las() && !up_nearest && (dw > sw || dh > sh);
        id<MTLTexture> sdepth = s;
        if (s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 || s.pixelFormat == MTLPixelFormatDepth24Unorm_Stencil8)
            sdepth = [s newTextureViewWithPixelFormat:s.pixelFormat];
        float r[4] = {(float)sx0 / s.width, (float)sy0 / s.height, (float)sw / s.width, (float)sh / s.height};
        [e setRenderPipelineState:pipe];
        [e setDepthStencilState:always];
        [e setViewport:(MTLViewport){dx0, dy0, dw, dh, 0, 1}];
        [e setVertexBytes:r length:16 atIndex:0];
        [e setFragmentTexture:sdepth atIndex:0];
        [e setFragmentSamplerState:up ? dlin : dnear atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
        stat_passes++;
        static int dbgc = -1;
        if (dbgc < 0) dbgc = getenv("LARECOMP_DEBUG_DEPTHCOPY") != NULL;
        if (dbgc) {
            static int nlog;
            if (nlog++ < 6) {
                id<MTLBuffer> rb = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
                id<MTLBlitCommandEncoder> bb = [cb() blitCommandEncoder];
                MTLBlitOption opt = (s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
                [bb copyFromTexture:s sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(s.width / 2, s.height / 2, 0)
                         sourceSize:MTLSizeMake(1, 1, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4 options:opt];
                opt = (d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
                [bb copyFromTexture:d sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(d.width / 2, d.height / 2, 0)
                         sourceSize:MTLSizeMake(1, 1, 1) toBuffer:rb destinationOffset:16 destinationBytesPerRow:4 destinationBytesPerImage:4 options:opt];
                [bb endEncoding];
                renderer_flush();
                const float *f = rb.contents;
                LOG("[copy] depth %lux%lu fmt %lu -> %lux%lu fmt %lu: centre src %g dst %g\n", (unsigned long)s.width, (unsigned long)s.height,
                    (unsigned long)s.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, f[0], f[4]);
            }
        }
        return;
    }
    if (renderer_format_is_depth(dt->format) || (s.pixelFormat == d.pixelFormat && sw == dw && sh == dh && s.sampleCount == d.sampleCount)) {
        id<MTLBlitCommandEncoder> b = [cb() blitCommandEncoder];
        [b copyFromTexture:s sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(sx0, sy0, 0)
                sourceSize:MTLSizeMake(MIN(sw, dw), MIN(sh, dh), 1) toTexture:d destinationSlice:0 destinationLevel:0
         destinationOrigin:MTLOriginMake(dx0, dy0, 0)];
        [b endEncoding];
        return;
    }
    draw_copy(s, sx0, sy0, sw, sh, d, dx0, dy0, dw, dh, flags & 1);
}

// ------------------------------------------------------------------ executor
static void exec_list(uint64_t handle, int depth) {
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        uint32_t stride = r->size;
        void *x = r->extra;
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        if (!st.render_enabled && nvn_command_needs_render(r->op)) { p += stride; continue; }
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) exec_list(h[i], depth + 1);
            break;
        }
        case CMD_BIND_PROGRAM: st.program = (NvnProgram *)(uintptr_t)r->x[0]; break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            {
                const uint64_t *tx = x;
                for (int i = 0; i < (int)r->x[0] && i < 8; i++) {
                    NvnTexture *t = (NvnTexture *)(uintptr_t)tx[i];
                    static int nlog;
                    if (t && (t->flags & 2) && nlog++ < 5) LOG("[metal] video texture %p used as render target\n", t);
                }
            }
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState old = st.rt;
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            bool same = rt_same(&old, &st.rt);
            static int dedup = -1;
            if (dedup < 0) { const char *e = ENV("LARECOMP_MTL_RT_DEDUP"); dedup = !e || atoi(e); }
            if (dedup && same && enc && !st.rt_dirty) break;  // the targets already bound: the pass goes on
            if (!td_defer_on() && !(dedup && same)) {  // (off mode: the old targets' recorded clears become passes now)
                RtState new = st.rt;
                st.rt = old;
                td_flush_targets(&old);
                st.rt = new;
            }
            st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            static int log_clears = -1;  // LARECOMP_LOG_CLEARS: every colour clear (frame, target size, mask, scissor)
            if (log_clears < 0) log_clears = getenv("LARECOMP_LOG_CLEARS") != NULL;
            if (log_clears && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x scissor %d,%d %dx%d\n", (unsigned long long)renderer_present_count(),
                    index, st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2],
                    st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
            else if (log_clears) LOG("[cclear] frame %llu idx %d: no target bound (ncolor %d)\n", (unsigned long long)renderer_present_count(), index, st.rt.ncolor);
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask) (runtime/nvn/cmdbuf.c clear_texture). Was never
            // executed: the game clears its text-box textures this way before drawing a new message, so the old
            // text stayed behind the new one (user 2026-10-08: "Load this file?" over "Please select a file.").
            // Done as a render-target clear: bind the texture alone, scissor = region, clear(), restore.
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            {   // the whole texture, every channel, not a target of the open pass: recorded for its next pass
                static int on = -1;
                if (on < 0) { const char *e = ENV("LARECOMP_MTL_LOAD_CLEAR"); on = !e || atoi(e); }
                const int32_t *rg = ct->region;
                bool whole = rg[3] <= 0 || rg[4] <= 0 || (rg[0] == 0 && rg[1] == 0 && rg[3] >= (int32_t)t->width && rg[4] >= (int32_t)t->height);
                bool open_here = enc && !st.rt_dirty && (st.rt.depth == t || (st.rt.ncolor > 0 && st.rt.color[0] == t) || (st.rt.ncolor > 1 && st.rt.color[1] == t) ||
                                                        (st.rt.ncolor > 2 && st.rt.color[2] == t) || (st.rt.ncolor > 3 && st.rt.color[3] == t));
                uint32_t mask = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF;
                if (on && whole && !ct->has_view && !open_here && mask == 0xF && !renderer_format_is_depth(t->format)) {
                    td_record(t, true, (int)r->x[6], ct->color, false, 0, false, 0);
                    if (!td_defer_on()) td_flush(t);
                    break;
                }
            }
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            static int nlog;
            if (nlog++ < 8 || g_debug_frame)
                LOG("[cleartex] %ux%u f%#x region %d,%d %dx%d color %g %g %g %g mask %#llx kind %llu\n", t->width, t->height, t->format,
                    rg[0], rg[1], rg[3], rg[4], ct->color[0], ct->color[1], ct->color[2], ct->color[3], r->x[4], r->x[6]);
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (st.rt.depth) {
                // (depth s0, depthMask x1, stencil x2, stencilMask x3)
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1],
                      (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH:
            dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]);
            if (st.program) {
                static uint64_t seen[16];
                static int nseen;
                uint64_t cs = st.program->stages[5].code_addr;
                bool known = false;
                for (int i = 0; i < nseen; i++) known |= seen[i] == cs;
                if (!known && nseen < 16) {
                    seen[nseen++] = cs;
                    char path[64];
                    snprintf(path, sizeof path, "build/cs_%d.bin", nseen - 1);
                    FILE *f = fopen(path, "wb");  // (no build/ outside a source checkout: the app's data root)
                    if (f) fwrite((void *)(uintptr_t)cs, 1, 0x4000, f), fclose(f);
                    LOG("[dispatch] new compute program %d: cs %llx groups %llu %llu %llu images:", nseen - 1, cs, r->x[0], r->x[1], r->x[2]);
                    for (int i = 0; i < 8; i++) if (st.img[5][i]) LOG(" %d=%llx", i, st.img[5][i]);
                    LOG(" tex:");
                    for (int i = 0; i < 8; i++) if (st.tex[5][i]) LOG(" %d=%llx", i, st.tex[5][i]);
                    LOG(" ubuf:");
                    for (int i = 0; i < 8; i++) if (st.ubuf[5][i].addr) LOG(" %d=%llx/%llu", i, st.ubuf[5][i].addr, st.ubuf[5][i].size);
                    LOG(" ssbo:");
                    for (int i = 0; i < 8; i++) if (st.sbuf[5][i].addr) LOG(" %d=%llx/%llu", i, st.sbuf[5][i].addr, st.sbuf[5][i].size);
                    LOG("\n");
                }
            }
            if (g_debug_frame && st.program) {
                LOG("[dispatch] %llu %llu %llu prog %p cs %llx\n", r->x[0], r->x[1], r->x[2], st.program, st.program->stages[5].code_addr);
                FILE *f = fopen("build/cs_code.bin", "wb");
                if (f) fwrite((void *)(uintptr_t)st.program->stages[5].code_addr, 1, 0x4000, f), fclose(f);
                f = fopen("build/cs_ctrl.bin", "wb");
                if (f) fwrite(st.program->stages[5].control, 1, 0x900, f), fclose(f);
                LOG("[dispatch] images:");
                for (int i = 0; i < 8; i++) if (st.img[5][i]) LOG(" %d=%llx", i, st.img[5][i]);
                LOG(" tex:");
                for (int i = 0; i < 8; i++) if (st.tex[5][i]) LOG(" %d=%llx", i, st.tex[5][i]);
                LOG(" ubuf:");
                for (int i = 0; i < 8; i++) if (st.ubuf[5][i].addr) LOG(" %d=%llx/%llu", i, st.ubuf[5][i].addr, st.ubuf[5][i].size);
                LOG("\n");
            }
            break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            // (buffer, size, offset, dataSize, data): apply the update in command order
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            break;
        case CMD_FENCE_SYNC: {
            // The CPU may read GPU-written memory (compute results) once the fence is signaled.
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("LARECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[0]) nvn_sync_set_seq(r->x[0], renderer_fence_seq());
            break;
        }
        case CMD_REPORT_COUNTER: {
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            static uint32_t seen;
            uint32_t type = (uint32_t)r->x[0];
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            // (Switch GPU ticks, 614.4 MHz: the game converts the difference to ns itself - see the Vulkan exec.c)
            if (!out) break;
            static int gpu = -1;
            if (gpu < 0) gpu = !(getenv("LARECOMP_MTL_COUNTER") && !strcmp(getenv("LARECOMP_MTL_COUNTER"), "cpu"));
            static uint64_t *prev_out;  // (frame pacing: the pair's start)
            out[0] = 0;
            uint64_t now_ticks = ns_to_gpu_ticks(plat_now_ns());
            // (overlap) the game reads this pair before its GPU work is done: the end counter reports the latest
            // completed pair's GPU span after this pair's start (the frame before, as a frame-late measurement)
            if (gpu && overlap_on() && prev_out && out - prev_out == 2) now_ticks = prev_out[1] + g_last_gpu_frame_ticks;
            out[1] = pace_end_counter(prev_out, out, now_ticks);
            if (gpu && type == 0) {  // (type 0: timestamp) the GPU's time at this point, once it's done
                renderer_submit();
                pthread_mutex_lock(&ctr_lock);
                if (nctr < CTR_MAX) {
                    ctr_pend[nctr].out = out, ctr_pend[nctr].start = prev_out;
                    ctr_pend[nctr].before = submit_seq, ctr_pend[nctr].after = submit_seq + 1;
                    nctr++;
                } else {
                    static int warned;
                    if (!warned++) LOG("[metal] timing counters: %d pending, this one keeps the host clock\n", CTR_MAX);
                }
                pthread_mutex_unlock(&ctr_lock);
            }
            prev_out = out;
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: {
            const TexRefRec *t = x;
            copy_texture(&t[0], &t[1], r->x[6]);
            break;
        }
        case CMD_DOWNSAMPLE: {
            const TexRefRec *t = x;
            copy_texture(&t[0], &t[1], 1);
            break;
        }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_SET_RENDER_ENABLE:
            if (r->x[6]) {
                if (r->x[0] > 1 || !r->x[1]) FATAL("nvn: invalid conditional rendering arguments\n");
                renderer_flush();
                st.render_enabled = nvn_render_condition((uint32_t)r->x[0], (void *)(uintptr_t)r->x[1]);
            } else st.render_enabled = r->x[0] != 0;
            break;
        case CMD_REPORT_VALUE:
            if (!r->x[1]) FATAL("nvn: null report-value buffer\n");
            nvn_report_value((void *)(uintptr_t)r->x[1], (uint32_t)r->x[0],
                (uint64_t)((unsigned __int128)plat_now_ns() * 384 / 625));
            break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            { static int nlog; if (nlog++ < 40 && ENV("LARECOMP_LOG_BINDS")) LOG("[copybuf] frame %llu %llx -> %llx %llu bytes\n", (unsigned long long)renderer_exec_frame(), r->x[0], r->x[1], r->x[2]); }
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        default: break;
        }
        p += stride;
    }
}

// In-place reboot (runtime/nvn/metal.m backend_reset; GPU idle, the game's textures already destroyed): drop
// everything keyed by guest addresses or guest-owned objects - programs (by code address), pipelines (by program),
// views, snapshots, bound state - so the next boot can't hit a stale entry. Metal's own shader cache stays warm.
// ------------------------------------------------------------------ shader preparation (shader_prep.h)
// Each record: translated here (the translator isn't re-entrant), compiled and built on a few worker threads (at most
// one build per core so the start screen stays smooth), kept in the g_prep_* tables for the game.
static dispatch_semaphore_t g_prep_slots;

bool renderer_prepare_begin(void) {
    if (!title_is_las()) return false;
    if (!dev) {
        extern void *ui_metal_device(void);
        dev = (__bridge id<MTLDevice>)ui_metal_device();
        if (!dev) return false;
    }
    if (!cache) cache = [NSMutableDictionary new];
    pthread_mutex_lock(&g_prep_lock);
    if (!g_prep_progs) {
        g_prep_progs = [NSMutableDictionary new];
        g_prep_pipes = [NSMutableDictionary new];
        g_prep_cs = [NSMutableDictionary new];
    }
    pthread_mutex_unlock(&g_prep_lock);
    if (!g_prep_queue) {
        g_prep_queue = dispatch_queue_create("larecomp.prepare", DISPATCH_QUEUE_CONCURRENT);
        long n = (long)NSProcessInfo.processInfo.activeProcessorCount - 2;
        g_prep_slots = dispatch_semaphore_create(n < 2 ? 2 : n > 12 ? 12 : n);
    }
    return true;
}

int renderer_prepare_pending(void) { return g_prep_pending; }
void renderer_prepare_end(void) {  // (the binary archive: saved now, off this thread - a PLAY waiting on the
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ archive_save(false); });  // preparation doesn't wait for it)
    LOG("[metal] shader archive: %d pipelines loaded from it, %d added; ready after %.0f ms; library compiles %.0f ms, "
        "pipeline builds %.0f ms (summed over threads)\n", (int)g_archive_hits, (int)g_archive_adds, g_archive_open_us / 1e3,
        g_lib_us / 1e3, g_pipe_us / 1e3);
}

static void prep_run(void (^job)(void)) {
    dispatch_semaphore_wait(g_prep_slots, DISPATCH_TIME_FOREVER);
    g_prep_pending++;
    dispatch_async(g_prep_queue, ^{
        @autoreleasepool { job(); }
        g_prep_pending--;
        dispatch_semaphore_signal(g_prep_slots);
    });
}

bool renderer_prepare(const ShaderRec *r, const ShaderBlob *vsb, const ShaderBlob *fsb) {
    if (r->kind == 'C') {
        uint32_t shared_memory = 0;
        if (vsb->control) memcpy(&shared_memory, vsb->control + 0x754, sizeof shared_memory);
        uint64_t ck = cs_content_key(r->vs, r->local, shared_memory);
        pthread_mutex_lock(&g_prep_lock);
        bool have = g_prep_cs[@(ck)] != nil;
        pthread_mutex_unlock(&g_prep_lock);
        if (have) return true;
        CsProg *cp = calloc(1, sizeof *cp);
        cp->shared_memory = shared_memory;
        memcpy(cp->local, r->local, sizeof cp->local);
        if (!translate_compute(cp, vsb->code, vsb->const_offset ? vsb->const_offset : 0x10000)) return false;
        pthread_mutex_lock(&g_prep_lock);
        g_prep_cs[@(ck)] = [NSValue valueWithPointer:cp];
        pthread_mutex_unlock(&g_prep_lock);
        prep_run(^{ compile_compute(cp); });
        return true;
    }
    // the keys the draw path makes for this state (draw)
    ShaderKey vkey = {0}, fkey = {0};
    vkey.flip_y = fkey.flip_y = (r->flags & SR_FLIP_Y) != 0;
    vkey.depth_minus_one = (r->flags & SR_DEPTH_MINUS_ONE) != 0;
    vkey.flip_x = fkey.flip_x = (r->flags & SR_FLIP_X) != 0;
    for (int i = 0; i < r->nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(r->attrib[i] & 0xFF);
    for (int i = 0; i < r->ncolor && i < 8; i++) if (r->color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(r->color[i]);
    uint64_t ck = prog_content_key(r->vs, r->fs, &vkey, &fkey);
    Prog *pr = prep_find_prog(ck);
    if (!pr) {
        pr = [Prog new];
        translate_program(pr, vsb->code, vsb->const_offset ? vsb->const_offset : 0x10000, fsb->code,
                          fsb->const_offset ? fsb->const_offset : 0x10000, &vkey, &fkey, 0, 0);
        pthread_mutex_lock(&g_prep_lock);
        g_prep_progs[@(ck)] = pr;  // (failed ones too: the game then skips them as it would its own)
        pthread_mutex_unlock(&g_prep_lock);
    }
    if (pr->failed) return false;
    ShaderRec rec = *r;
    prep_run(^{
        @synchronized(pr) { if (!pr->ready && !pr->failed) compile_program(pr); }
        if (!pr->ready) return;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        attachment_formats_rec(pd, &rec);
        pipeline_desc(pd, pr, &rec);
        NSString *body = pipeline_key(pd, pr);
        if (prep_find_pipe(body)) return;
        NSError *err = nil;
        id<MTLRenderPipelineState> ps = archive_render_pipeline(pd, &err);
        if (!ps) { static int n; if (n++ < 10) LOG("[shader] prepared pipeline failed: %s\n", err.localizedDescription.UTF8String); return; }
        pthread_mutex_lock(&g_prep_lock);
        g_prep_pipes[body] = ps;
        pthread_mutex_unlock(&g_prep_lock);
    });
    return true;
}

void renderer_reset(void) {
    memset(td_tab, 0, sizeof td_tab);  // (recorded clears and depth guesses of the old boot's textures)
    td_pending = 0;
    renderer_flush();
    pthread_mutex_lock(&gone_lock);
    ngone = 0;
    pthread_mutex_unlock(&gone_lock);
    for (int i = 0; i < VIEW_TAB; i++) {
        if (view_tab[i].view) CFRelease(view_tab[i].view);
        view_tab[i].view = view_tab[i].base = NULL;
        view_tab[i].key = 0;
    }
    g_views_live = 0;
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    [cache removeAllObjects];
    [g_cs_progs removeAllObjects];
    @synchronized(g_ready_pipes) { [g_ready_pipes removeAllObjects]; }
    g_boot_gen++;
    [g_snap_copies removeAllObjects];
    // storage snapshots: their copies live in guest memory, which the reboot wipes, so a kept entry would hand back
    // zeroed data (blank names and death counts on the file select)
    for (int i = 0; i < nwatch_pages; i++)
        if (watch_pages[i].armed) plat_protect(watch_pages[i].page, PLAT_PAGE, PLAT_PROT_RW), watch_pages[i].armed = 0;
    nwatch_pages = 0;
    nrs = 0;
    snap_ring_off = 0;
    snap_gen++;
    memset((void *)rs_cpu_wrote, 0, sizeof rs_cpu_wrote);
    memset(rs_hit_inside, 0, sizeof rs_hit_inside);
    memset(rs_store_frame, 0, sizeof rs_store_frame);
    memset(snap_tex, 0, sizeof snap_tex);
    memset(snap_ok, 0, sizeof snap_ok);
    for (int i = 0; i < 4; i++) { feedback_tex[i] = NULL; feedback_copy[i] = nil; }
    nfeedback = 0;
    g_cur_fs_hash = 0;
    last_bias_enc = nil;
    pthread_mutex_lock(&ctr_lock);
    nctr = 0;  // (pending timing counters point into the old boot's memory)
    pthread_mutex_unlock(&ctr_lock);
    pthread_mutex_lock(&cb_lock);
    last_cb = nil;
    for (int i = 0; i < CB_HIST; i++) cb_hist[i] = nil;  // (they retain the old guest heap buffer)
    pthread_mutex_unlock(&cb_lock);
    st_defaults();
}

void backend_execute(uint64_t handle) {
    @autoreleasepool {
        exec_list(handle, 0);
        static int sync_submit = -1;
        if (sync_submit < 0) sync_submit = getenv("LARECOMP_SYNC_SUBMIT") ? 1 : 0;
        if (sync_submit) renderer_flush();  // old behaviour: finish the GPU work before returning
        else renderer_submit();  // the guest fences (nvnSyncWait) before reusing memory the GPU reads
        if (counters_want_next()) {  // (a timing counter ended the list: its buffer after it, empty)
            (void)cb();
            renderer_submit();
        }
    }
}
