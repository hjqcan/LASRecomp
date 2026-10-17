// NVN recording uses a private host arena per recorder. Guest command slices
// supply allocation budgets; completed lists live in their control-memory handles.
// This also keeps parallel recordings independent when scratch slices are reused.
#include <stdlib.h>
#include <string.h>
#include "nvn.h"

#define END_SIZE offsetof(CmdRec, x)
#define JUMP_RESERVE (END_SIZE + sizeof(uint64_t))
#define COMMAND_REF_SIZE 16
#define LIST_BUCKETS 4096

typedef struct CommandList {
    uint64_t control;
    NvnPool *pool;
    void *records;
    unsigned refs;
    void **children;
    size_t child_count;
    struct CommandList *next;
} CommandList;
static CommandList *command_lists[LIST_BUCKETS];
static pthread_mutex_t command_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t command_hash(uint64_t address) {
    return ((address >> 2) * UINT64_C(11400714819323198485)) & (LIST_BUCKETS - 1);
}
static void list_release(CommandList *list) {
    if (--list->refs) return;
    for (size_t i = 0; i < list->child_count; i++) list_release(list->children[i]);
    free(list->children); free(list->records); free(list);
}
void *nvn_command_list_acquire(uint64_t handle) {
    pthread_mutex_lock(&command_lock);
    CommandList *list = command_lists[command_hash(handle)];
    while (list && list->control != handle) list = list->next;
    if (!list) FATAL("nvn: unknown command handle %llx\n", handle);
    list->refs++;
    pthread_mutex_unlock(&command_lock);
    return list;
}
uint64_t nvn_command_list_records(void *list) { return (uint64_t)(uintptr_t)((CommandList *)list)->records; }
void nvn_command_list_release(void *list) {
    pthread_mutex_lock(&command_lock); list_release(list); pthread_mutex_unlock(&command_lock);
}
static void command_list_remove(uint64_t handle) {
    CommandList **p = &command_lists[command_hash(handle)];
    while (*p) {
        CommandList *list = *p;
        if (list->control == handle) { *p = list->next; list_release(list); return; }
        p = &list->next;
    }
}
void nvn_command_handle_destroy(uint64_t handle) {
    pthread_mutex_lock(&command_lock);
    command_list_remove(handle);
    pthread_mutex_unlock(&command_lock);
}
void nvn_command_pool_destroy(NvnPool *pool) {
    pthread_mutex_lock(&command_lock);
    for (size_t i = 0; i < LIST_BUCKETS; i++) {
        CommandList **p = &command_lists[i];
        while (*p) {
            CommandList *list = *p;
            if (list->pool == pool) { *p = list->next; list_release(list); }
            else p = &list->next;
        }
    }
    pool->command_storage = NULL;
    pthread_mutex_unlock(&command_lock);
}
RT_ON_RESET(command_storage_reset) {
    for (size_t i = 0; i < LIST_BUCKETS; i++)
        while (command_lists[i]) command_list_remove(command_lists[i]->control);
}
static NvnCmdBuf *cb_of(uint64_t guest) { return HOST(NvnCmdBuf, guest); }
static void control_reserve(Cpu *c, NvnCmdBuf *cb, uint32_t size);
NVN(nvnCommandBufferInitialize) {
    NvnCmdBuf *cb = calloc(1, sizeof *cb);
    if (!cb) FATAL("nvn: command recorder allocation failed\n");
    cb->guest = A(0); cb->callback_enabled = true;
    SET_HOST(A(0), cb); RET(1);
}
NVN(nvnCommandBufferFinalize) {
    NvnCmdBuf *cb = cb_of(A(0));
    for (size_t i = 0; i < cb->record_ref_count; i++) nvn_command_list_release(cb->record_refs[i]);
    free(cb->record_refs);
    free(cb->records); free(cb); SET_HOST(A(0), 0);
}
NVN(nvnCommandBufferSetDebugLabel) { (void)c; }
static CmdRec *raw_rec(uint64_t at, uint32_t op, uint32_t size) {
    CmdRec *r = (CmdRec *)(uintptr_t)at;
    memset(r, 0, size < sizeof *r ? size : sizeof *r);
    r->op = op; r->size = size; return r;
}
NVN(nvnCommandBufferAddCommandMemory) {
    NvnCmdBuf *cb = cb_of(A(0));
    if (!A(1)) FATAL("nvn: null command memory pool\n");
    NvnPool *pool = HOST(NvnPool, A(1));
    uint64_t mem = pool->storage + A(2), size = A(3);
    if (A(2) > pool->size) FATAL("nvn: command memory offset exceeds pool\n");
    // ACNH passes the API's 512-byte minimum even for a shorter final slice.
    // Only the bytes actually remaining in the pool are available for recording.
    if (size > pool->size - A(2)) size = pool->size - A(2);
    if (cb->recording && cb->used) control_reserve(c, cb, 32);
    cb->mem = mem; cb->pool = pool; cb->size = size; cb->used = 0;
}
NVN(nvnCommandBufferAddControlMemory) {
    NvnCmdBuf *cb = cb_of(A(0));
    cb->control_mem = A(1); cb->control_size = A(2); cb->control_used = 0;
}
NVN(nvnCommandBufferGetCommandMemorySize) { RET(cb_of(A(0))->size); }
NVN(nvnCommandBufferGetCommandMemoryUsed) { RET(cb_of(A(0))->used); }
NVN(nvnCommandBufferGetCommandMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    RET(cb->size > cb->used ? cb->size - cb->used : 0);
}
NVN(nvnCommandBufferGetControlMemorySize) { RET(cb_of(A(0))->control_size); }
NVN(nvnCommandBufferGetControlMemoryUsed) { RET(cb_of(A(0))->control_used); }
NVN(nvnCommandBufferGetControlMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    RET(cb->control_size > cb->control_used ? cb->control_size - cb->control_used : 0);
}
NVN(nvnCommandBufferSetMemoryCallback) { cb_of(A(0))->callback = A(1); }
NVN(nvnCommandBufferGetMemoryCallback) { RET(cb_of(A(0))->callback); }
NVN(nvnCommandBufferSetMemoryCallbackData) { cb_of(A(0))->callback_data = A(1); }
NVN(nvnCommandBufferGetMemoryCallbackData) { RET(cb_of(A(0))->callback_data); }
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled) { cb_of(A(0))->callback_enabled = !!A(1); }
static void memory_callback(Cpu *c, NvnCmdBuf *cb, unsigned event, unsigned size) {
    uint64_t saved[8]; memcpy(saved, c->x, sizeof saved);
    c->x[0] = cb->guest; c->x[1] = event; c->x[2] = size; c->x[3] = cb->callback_data;
    guest_call(c, cb->callback); memcpy(c->x, saved, sizeof saved);
}
static void control_reserve(Cpu *c, NvnCmdBuf *cb, uint32_t size) {
    for (int tries = 0; !cb->control_mem || cb->control_used + size + 16 > cb->control_size; tries++) {
        if (!cb->callback || tries == 2)
            FATAL("nvn: out of control memory (need %u, used %llu/%llu)\n", size, cb->control_used, cb->control_size);
        memory_callback(c, cb, 1, 256);
    }
    cb->control_used += size;
}
static CmdRec *rec_with_cost(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra, uint32_t size) {
    if (extra > UINT32_MAX - sizeof(CmdRec) - 7) FATAL("nvn: oversized command payload\n");
    uint32_t payload_size = op == CMD_END ? END_SIZE : (uint32_t)((sizeof(CmdRec) + extra + 7) & ~7u);
    // Begin/end metadata and chunk links live in host/control storage. They do
    // not consume guest GPU command bytes or need a guest-memory jump reserve.
    for (int tries = 0; size && (!cb->mem || cb->used + size > cb->size); tries++) {
        if (!cb->callback || !cb->callback_enabled || tries == 2)
            FATAL("nvn: out of command memory (need %u, op %u, used %llu/%llu, callback %llx)\n",
                size, op, cb->used, cb->size, cb->callback);
        memory_callback(c, cb, 0, size);
    }
    size_t need = cb->records_used + payload_size;
    if (need > cb->records_capacity) {
        size_t capacity = cb->records_capacity ? cb->records_capacity * 2 : 256;
        if (capacity < need) capacity = need;
        uint8_t *records = realloc(cb->records, capacity);
        if (!records) FATAL("nvn: command arena allocation failed\n");
        cb->records = records; cb->records_capacity = capacity;
    }
    CmdRec *r = raw_rec((uint64_t)(uintptr_t)(cb->records + cb->records_used), op, payload_size);
    cb->records_used += payload_size; cb->used += size;
    return r;
}
static CmdRec *rec(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    return rec_with_cost(c, cb, op, extra, op == CMD_END ? 0 : COMMAND_REF_SIZE);
}
NVN(nvnCommandBufferBeginRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    if (cb->recording) FATAL("nvn: nested command recording\n");
    cb->recording = true; cb->records_used = 0;
    control_reserve(c, cb, 16);
    cb->control_start = cb->control_mem + cb->control_used - 16;
    rec_with_cost(c, cb, CMD_BARRIER, 0, 0);
}
NVN(nvnCommandBufferEndRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    if (!cb->recording) { RET(0); return; }
    rec(c, cb, CMD_END, 0); control_reserve(c, cb, 40);
    cb->recording = false;
    CommandList *list = calloc(1, sizeof *list);
    if (!list) FATAL("nvn: command-list handle allocation failed\n");
    list->control = cb->control_start; list->pool = cb->pool; list->records = cb->records;
    list->refs = 1; list->children = cb->record_refs; list->child_count = cb->record_ref_count;
    cb->record_refs = NULL; cb->record_ref_count = cb->record_ref_capacity = 0;
    cb->records = NULL; cb->records_used = cb->records_capacity = 0;
    pthread_mutex_lock(&command_lock);
    command_list_remove(list->control);
    size_t bucket = command_hash(list->control);
    list->next = command_lists[bucket]; command_lists[bucket] = list;
    list->pool->command_storage = list->pool;
    CmdRec *jump = raw_rec(list->control, CMD_JUMP, JUMP_RESERVE);
    jump->x[0] = (uint64_t)(uintptr_t)list->records;
    pthread_mutex_unlock(&command_lock);
    RET(list->control);
}
NVN(nvnCommandBufferIsRecording) { RET(cb_of(A(0))->recording); }

