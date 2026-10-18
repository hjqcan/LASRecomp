// NVN implemented on the host. Guest NVN objects hold a pointer to a host object at offset 0;
// builders and small state objects store their fields directly in guest memory.
#pragma once
#include <string.h>
#include "../hle.h"
#include "tegra.h"

#define NVN(name) HLE_FUNC(#name, hle_##name)

// argument helpers
#define A(i) (c->x[(i)])
#define AP(i) ((void *)(uintptr_t)c->x[(i)])
#define AF(i) (*(float *)&c->v[(i)])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define RETF(f) do { c->v[0] = 0; *(float *)&c->v[0] = (f); } while (0)
#define HOST(T, guest) ((T *)(uintptr_t)(*(uint64_t *)(uintptr_t)(guest)))
#define SET_HOST(guest, p) (*(uint64_t *)(uintptr_t)(guest) = (uint64_t)(uintptr_t)(p))
#define G(T, base, off) (*(T *)((uint8_t *)(uintptr_t)(base) + (off)))

typedef struct NvnPool {
    uint64_t storage, size;
    uint32_t flags;
    void *command_storage;
} NvnPool;

typedef struct NvnTexture {
    TexLayout layout;
    uint32_t target, format, flags, width, height, depth, levels, samples, ds_mode;
    uint8_t swizzle[4];
    uint64_t stride;
    uint64_t addr;  // guest address of texel storage
    NvnPool *pool;
    uint64_t pool_offset;
    void *backend;  // renderer-side texture
    uint64_t debug_id;
    char label[64];
} NvnTexture;

typedef struct {
    uint32_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_aniso;
    float border[4];
    void *backend;
} NvnSamplerDesc;

typedef struct {
    uint32_t setmask;  // which fields are overridden
    uint16_t base_level, num_levels, min_layer, num_layers;
    uint32_t format, target, ds_mode;
    uint8_t swizzle[4];
} NvnTextureView;  // expanded host view; the guest's NVN 55 view occupies only 16 bytes

static inline bool nvn_compact_texture_views(void) { return title_profile_current()->nvn_major >= 55; }
static inline NvnTextureView nvn_texture_view_read(const void *guest) {
    NvnTextureView v = {0};
    if (!nvn_compact_texture_views()) { memcpy(&v, guest, sizeof v); return v; }
    uint64_t bits;
    memcpy(&bits, guest, 8);
    v.min_layer = (uint16_t)bits; v.num_layers = (uint16_t)(bits >> 16);
    v.base_level = (bits >> 32) & 255; v.num_levels = (bits >> 40) & 255;
    v.target = (bits >> 48) & 255; v.setmask = (bits >> 56) & 63; v.ds_mode = bits >> 62;
    memcpy(&v.format, (const uint8_t *)guest + 8, 4);
    memcpy(v.swizzle, (const uint8_t *)guest + 12, 4);
    return v;
}
static inline void nvn_texture_view_write(void *guest, const NvnTextureView *v) {
    if (!nvn_compact_texture_views()) { memcpy(guest, v, sizeof *v); return; }
    uint64_t bits = (uint64_t)v->min_layer | (uint64_t)v->num_layers << 16 |
        (uint64_t)(v->base_level & 255) << 32 | (uint64_t)(v->num_levels & 255) << 40 |
        (uint64_t)(v->target & 255) << 48 | (uint64_t)(v->setmask & 63) << 56 | (uint64_t)(v->ds_mode & 3) << 62;
    memcpy(guest, &bits, 8);
    memcpy((uint8_t *)guest + 8, &v->format, 4);
    memcpy((uint8_t *)guest + 12, v->swizzle, 4);
}

typedef struct {
    NvnTexture *tex;
    NvnTextureView view;
    bool has_view;
} NvnTexEntry;

typedef struct NvnDescPool {
    uint64_t addr;
    int count;
    void *entries;  // NvnTexEntry[] or NvnSamplerDesc[]
} NvnDescPool;

typedef struct NvnShaderStage {
    uint32_t stage;           // NVNshaderStage
    uint64_t code_addr;       // GPU (== CPU) address of the code
    const uint8_t *control;   // control section
    uint32_t const_offset, const_size;  // compiler constants (bound as c1), within the code blob
    void *backend;
} NvnShaderStage;

typedef struct NvnProgram {
    NvnShaderStage stages[6];
    uint32_t stage_mask;
    void *backend;
    uint64_t revision;        // changes when shader binaries are replaced, even at reused addresses
} NvnProgram;

typedef struct NvnWindow {
    uint64_t textures[8];  // guest NVNtexture*
    int num, active, interval, next;
    int crop[4];
    uint64_t present_time_ns;
} NvnWindow;

