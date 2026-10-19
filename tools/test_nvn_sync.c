// The production SyncWait must wait for fence publication AND GPU completion.
#include <assert.h>
#include <stdatomic.h>
#include <stdarg.h>
#include "../runtime/hle.h"
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#include "../runtime/nvn/objects.c"

volatile int g_rt_unwind;
static _Atomic uint64_t complete;
static uint64_t test_guest;
bool rq_on(void) { return false; }
uint64_t rq_current_item(void) { return 0; }
int rq_wait_item_timeout(uint64_t q, uint64_t timeout, uint64_t *seq) { (void)q; (void)timeout; (void)seq; abort(); }
void backend_texture_destroyed(NvnTexture *t) { (void)t; abort(); }
void rt_fatal_msg(const char *fmt, ...) { va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); }
void rt_fatal_trace(void) { abort(); }
uint64_t plat_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
int renderer_wait_seq_timeout(uint64_t sequence, uint64_t timeout) {
    uint64_t start = plat_now_ns(); bool waited = false;
    while (atomic_load(&complete) < sequence) {
        if (plat_now_ns()-start >= timeout) return 2;
        struct timespec delay = {.tv_nsec=1000000}; nanosleep(&delay,NULL); waited = true;
    }
    return waited ? 1 : 0;
}
static void *publish(void *arg) {
    struct timespec delay = {.tv_nsec=30000000}; nanosleep(&delay,NULL);
    nvn_sync_set_seq((uint64_t)(uintptr_t)&test_guest, 5);
    nanosleep(&delay,NULL); atomic_store(&complete,5); return NULL;
}
int main(void) {
    Cpu c = {0}; c.x[0] = (uint64_t)(uintptr_t)&test_guest; hle_nvnSyncInitialize(&c);
    c.x[0] = (uint64_t)(uintptr_t)&test_guest; c.x[1]=0; hle_nvnSyncWait(&c); assert(c.x[0]==0);
    nvn_sync_arm((uint64_t)(uintptr_t)&test_guest);
    c.x[0] = (uint64_t)(uintptr_t)&test_guest; c.x[1]=0; hle_nvnSyncWait(&c); assert(c.x[0]==2);
    atomic_store(&complete,4);
    pthread_t producer; pthread_create(&producer,NULL,publish,NULL);
    uint64_t start=plat_now_ns();
    c.x[0] = (uint64_t)(uintptr_t)&test_guest; c.x[1]=1000000000; hle_nvnSyncWait(&c);
    assert(c.x[0]==1 && plat_now_ns()-start >= 50000000);
    pthread_join(producer,NULL);
    c.x[0] = (uint64_t)(uintptr_t)&test_guest; c.x[1]=0; hle_nvnSyncWait(&c); assert(c.x[0]==0);
    nvn_sync_arm((uint64_t)(uintptr_t)&test_guest);
    nvn_sync_set_seq((uint64_t)(uintptr_t)&test_guest,6);
    c.x[0] = (uint64_t)(uintptr_t)&test_guest; c.x[1]=1000000; hle_nvnSyncWait(&c); assert(c.x[0]==2);
    c.x[0] = (uint64_t)(uintptr_t)&test_guest; hle_nvnSyncFinalize(&c);
    puts("PASS: unfenced sync, polling, delayed submission, GPU completion, reuse and timeouts");
}
