// nfp:user - amiibo. Behaves like a console with NFC available but no amiibo ever touched: the game's own
// "no amiibo" path runs (issue #282: a null IUser used to crash Dampe's amiibo option).
// Command ids follow the public IUser interface layout.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

enum { NFP_STATE_NONE = 0, NFP_STATE_INITIALIZED = 1 };
enum { DEV_INITIALIZED = 0, DEV_SEARCHING = 1 };

// Result (module 115): no tag in range.
#define NFP_RESULT(desc) (0x73u | ((uint32_t)(desc) << 9))
#define RESULT_DEVICE_NOT_FOUND NFP_RESULT(64)

typedef struct {
    int state, dev_state;
} NfpUser;

static NfpUser g_nfp;  // one IUser at a time is all a game opens

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

static void nfp_iuser(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    NfpUser *u = &g_nfp;
    switch (req->cmd) {
    case 0:  // Initialize
        u->state = NFP_STATE_INITIALIZED;
        u->dev_state = DEV_INITIALIZED;
        break;
    case 1:  // Finalize
        u->state = NFP_STATE_NONE;
        u->dev_state = DEV_INITIALIZED;
        break;
    case 2: {  // ListDevices -> one NFC reader (the controller), so the game can search and time out / cancel
        IpcBuf *b = out_buf(req, 0);
        int n = 0;
        if (b && b->addr && b->size >= 8) {
            memset((void *)(uintptr_t)b->addr, 0, 8);  // device handle 0 (player 1 controller)
            n = 1;
        }
        OUT(uint32_t, 0) = (uint32_t)n;
        resp_size(resp, 4);
        break;
    }
    case 3:  // StartDetection
        u->dev_state = DEV_SEARCHING;
        break;
    case 4:  // StopDetection
        u->dev_state = DEV_INITIALIZED;
        break;
    case 5: case 6: case 7: case 8: case 9: case 10: case 11: case 12:  // Mount .. CreateApplicationArea
    case 13: case 14: case 15: case 16: case 24:  // Get*Info, RecreateApplicationArea
        resp->result = RESULT_DEVICE_NOT_FOUND;  // no tag present
        break;
    case 17: case 18: case 23:  // Attach{Activate,Deactivate,AvailabilityChange}Event (never signalled)
        resp->copy_handles[resp->ncopy++] = new_event_handle();
        break;
    case 19:  // GetState
        OUT(uint32_t, 0) = (uint32_t)u->state;
        resp_size(resp, 4);
        break;
    case 20:  // GetDeviceState
        OUT(uint32_t, 0) = (uint32_t)u->dev_state;
        resp_size(resp, 4);
        break;
    case 21:  // GetNpadId
        OUT(uint32_t, 0) = 0;  // player 1
        resp_size(resp, 4);
        break;
    case 22:  // GetApplicationAreaSize
        OUT(uint32_t, 0) = 0xD8;
        resp_size(resp, 4);
        break;
    default:
        LOG("[nfp] IUser cmd %u (ok)\n", req->cmd);
        resp_size(resp, 0x10);
    }
    if (req->cmd != 19 && req->cmd != 20) LOG("[nfp] IUser cmd %u -> %x\n", req->cmd, resp->result);
}

SERVICE("nfp:user", nfp_user) {
    (void)s;
    if (req->cmd == 0) {  // CreateUserInterface
        ipc_return_object(resp, service_new("nfp:IUser", nfp_iuser, NULL));
        return;
    }
    LOG("[nfp] IUserManager cmd %u (ok)\n", req->cmd);
}
