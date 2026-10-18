// Online-save backup status for offline applications. Local save files and
// runtime backups are independent of this service. No cloud work is scheduled.
#include <string.h>
#include "../hle.h"
#include "../ipc.h"

// ABI and result codes: Ryujinx IOlscServiceForApplication (MIT).
#define OLSC_NULL_ARGUMENT MAKE_RESULT(179, 100)
#define OLSC_NOT_INITIALIZED MAKE_RESULT(179, 101)
#define OLSC_UNSUPPORTED MAKE_RESULT(10, 221)

SERVICE("olsc:u", olsc_user) {
    if (req->cmd == 0) { // Initialize(pid); no per-session allocation needed.
        s->state = (void *)(uintptr_t)1;
        return;
    }
    if (!s->state) { resp->result = OLSC_NOT_INITIALIZED; return; }
    if (req->cmd == 13 || req->cmd == 14) {
        unsigned uid_offset = req->cmd == 13 ? 0 : 8;
        if (req->data_size < uid_offset + 16) {
            resp->result = OLSC_NULL_ARGUMENT; return;
        }
        uint64_t uid[2]; memcpy(uid, req->data + uid_offset, sizeof uid);
        if (!(uid[0] | uid[1])) { resp->result = OLSC_NULL_ARGUMENT; return; }
        if (req->cmd == 13) { // GetSaveDataBackupSetting -> u8 (disabled)
            OUT(uint8_t, 0) = 2;
            resp_size(resp, 1);
        } else { // Allow disabling; enabling cloud backup is unavailable.
            uint64_t enabled; memcpy(&enabled, req->data, 8);
            if (enabled) resp->result = OLSC_UNSUPPORTED;
        }
        return;
    }
    resp->result = OLSC_UNSUPPORTED;
}
