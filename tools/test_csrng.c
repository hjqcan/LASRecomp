#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/hle.h"
#include "../runtime/ipc.h"

static unsigned calls;
#if defined(_WIN32) || defined(__linux__)
static bool fail;
#endif
#ifdef __linux__
static bool interrupt_once, short_once;
#endif
#ifdef _WIN32
static int test_rand_s(unsigned int *value) {
    calls++;
    if (fail) return EIO;
    return rand_s(value);
}
#define rand_s test_rand_s
#elif defined(__linux__)
static ssize_t test_getrandom(void *buffer, size_t size, unsigned int flags) {
    calls++;
    if (fail) { errno = EIO; return -1; }
    if (interrupt_once) { interrupt_once = false; errno = EINTR; return -1; }
    if (short_once) { short_once = false; size = 1; }
    return getrandom(buffer, size, flags);
}
#define getrandom test_getrandom
#else
static void test_arc4random_buf(void *buffer, size_t size) {
    calls++;
    arc4random_buf(buffer, size);
}
#define arc4random_buf test_arc4random_buf
#endif
#include "../runtime/services/csrng.c"

void rt_fatal_msg(const char *fmt, ...) { (void)fmt; abort(); }
void rt_fatal_trace(void) { abort(); }

static IpcResp generate(void *out, uint64_t size) {
    IpcReq request = {.nb=1, .b={{(uintptr_t)out, size}}};
    IpcResp response = {0};
    csrng(NULL, &request, &response);
    return response;
}

int main(void) {
    uint8_t bytes[1033], second[1024];
    for (size_t n=0; n<=1024; n++) {
        memset(bytes, 0xA5, sizeof bytes);
        IpcResp r = generate(bytes+3, n);
        assert(!r.result && !r.data_size && !r.nobjects && !r.ncopy && !r.nmove);
        for (size_t i=0; i<3; i++) assert(bytes[i] == 0xA5);
        for (size_t i=3+n; i<sizeof bytes; i++) assert(bytes[i] == 0xA5);
    }
    // A plumbing check, not a statistical claim about the OS entropy source.
    assert(!generate(second, sizeof second).result);
    assert(memcmp(second, bytes+3, sizeof second));
    unsigned before = calls;
    assert(!generate(NULL, 0).result && calls == before);
    assert(generate(NULL, 1).result && calls == before);
    assert(generate((void *)(UINTPTR_MAX-1), 8).result && calls == before);
    IpcReq q = {0}; IpcResp r = {0};
    csrng(NULL, &q, &r); assert(r.result && calls == before);
    q.nc=1; q.c[0]=(IpcBuf){(uintptr_t)bytes, sizeof bytes}; r=(IpcResp){0};
    csrng(NULL, &q, &r); assert(r.result && calls == before);
    q.cmd=1; r=(IpcResp){0}; csrng(NULL, &q, &r); assert(r.result && calls == before);
#if defined(_WIN32) || defined(__linux__)
    memset(bytes, 0xA5, sizeof bytes); fail=true;
    assert(generate(bytes+3, 500).result);
    for (size_t i=0; i<sizeof bytes; i++) assert(bytes[i] == 0xA5);
    fail=false;
#endif
#ifdef __linux__
    interrupt_once=true; short_once=true;
    assert(!generate(bytes+3, 513).result && !interrupt_once && !short_once);
    for (size_t i=0; i<3; i++) assert(bytes[i] == 0xA5);
    for (size_t i=516; i<sizeof bytes; i++) assert(bytes[i] == 0xA5);
#endif
    puts("PASS: csrng host entropy, byte bounds, empty requests, invalid descriptors and OS failure handling");
}