// ---- generic recorders
typedef struct { uint64_t x[8]; float f[4]; } Snap;
static Snap snap(Cpu *c) {
    Snap s;
    memcpy(s.x, c->x, sizeof s.x);
    for (int i = 0; i < 4; i++) s.f[i] = *(float *)&c->v[i];
    return s;
}
static CmdRec *record(Cpu *c, uint32_t op, uint32_t extra, Snap *s) {
    CmdRec *r = rec(c, cb_of(s->x[0]), op, extra);
    memcpy(r->x, s->x + 1, sizeof r->x);
    memcpy(r->f, s->f, sizeof r->f);
    return r;
}
// record with a copy of `len` bytes from the guest pointer in argument `arg`
static CmdRec *record_copy(Cpu *c, uint32_t op, int arg, uint32_t len) {
    Snap s = snap(c);
    CmdRec *r = record(c, op, len, &s);
    if (s.x[arg] && len) memcpy(r->extra, (void *)(uintptr_t)s.x[arg], len);
    return r;
}
#define PLAIN(name, op) NVN(name) { Snap s = snap(c); record(c, op, 0, &s); }

// ---- state binding
NVN(nvnCommandBufferBindProgram) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_PROGRAM, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnProgram, s.x[1]);  // host program
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, CMD_BIND_BLEND, 1, 8); }
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, CMD_BIND_CHANNEL_MASK, 1, 4); }
NVN(nvnCommandBufferBindColorState) { record_copy(c, CMD_BIND_COLOR, 1, 4); }
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, CMD_BIND_DEPTH_STENCIL, 1, 8); }
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, CMD_BIND_MULTISAMPLE, 1, 0x18); }
NVN(nvnCommandBufferBindPolygonState) { record_copy(c, CMD_BIND_POLYGON, 1, 4); }
NVN(nvnCommandBufferBindVertexAttribState) { record_copy(c, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4); }
NVN(nvnCommandBufferBindVertexStreamState) { record_copy(c, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8); }