typedef struct NvnSync {
    bool armed, signaled;
    uint64_t seq;  // GPU submission this fence belongs to (renderer_fence_seq), 0 = none
    pthread_cond_t submitted;
} NvnSync;
void nvn_sync_arm(uint64_t guest);
void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
int renderer_wait_seq_timeout(uint64_t seq, uint64_t timeout);

// ---- host command recording, with guest allocation accounting
typedef struct {
    NvnPool *pool;
    uint64_t mem;    // current command memory chunk (guest address)
    uint64_t size;   // chunk size
    uint64_t used;   // bytes used in chunk
    uint64_t callback, callback_data;
    uint64_t control_mem, control_size, control_used;
    uint64_t control_start;
    uint8_t *records;
    size_t records_used, records_capacity;
    void **record_refs;
    size_t record_ref_count, record_ref_capacity;
    bool recording, callback_enabled;
    uint64_t guest;  // the guest NVNcommandBuffer*
} NvnCmdBuf;

// One recorded command: header, the call's integer and float arguments, then copied data.
typedef struct {
    uint32_t op, size;  // size includes the header, multiple of 8
    uint64_t x[7];      // x1..x7 of the call
    float f[4];         // s0..s3 of the call
    uint8_t extra[];
} CmdRec;

// Expanded records are frozen per control-memory handle and survive destruction
// of the recorder. Pool destruction and control-handle reuse retire them.
void nvn_command_pool_destroy(NvnPool *pool);
void nvn_command_handle_destroy(uint64_t handle);
void *nvn_command_list_acquire(uint64_t handle);
uint64_t nvn_command_list_records(void *list);
void nvn_command_list_release(void *list);

enum {
    CMD_END = 0, CMD_JUMP, CMD_CALL,
    CMD_BIND_PROGRAM, CMD_BIND_BLEND, CMD_BIND_CHANNEL_MASK, CMD_BIND_COLOR, CMD_BIND_DEPTH_STENCIL,
    CMD_BIND_MULTISAMPLE, CMD_BIND_POLYGON, CMD_BIND_VERTEX_ATTRIB, CMD_BIND_VERTEX_STREAM,
    CMD_BIND_VERTEX_BUFFERS, CMD_BIND_UNIFORM_BUFFERS, CMD_BIND_STORAGE_BUFFERS, CMD_BIND_TEXTURES,
    CMD_BIND_IMAGES, CMD_SET_TEXTURE_POOL, CMD_SET_SAMPLER_POOL, CMD_SET_RENDER_TARGETS,
    CMD_SET_VIEWPORTS, CMD_SET_SCISSORS, CMD_SET_DEPTH_RANGES, CMD_SET_BLEND_COLOR, CMD_SET_STENCIL,
    CMD_SET_PRIMITIVE_RESTART, CMD_SET_ALPHA_REF, CMD_SET_DEPTH_BOUNDS, CMD_SET_POLYGON_OFFSET,
    CMD_SET_LINE_WIDTH, CMD_SET_POINT_SIZE, CMD_SET_SAMPLE_MASK, CMD_SET_RENDER_ENABLE,
    CMD_CLEAR_COLOR, CMD_CLEAR_DEPTH_STENCIL, CMD_CLEAR_TEXTURE, CMD_CLEAR_BUFFER,
    CMD_DRAW_ARRAYS, CMD_DRAW_ELEMENTS, CMD_DRAW_ARRAYS_INDIRECT, CMD_DRAW_ELEMENTS_INDIRECT,
    CMD_DISPATCH, CMD_DISPATCH_INDIRECT,
    CMD_COPY_BUFFER_TO_BUFFER, CMD_COPY_BUFFER_TO_TEXTURE, CMD_COPY_TEXTURE_TO_BUFFER,
    CMD_COPY_TEXTURE_TO_TEXTURE, CMD_DOWNSAMPLE, CMD_UPDATE_UNIFORM_BUFFER, CMD_FENCE_SYNC, CMD_WAIT_SYNC,
    CMD_BARRIER, CMD_SIGNAL_EVENT, CMD_WAIT_EVENT, CMD_REPORT_COUNTER, CMD_DISCARD_COLOR,
    CMD_DISCARD_DEPTH_STENCIL, CMD_SET_COPY_STRIDES, CMD_DRAW_TEXTURE, CMD_RESOLVE_DEPTH,
    CMD_REPORT_VALUE, CMD_COUNT
};



// renderer backend (runtime/nvn/metal.m)
void backend_init(void);
void backend_execute(uint64_t handle);            // run a recorded command list
#ifndef __APPLE__
void backend_prepare_program(NvnProgram *program); // Vulkan: snapshot and prepare registered shaders ahead of draws
#endif
void backend_present(NvnTexture *tex);            // show a texture in the window
void backend_texture_destroyed(NvnTexture *tex);
void backend_pool_written(uint64_t addr, uint64_t size);
