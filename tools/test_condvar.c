// Exercise the real Horizon condition-variable implementation. In particular,
// the SDK reacquires its mutex after timeout, but not after a successful signal.
#include "../runtime/hle.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/kernel.c"
#undef NDEBUG
#include <assert.h>

// Unused platform/IPC entry points from the included kernel translation unit.
// Abort if a test accidentally leaves the synchronization code under test.
Module g_modules[4];
int g_nmodules;
uint32_t vi_vsync_handle(void) { abort(); }
void rt_fatal_trace(void) { abort(); }
void rt_fatal_msg(const char *fmt, ...) { (void)fmt; abort(); }
void plat_thread_guest(int prio) { (void)prio; abort(); }
void plat_commit(uint64_t addr, uint64_t size) { abort(); }
const char *plat_symbol(uint64_t pc, uint64_t *offset) { abort(); }
void plat_print_backtrace(void) { abort(); }
void crash_handler_install(void) { abort(); }
void guest_call(Cpu *c, uint64_t addr) { abort(); }
int ipc_handle_request(Cpu *c, uint32_t handle) { abort(); }
uint32_t ipc_connect_named_port(const char *name) { abort(); }

uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static uint32_t mutex_word, condition_word;
static KThread waiter;
static int contended;
static void *signal_waiter(void *unused) {
    (void)unused;
    // Wait until the real waiter has atomically released its mutex and joined
    // the condition queue; do not depend on a scheduling delay in the test.
    for (;;) {
        pthread_mutex_lock(&g_klock);
        bool ready = g_waitlist == &waiter && waiter.wait_kind == WAIT_CONDVAR;
        if (ready && contended) mutex_word = 0x222;
        pthread_mutex_unlock(&g_klock);
        if (ready) break;
        sched_yield();
    }
    svc_signal_process_wide_key((uint64_t)&condition_word, 1);
    if (contended) {
        pthread_mutex_lock(&g_klock);
        assert(waiter.wait_kind == WAIT_MUTEX);
        mutex_release((uint64_t)&mutex_word);
        pthread_mutex_unlock(&g_klock);
    }
    return NULL;
}

int main(void) {
    waiter.handle = 0x130;
    waiter.prio = 46;
    pthread_cond_init(&waiter.cond, NULL);
    t_cur = &waiter;
    for (int i = 0; i < 40; i++) {
        mutex_word = waiter.handle;
        uint32_t r = svc_wait_process_wide_key((uint64_t)&mutex_word,
            (uint64_t)&condition_word, waiter.handle, i ? 1000000 : 0);
        assert(r == R_TIMEOUT && mutex_word == 0 && g_waitlist == NULL);
        // This is what the real SDK does after receiving R_TIMEOUT. The old
        // kernel had already taken ownership, so this would fail/deadlock.
        assert(mutex_acquire_or_mark((uint64_t)&mutex_word, waiter.handle));
        mutex_release((uint64_t)&mutex_word);
    }
    for (contended = 0; contended <= 1; contended++) {
        mutex_word = waiter.handle;
        pthread_t signaler;
        assert(pthread_create(&signaler, NULL, signal_waiter, NULL) == 0);
        uint32_t r = svc_wait_process_wide_key((uint64_t)&mutex_word,
            (uint64_t)&condition_word, waiter.handle, -1);
        assert(r == R_OK && mutex_word == waiter.handle && g_waitlist == NULL);
        pthread_join(signaler, NULL);
        mutex_release((uint64_t)&mutex_word);
    }
    pthread_cond_destroy(&waiter.cond);
    puts("PASS: timeout returns unlocked; repeated SDK reacquire; signal returns owned; contended ownership transfer");
    return 0;
}