// (cmd, first, count, const NVNbufferRange*) and the single-binding forms
NVN(nvnCommandBufferBindVertexBuffers) { record_copy(c, CMD_BIND_VERTEX_BUFFERS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferBindVertexBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_VERTEX_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = 1;
    memcpy(r->extra, &s.x[2], 16);
}
// (cmd, stage, first, count, const NVNbufferRange*)
NVN(nvnCommandBufferBindUniformBuffers) { record_copy(c, CMD_BIND_UNIFORM_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindUniformBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_UNIFORM_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
NVN(nvnCommandBufferBindStorageBuffers) { record_copy(c, CMD_BIND_STORAGE_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindStorageBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_STORAGE_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
// (cmd, stage, first, count, const handle*)
NVN(nvnCommandBufferBindTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindImages) { record_copy(c, CMD_BIND_IMAGES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindImage) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_IMAGES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindSeparateTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateSamplers) { (void)c; }
NVN(nvnCommandBufferBindSeparateSampler) { (void)c; }
NVN(nvnCommandBufferBindTransformFeedbackBuffers) { (void)c; }
NVN(nvnCommandBufferBindTransformFeedbackBuffer) { (void)c; }
NVN(nvnCommandBufferBindCoverageModulationTable) { (void)c; }

NVN(nvnCommandBufferSetTexturePool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_TEXTURE_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}
NVN(nvnCommandBufferSetSamplerPool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SAMPLER_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}

// SetRenderTargets(cmd, n, colors[], colorViews[], depth, depthView)
// extra: { u64 tex[8]; NvnTextureView views[8]; u8 has_view[8]; u64 depth; NvnTextureView dview; u8 has_dview }
typedef struct {
    uint64_t tex[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    uint64_t depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtRec;
NVN(nvnCommandBufferSetRenderTargets) {
    Snap s = snap(c);
    int n = (int)s.x[1];
    if (n > 8) n = 8;
    RtRec rt = {0};
    const uint64_t *colors = (const uint64_t *)(uintptr_t)s.x[2], *views = (const uint64_t *)(uintptr_t)s.x[3];
    for (int i = 0; i < n; i++) {
        rt.tex[i] = colors && colors[i] ? (uint64_t)(uintptr_t)HOST(NvnTexture, colors[i]) : 0;
        if (views && views[i]) { rt.has_view[i] = 1; memcpy(&rt.views[i], (void *)(uintptr_t)views[i], sizeof(NvnTextureView)); }
    }
    rt.depth = s.x[4] ? (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[4]) : 0;
    if (s.x[5]) { rt.has_dview = 1; memcpy(&rt.dview, (void *)(uintptr_t)s.x[5], sizeof(NvnTextureView)); }
    CmdRec *r = record(c, CMD_SET_RENDER_TARGETS, sizeof rt, &s);
    r->x[0] = (uint64_t)n;
    memcpy(r->extra, &rt, sizeof rt);
}

// viewports/scissors/depth ranges
NVN(nvnCommandBufferSetViewports) { record_copy(c, CMD_SET_VIEWPORTS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetViewport) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_VIEWPORTS, 16, &s);
    float v[4] = {(float)(int32_t)s.x[1], (float)(int32_t)s.x[2], (float)(int32_t)s.x[3], (float)(int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetScissors) { record_copy(c, CMD_SET_SCISSORS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetScissor) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SCISSORS, 16, &s);
    int32_t v[4] = {(int32_t)s.x[1], (int32_t)s.x[2], (int32_t)s.x[3], (int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetDepthRanges) { record_copy(c, CMD_SET_DEPTH_RANGES, 3, (uint32_t)A(2) * 8); }
NVN(nvnCommandBufferSetDepthRange) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_DEPTH_RANGES, 8, &s);
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, s.f, 8);
}
NVN(nvnCommandBufferSetBlendColor) { record_copy(c, CMD_SET_BLEND_COLOR, 1, 16); }
NVN(nvnCommandBufferSetStencilRef) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 0; }
NVN(nvnCommandBufferSetStencilMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 1; }
NVN(nvnCommandBufferSetStencilValueMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 2; }
PLAIN(nvnCommandBufferSetPrimitiveRestart, CMD_SET_PRIMITIVE_RESTART)
PLAIN(nvnCommandBufferSetAlphaRef, CMD_SET_ALPHA_REF)
PLAIN(nvnCommandBufferSetDepthBounds, CMD_SET_DEPTH_BOUNDS)
PLAIN(nvnCommandBufferSetPolygonOffsetClamp, CMD_SET_POLYGON_OFFSET)
PLAIN(nvnCommandBufferSetLineWidth, CMD_SET_LINE_WIDTH)
PLAIN(nvnCommandBufferSetPointSize, CMD_SET_POINT_SIZE)
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_SAMPLE_MASK)
NVN(nvnCommandBufferSetRenderEnable) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_RENDER_ENABLE, 0, &s);
    r->x[6] = 0;
}
NVN(nvnCommandBufferSetRenderEnableConditional) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_RENDER_ENABLE, 0, &s);
    r->x[6] = 1;
}
NVN(nvnCommandBufferSetDepthClamp) { (void)c; }
NVN(nvnCommandBufferSetRasterizerDiscard) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterEnable) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterDilate) { (void)c; }
NVN(nvnCommandBufferSetSubpixelPrecisionBias) { (void)c; }
NVN(nvnCommandBufferSetViewportSwizzles) { (void)c; }
NVN(nvnCommandBufferSetStencilCullCriteria) { (void)c; }
NVN(nvnCommandBufferSetColorReductionEnable) { (void)c; }
NVN(nvnCommandBufferSetColorReductionThresholds) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheAction) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheTileSize) { (void)c; }
NVN(nvnCommandBufferSetShaderScratchMemory) { (void)c; }
NVN(nvnCommandBufferSetPatchSize) { (void)c; }
NVN(nvnCommandBufferSetInnerTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetOuterTessellationLevels) { (void)c; }
NVN(nvnCommandBufferSetProgramSubroutines) { (void)c; }
NVN(nvnCommandBufferSaveZCullData) { (void)c; }
NVN(nvnCommandBufferRestoreZCullData) { (void)c; }

