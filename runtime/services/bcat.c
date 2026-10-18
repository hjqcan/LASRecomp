// Offline background-delivery service. No delivery cache is downloaded or
// fabricated. IPC layout: https://switchbrew.org/wiki/BCAT_services
#include <string.h>
#include "../hle.h"
#include "../ipc.h"

static void bcat_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 10100: case 10101: case 20100: case 20101:
        resp->result = 0xD46ED; // network unavailable; no progress object on failure
        break;
    case 10200: break;
    default: FATAL("[bcat] unsupported offline command %u\n", req->cmd);
    }
}

static void bcat_empty_entry(Service *s, IpcReq *req, IpcResp *resp) {
    // Creating a reader does not imply that a cached directory/file exists.
    // Open receives one 0x20-byte directory name, plus a file name for files.
    if (req->cmd == 0) {
        unsigned names_size = s->state ? 0x40 : 0x20;
        resp->result = MAKE_RESULT(122, req->data_size < names_size ? 1 : 2);
    } else if (req->cmd <= (s->state ? 3u : 2u)) {
        resp->result = MAKE_RESULT(122, 7); // NotOpen: no bytes or entries returned
    } else resp->result = MAKE_RESULT(10, 221);
}

static void bcat_storage(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0 || req->cmd == 1) {
        int file = req->cmd == 0;
        ipc_return_object(resp, service_new(file ? "IDeliveryCacheFileService" : "IDeliveryCacheDirectoryService",
                                           bcat_empty_entry, (void *)(uintptr_t)file));
    } else if (req->cmd == 10) {
        IpcBuf *b = out_buf(req, 0);
        if (b) memset((void *)(uintptr_t)b->addr, 0, b->size);
        OUT(int32_t, 0) = 0;
        resp_size(resp, 4);
    } else FATAL("[bcat] unsupported cache command %u\n", req->cmd);
}

SERVICE("bcat:u", bcat_user) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("IBcatService", bcat_service, NULL));
    else if (req->cmd == 1 || req->cmd == 2)
        ipc_return_object(resp, service_new("IDeliveryCacheStorageService", bcat_storage, NULL));
    else FATAL("[bcat] unsupported creator command %u\n", req->cmd);
}
