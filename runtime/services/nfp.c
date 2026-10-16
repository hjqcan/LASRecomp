// NFC peripheral discovery with no attached reader. Command protocol:
// https://switchbrew.org/wiki/NFC_services#nfp:user
#include <stdlib.h>
#include <string.h>
#include "../hle.h"
#include "../ipc.h"

typedef struct { bool initialized; uint32_t available; } NfpState;
static void nfp_close(Service *s) { free(s->state); s->state = NULL; }
static void nfp_user(Service *s, IpcReq *req, IpcResp *resp) {
    NfpState *state = s->state;
    switch (req->cmd) {
    case 0: state->initialized = true; break;
    case 1: state->initialized = false; break;
    case 2: {
        IpcBuf *b = out_buf(req, 0);
        if (b) memset((void *)(uintptr_t)b->addr, 0, b->size);
        OUT(int32_t, 0) = 0; resp_size(resp, 4); break;
    }
    case 19: OUT(uint32_t, 0) = state->initialized ? 1 : 0; resp_size(resp, 4); break;
    case 23: resp->copy_handles[resp->ncopy++] = state->available; break;
    default: FATAL("[nfp] unsupported peripheral command %u\n", req->cmd);
    }
}
SERVICE("nfp:user", nfp_manager) {
    (void)s;
    if (req->cmd != 0) FATAL("[nfp] unsupported manager command %u\n", req->cmd);
    NfpState *state = calloc(1, sizeof *state);
    uint32_t writable;
    kernel_create_event_pair(&writable, &state->available);
    Service *user = service_new("NfpIUser", nfp_user, state);
    user->on_close = nfp_close;
    ipc_return_object(resp, user);
}
