// csrng command 0: fill one output map-alias buffer with host OS random bytes.
// Wire contract: https://github.com/switchbrew/libnx/blob/master/nx/source/services/csrng.c
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "../hle.h"
#include "../ipc.h"

static bool csrng_fill(void *buffer, size_t size) {
    uint8_t *out = buffer;
    while (size) {
#ifdef _WIN32
        unsigned int value;
        if (rand_s(&value)) return false;
        size_t n = size < sizeof value ? size : sizeof value;
        memcpy(out, &value, n);
#elif defined(__linux__)
        // Keep OS writes in host memory; the final CPU copy participates in
        // the guest's lazy commitment and dirty-page tracking.
        uint8_t bytes[256];
        size_t want = size < sizeof bytes ? size : sizeof bytes;
        ssize_t got = getrandom(bytes, want, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        size_t n = (size_t)got;
        memcpy(out, bytes, n);
#else
        uint8_t bytes[256];
        size_t n = size < sizeof bytes ? size : sizeof bytes;
        arc4random_buf(bytes, n);
        memcpy(out, bytes, n);
#endif
        out += n;
        size -= n;
    }
    return true;
}

SERVICE("csrng", csrng) {
    (void)s;
    if (req->cmd != 0) {
        resp->result = 0xF601;
        if (getenv("LARECOMP_STRICT_IPC")) FATAL("[csrng] unsupported command %u\n", req->cmd);
        return;
    }
    if (req->nb != 1) { resp->result = 0xE801; return; }
    IpcBuf b = req->b[0];
    if (b.size > SIZE_MAX || (b.size && (!b.addr || b.addr > UINTPTR_MAX - b.size))) {
        resp->result = 0xE801; return;
    }
    if (!csrng_fill((void *)(uintptr_t)b.addr, (size_t)b.size)) {
        // Do not claim success or substitute a weak PRNG after an OS failure.
        resp->result = 0xFA01; return;
    }
    static unsigned logged;
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 3)
        LOG("[csrng] generated %llu bytes\n", (unsigned long long)b.size);
}
