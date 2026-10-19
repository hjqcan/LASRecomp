#pragma once
#include "nvn.h"
#include <string.h>

// Look ahead only across state changes and draws. Stop before commands that can
// change guest memory or wait for another producer, then resume after execution.
// The caller snapshots shader inputs and joins each job before its draw executes.
static inline bool nvn_shader_prefetch_barrier(uint32_t op) {
    return op == CMD_DISPATCH || op == CMD_DISPATCH_INDIRECT ||
           (op >= CMD_CLEAR_COLOR && op <= CMD_CLEAR_BUFFER) ||
           (op >= CMD_COPY_BUFFER_TO_BUFFER && op <= CMD_REPORT_COUNTER) ||
           op == CMD_REPORT_VALUE || op == CMD_SET_RENDER_ENABLE ||
           op == CMD_RESOLVE_DEPTH || op == CMD_DRAW_TEXTURE;
}

typedef struct {
    NvnProgram *program;
    uint32_t attrib[16], rt_format[8];
    int nattrib, ncolor;
    bool render_enabled;
} NvnShaderPrefetch;

typedef void (*NvnShaderPrefetchVisit)(const NvnShaderPrefetch *);

static inline bool nvn_shader_prefetch(uint64_t handle, int depth, unsigned *budget,
                                      NvnShaderPrefetch *s, NvnShaderPrefetchVisit visit) {
    for (uint64_t p = handle; *budget; ) {
        --*budget;
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->size < sizeof *r || (r->size & 7) || r->op >= CMD_COUNT) return false;
        if (nvn_shader_prefetch_barrier(r->op)) return false;
        switch (r->op) {
        case CMD_END: return true;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            if (r->x[0] > (r->size - sizeof *r) / sizeof(uint64_t)) return false;
            const uint64_t *h = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++)
                if (!nvn_shader_prefetch(h[i], depth + 1, budget, s, visit)) return false;
            break;
        }
        case CMD_BIND_PROGRAM: s->program = (NvnProgram *)(uintptr_t)r->x[0]; break;
        case CMD_BIND_VERTEX_ATTRIB:
            s->nattrib = r->x[0] > 16 ? 16 : (int)r->x[0];
            if ((size_t)s->nattrib * 4 > r->size - sizeof *r) return false;
            memcpy(s->attrib, r->extra, s->nattrib * 4);
            break;
        case CMD_SET_RENDER_TARGETS: {
            if (r->size - sizeof *r < 8 * sizeof(uint64_t)) return false;
            const uint64_t *tex = (const uint64_t *)r->extra;
            s->ncolor = r->x[0] > 8 ? 8 : (int)r->x[0];
            memset(s->rt_format, 0, sizeof s->rt_format);
            for (int i = 0; i < s->ncolor; i++)
                if (tex[i]) s->rt_format[i] = ((NvnTexture *)(uintptr_t)tex[i])->format;
            break;
        }
        case CMD_DRAW_ARRAYS: case CMD_DRAW_ELEMENTS:
            if (s->render_enabled && s->program) visit(s);
            break;
        default: break;
        }
        p += r->size;
    }
    return false;
}