static uint64_t copy_row_stride, copy_image_stride;
RT_ON_RESET(cmdbuf_reset) { copy_row_stride = copy_image_stride = 0; }
NVN(nvnCommandBufferSetCopyRowStride) {
    Snap s = snap(c);
    copy_row_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferSetCopyImageStride) {
    Snap s = snap(c);
    copy_image_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferGetCopyRowStride) { RET(copy_row_stride); }
NVN(nvnCommandBufferGetCopyImageStride) { RET(copy_image_stride); }

// ---- clears
NVN(nvnCommandBufferClearColor) { record_copy(c, CMD_CLEAR_COLOR, 2, 16); }
NVN(nvnCommandBufferClearColori) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 1; }
NVN(nvnCommandBufferClearColorui) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 2; }
PLAIN(nvnCommandBufferClearDepthStencil, CMD_CLEAR_DEPTH_STENCIL)
// ClearTexture(cmd, texture, view, region, color, mask)
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } ClearTexRec;
static void clear_texture(Cpu *c, int kind) {
    Snap s = snap(c);
    ClearTexRec ct = {0};
    ct.tex = (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[1]);
    if (s.x[2]) { ct.has_view = 1; memcpy(&ct.view, (void *)(uintptr_t)s.x[2], sizeof ct.view); }
    if (s.x[3]) memcpy(ct.region, (void *)(uintptr_t)s.x[3], sizeof ct.region);
    if (s.x[4]) memcpy(ct.color, (void *)(uintptr_t)s.x[4], 16);
    CmdRec *r = record(c, CMD_CLEAR_TEXTURE, sizeof ct, &s);
    r->x[6] = (uint64_t)kind;
    memcpy(r->extra, &ct, sizeof ct);
}
NVN(nvnCommandBufferClearTexture) { clear_texture(c, 0); }
NVN(nvnCommandBufferClearTexturei) { clear_texture(c, 1); }
NVN(nvnCommandBufferClearTextureui) { clear_texture(c, 2); }
PLAIN(nvnCommandBufferClearBuffer, CMD_CLEAR_BUFFER)
// DiscardColor(cmdbuf, index): the target's contents become undefined. On the Switch a discarded (compressed)
// target reads back cleared; areas the game then leaves undrawn (e.g. a pit after a floor tile falls away) stay
// black there. Treated as a clear to (0,0,0,0) (LARECOMP_KEEP_DISCARD=1: old no-op behaviour).
NVN(nvnCommandBufferDiscardColor) {
    static int keep = -1, logged;
    if (keep < 0) keep = getenv("LARECOMP_KEEP_DISCARD") != NULL;
    if (logged < 5) { logged++; LOG("[nvn] DiscardColor index %llu%s\n", (unsigned long long)c->x[1], keep ? " (kept)" : " -> clear"); }
    if (keep) return;
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_CLEAR_COLOR, 16, &s);
    memset(r->extra, 0, 16);
    r->x[2] = 0xF;  // all channels
    r->x[5] = 0;    // float clear
}
NVN(nvnCommandBufferDiscardDepthStencil) { (void)c; }

