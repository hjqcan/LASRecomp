// Solo-play friends service: an empty local friend list and completed local
// presence updates. IPC IDs: https://switchbrew.org/wiki/Friend_services and
// switchbrew/libnx nx/source/services/friends.c. No remote requests are sent.
#include <stdlib.h>
#include <string.h>
#include "../hle.h"
#include "../ipc.h"

typedef struct { KEvent *done; uint32_t readable; } FriendState;

static void finish(FriendState *s) {
    pthread_mutex_lock(&g_klock);
    kernel_signal(s->done);
    pthread_mutex_unlock(&g_klock);
}

static void friends_close(Service *s) { free(s->state); s->state = NULL; }

static void friend_service(Service *s, IpcReq *req, IpcResp *resp) {
    FriendState *f = s->state;
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = f->readable; break;
    case 1: finish(f); break;
    case 10100: case 10101: case 10400: case 20100: case 20101: {
        IpcBuf *b = out_buf(req, 0);
        if (b) memset((void *)(uintptr_t)b->addr, 0, b->size);
        OUT(int32_t, 0) = 0;
        resp_size(resp, 4);
        finish(f);
        break;
    }
    case 10600: case 10601: case 10610: case 10702:
        finish(f); // local presence/session bookkeeping while offline
        break;
    case 20800: {
        IpcBuf *b = out_buf(req, 0);
        if (b) memset((void *)(uintptr_t)b->addr, 0, b->size);
        break;
    }
    default: FATAL("[friends] unsupported offline command %u\n", req->cmd);
    }
}

SERVICE("friend:u", friends_user) {
    (void)s;
    if (req->cmd != 0) FATAL("[friends] unsupported creator command %u\n", req->cmd);
    FriendState *f = calloc(1, sizeof *f);
    uint32_t writable;
    f->done = kernel_create_event_pair(&writable, &f->readable);
    Service *obj = service_new("IFriendService", friend_service, f);
    obj->on_close = friends_close;
    ipc_return_object(resp, obj);
}
