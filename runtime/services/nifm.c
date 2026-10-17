// Local/offline network-interface service. ACNH initializes this even in solo
// play. Command layouts: switchbrew/libnx nx/source/services/nifm.c and
// nx/include/switch/services/nifm.h. No sockets or remote sessions are opened.
#include <stdlib.h>
#include <string.h>
#include "../hle.h"
#include "../ipc.h"

#define NIFM_NO_INTERNET 0xD46EDu
typedef struct { KEvent *changed, *finished; uint32_t handles[2]; } NifmRequest;

static void signal_request(NifmRequest *r) {
    pthread_mutex_lock(&g_klock);
    kernel_signal(r->changed);
    kernel_signal(r->finished);
    pthread_mutex_unlock(&g_klock);
}

static void request_close(Service *s) { free(s->state); }
static void request(Service *s, IpcReq *req, IpcResp *resp) {
    NifmRequest *r = s->state;
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break; // idle / error, never waiting
    case 1: resp->result = NIFM_NO_INTERNET; break;
    case 2:
        resp->copy_handles[resp->ncopy++] = r->handles[0];
        resp->copy_handles[resp->ncopy++] = r->handles[1];
        break;
    case 3: case 4: signal_request(r); break; // cancel / submit completes offline
    default: LOG("[nifm] request option %u (offline)\n", req->cmd); break;
    }
}

static void general(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: { // GetClientId
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 4) *(uint32_t *)(uintptr_t)b->addr = 1;
        break;
    }
    case 4: {
        NifmRequest *r = calloc(1, sizeof *r);
        uint32_t write_handle;
        r->changed = kernel_create_event_pair(&write_handle, &r->handles[0]);
        r->finished = kernel_create_event_pair(&write_handle, &r->handles[1]);
        signal_request(r);
        Service *obj = service_new("nifm::IRequest", request, r);
        obj->on_close = request_close;
        ipc_return_object(resp, obj);
        break;
    }
    case 5: case 8: case 12: case 15: case 18:
        resp->result = NIFM_NO_INTERNET;
        break;
    case 7: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break; // no saved networks
    case 17: case 20: case 21: case 22:
        OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    default: LOG("[nifm] general option %u (offline)\n", req->cmd); break;
    }
}

static void root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 4 || req->cmd == 5)
        ipc_return_object(resp, service_new("nifm::IGeneralService", general, NULL));
    else resp->result = MAKE_RESULT(10, 221);
}
SERVICE("nifm:u", nifm_user) { root(s, req, resp); }
SERVICE("nifm:s", nifm_system) { root(s, req, resp); }
SERVICE("nifm:a", nifm_admin) { root(s, req, resp); }