// ---- draws (all integer arguments fit in x1..x7)
NVN(nvnCommandBufferDrawArrays) { Snap s = snap(c); CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0, &s); r->x[3] = 0; r->x[4] = 1; }
NVN(nvnCommandBufferDrawArraysInstanced) { Snap s = snap(c); record(c, CMD_DRAW_ARRAYS, 0, &s); }
// Debug: LARECOMP_TRACE_COUNT=<n> logs the guest call chain of indexed draws with n indices.
static void trace_draw(Cpu *c, uint64_t count) {
    static long want = -2;
    if (want == -2) want = getenv("LARECOMP_TRACE_COUNT") ? atol(getenv("LARECOMP_TRACE_COUNT")) : -1;
    if (want < 0 || (long)count != want) return;
    static int n;
    if (n++ > 40) return;
    LOG("[trace] draw count %llu lr %llx chain:", (unsigned long long)count, (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 14 && fp >= 0x7000000000ull && fp < 0x8000000000ull && (fp & 7) == 0; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
        LOG(" %llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    LOG("\n");
}

NVN(nvnCommandBufferDrawElements) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[4] = 0; r->x[5] = 0; r->x[6] = 1;  // base vertex, base instance, instances
}
NVN(nvnCommandBufferDrawElementsBaseVertex) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[5] = 0; r->x[6] = 1;
}
NVN(nvnCommandBufferDrawElementsInstanced) { trace_draw(c, c->x[3]); Snap s = snap(c); record(c, CMD_DRAW_ELEMENTS, 0, &s); }
PLAIN(nvnCommandBufferDrawArraysIndirect, CMD_DRAW_ARRAYS_INDIRECT)
PLAIN(nvnCommandBufferDrawElementsIndirect, CMD_DRAW_ELEMENTS_INDIRECT)
NVN(nvnCommandBufferMultiDrawArraysIndirectCount) { (void)c; }
NVN(nvnCommandBufferMultiDrawElementsIndirectCount) { (void)c; }
NVN(nvnCommandBufferDrawTransformFeedback) { (void)c; }
NVN(nvnCommandBufferDrawTexture) { (void)c; }
PLAIN(nvnCommandBufferDispatchCompute, CMD_DISPATCH)
PLAIN(nvnCommandBufferDispatchComputeIndirect, CMD_DISPATCH_INDIRECT)
NVN(nvnCommandBufferBeginTransformFeedback) { (void)c; }
NVN(nvnCommandBufferEndTransformFeedback) { (void)c; }
NVN(nvnCommandBufferPauseTransformFeedback) { (void)c; }
NVN(nvnCommandBufferResumeTransformFeedback) { (void)c; }

// ---- copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRef;
static void tex_ref(TexRef *t, uint64_t tex, uint64_t view, uint64_t region) {
    t->tex = tex ? (uint64_t)(uintptr_t)HOST(NvnTexture, tex) : 0;
    if (view) { t->has_view = 1; memcpy(&t->view, (void *)(uintptr_t)view, sizeof t->view); }
    if (region) memcpy(t->region, (void *)(uintptr_t)region, sizeof t->region);
}
NVN(nvnCommandBufferCopyBufferToTexture) {  // (cmd, src, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[2], s.x[3], s.x[4]);
    CmdRec *r = record(c, CMD_COPY_BUFFER_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToBuffer) {  // (cmd, src, srcView, srcRegion, dst, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[1], s.x[2], s.x[3]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_BUFFER, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToTexture) {  // (cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], s.x[3]);
    tex_ref(&t[1], s.x[4], s.x[5], s.x[6]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
PLAIN(nvnCommandBufferCopyBufferToBuffer, CMD_COPY_BUFFER_TO_BUFFER)
NVN(nvnCommandBufferDownsample) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], 0, 0);
    tex_ref(&t[1], s.x[2], 0, 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferDownsampleTextureView) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], 0);
    tex_ref(&t[1], s.x[3], s.x[4], 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferTiledDownsample) { (void)c; }
