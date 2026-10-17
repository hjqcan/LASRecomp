// Offline play-report sink. Reporting calls are acknowledged and discarded;
// no report contents, account information or network traffic are retained.
// Protocol IDs: https://switchbrew.org/wiki/BCAT_services#prepo:a,_prepo:a2,_prepo:m,_prepo:u,_prepo:s
#include "../hle.h"
#include "../ipc.h"

static void prepo_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd >= 10100 && req->cmd <= 10105) {
        // SaveReport[WithUser] across the three SDK generations. UID precedes
        // the application resource id in the odd-numbered variants.
        if (req->data_size < ((req->cmd & 1) ? 24u : 8u) || req->na + req->nx < 2) {
            resp->result = 0x1c01; return;
        }
        static unsigned logged;
        if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 3)
            LOG("[prepo] discarded offline play report (command %u)\n", req->cmd);
        return;
    }
    // This local runtime does not support transmission or report retrieval.
    resp->result = 0x4201;
}
SERVICE("prepo:u", prepo_user) { prepo_service(s, req, resp); }