NVN(nvnCommandBufferTiledDownsampleTextureView) { (void)c; }
NVN(nvnCommandBufferResolveDepthBuffer) { (void)c; }
// UpdateUniformBuffer(cmd, buffer, size, offset, dataSize, data)
NVN(nvnCommandBufferUpdateUniformBuffer) { record_copy(c, CMD_UPDATE_UNIFORM_BUFFER, 5, (uint32_t)A(4)); }

// ---- synchronization and misc
NVN(nvnCommandBufferFenceSync) {
    // work executes at submit time, so the sync is satisfied once this list has run
    Snap s = snap(c);
    record(c, CMD_FENCE_SYNC, 0, &s);
    nvn_sync_arm(s.x[1]);
}
PLAIN(nvnCommandBufferWaitSync, CMD_WAIT_SYNC)
PLAIN(nvnCommandBufferBarrier, CMD_BARRIER)
PLAIN(nvnCommandBufferSignalEvent, CMD_SIGNAL_EVENT)
PLAIN(nvnCommandBufferWaitEvent, CMD_WAIT_EVENT)
PLAIN(nvnCommandBufferReportCounter, CMD_REPORT_COUNTER)
PLAIN(nvnCommandBufferReportValue, CMD_REPORT_VALUE)
NVN(nvnCommandBufferResetCounter) { (void)c; }
NVN(nvnCommandBufferCallCommands) {
    Snap s = snap(c);
    NvnCmdBuf *cb = cb_of(s.x[0]);
    if (s.x[1] > 1000000 || (s.x[1] && !s.x[2])) FATAL("nvn: invalid command-call handles\n");
    size_t count = s.x[1], needed = cb->record_ref_count + count;
    if (needed > cb->record_ref_capacity) {
        size_t capacity = needed * 2;
        void **refs = realloc(cb->record_refs, capacity * sizeof *refs);
        if (!refs) FATAL("nvn: command reference allocation failed\n");
        cb->record_refs = refs; cb->record_ref_capacity = capacity;
    }
    // Capture the list now: CallCommands may outlive the child's control handle.
    const uint64_t *handles = (const void *)(uintptr_t)s.x[2];
    size_t first = cb->record_ref_count;
    for (size_t i = 0; i < count; i++) cb->record_refs[cb->record_ref_count++] = nvn_command_list_acquire(handles[i]);
    CmdRec *r = record(c, CMD_CALL, count * sizeof(uint64_t), &s);
    uint64_t *out = (void *)r->extra;
    for (size_t i = 0; i < count; i++) out[i] = nvn_command_list_records(cb->record_refs[first + i]);
}
static void copy_commands(Cpu *c, NvnCmdBuf *cb, uint64_t handle, int depth) {
    if (depth >= 32) FATAL("nvn: cyclic or excessively nested command copy\n");
    uint64_t address = handle;
    for (unsigned guard = 0; guard < 10000000; guard++) {
        const CmdRec *ref = (const CmdRec *)(uintptr_t)address;
        uint32_t stride = ref->size;
        const CmdRec *src = ref;
        if (src->op == CMD_END) return;
        if (src->op == CMD_JUMP) { address = src->x[0]; continue; }
        if (src->op == CMD_CALL) {
            const uint64_t *handles = (const void *)src->extra;
            for (uint64_t i = 0; i < src->x[0]; i++) copy_commands(c, cb, handles[i], depth + 1);
        } else {
            if (src->size < sizeof *src || src->op >= CMD_COUNT)
                FATAL("nvn: invalid command copy record\n");
            CmdRec *dst = rec(c, cb, src->op, src->size - sizeof *src);
            memcpy(dst, src, src->size);
            if (src->op == CMD_FENCE_SYNC) nvn_sync_arm(src->x[0]);
        }
        address += stride;
    }
    FATAL("nvn: unterminated command copy\n");
}
NVN(nvnCommandBufferCopyCommands) {
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t count = A(1);
    if (count > 1000000 || (count && !A(2))) FATAL("nvn: invalid command-copy handles\n");
    uint64_t *handles = malloc(count * sizeof *handles);
    if (count && !handles) FATAL("nvn: command-copy allocation failed\n");
    if (count) memcpy(handles, AP(2), count * sizeof *handles);
    for (uint64_t i = 0; i < count; i++) {
        void *list = nvn_command_list_acquire(handles[i]);
        copy_commands(c, cb, nvn_command_list_records(list), 0);
        nvn_command_list_release(list);
    }
    free(handles);
}
NVN(nvnCommandBufferInsertDebugMarker) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerDynamic) { (void)c; }
NVN(nvnCommandBufferInsertDebugMarkerStatic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroup) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupDynamic) { (void)c; }
NVN(nvnCommandBufferPushDebugGroupStatic) { (void)c; }
NVN(nvnCommandBufferPopDebugGroup) { (void)c; }
NVN(nvnCommandBufferPopDebugGroupId) { (void)c; }
