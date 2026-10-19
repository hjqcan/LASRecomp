// Exercise the real Horizon condition-variable implementation. In particular,
// the SDK reacquires its mutex after timeout, but not after a successful signal.
#include "../runtime/hle.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
// Pause immediately after the real kernel publishes an unlocked mutex, so a
// user-space sender can inspect the wait flag before the waiter continues.
static void after_kernel_store(void *address);
#define __atomic_store_n(ptr, value, order) do { \
    __auto_type test_address = (ptr); \
    __atomic_store_n(test_address, (value), (order)); \
    after_kernel_store((void *)test_address); \
} while (0)
#include "../runtime/kernel.c"
#undef __atomic_store_n
#undef NDEBUG
#include <assert.h>

// Unused platform/IPC entry points from the included kernel translation unit.
// Abort if a test accidentally leaves the synchronization code under test.
Module g_modules[TITLE_MAX_MODULES];
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
const char *hle_name_at(uint64_t pc) { (void)pc; return NULL; }

// Multi's unrelated memory/profile/research entry points also fail closed.
void kernel_memory_reset(void) { abort(); }
uint64_t kernel_memory_info(unsigned id) { (void)id; abort(); }
uint32_t kernel_set_heap_size(uint64_t size) { (void)size; abort(); }
uint32_t kernel_map_physical(uint64_t addr, uint64_t size, bool map) {
    (void)addr; (void)size; (void)map; abort();
}
void kernel_region_add(uint64_t addr, uint64_t size, uint32_t type, uint32_t perm) {
    (void)addr; (void)size; (void)type; (void)perm; abort();
}
void kernel_region_remove(uint64_t addr) { (void)addr; abort(); }
void kernel_query_memory(uint64_t addr, uint64_t out) { (void)addr; (void)out; abort(); }
const TitleProfile *title_profile_current(void) { abort(); }
void research_external(int kind, uint64_t pc, uint32_t thread, uint64_t index, uint64_t value) {
    (void)kind; (void)pc; (void)thread; (void)index; (void)value; abort();
}
void plat_exit(int status) { (void)status; abort(); }

uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static uint32_t mutex_word, condition_word;
static KThread waiter;
static int contended;
static pthread_mutex_t schedule = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t schedule_changed = PTHREAD_COND_INITIALIZER;
static bool gate_release, released, sender_checked;
static uint32_t observed_flag, queued_messages;

static void after_kernel_store(void *address) {
    if (address != &mutex_word || !gate_release) return;
    gate_release = false;
    pthread_mutex_lock(&schedule);
    released = true;
    pthread_cond_broadcast(&schedule_changed);
    while (!sender_checked) pthread_cond_wait(&schedule_changed, &schedule);
    pthread_mutex_unlock(&schedule);
}

static void *send_during_release(void *unused) {
    (void)unused;
    pthread_mutex_lock(&schedule);
    while (!released) pthread_cond_wait(&schedule_changed, &schedule);
    pthread_mutex_unlock(&schedule);
    uint32_t expected = 0;
    assert(__atomic_compare_exchange_n(&mutex_word, &expected, 0x222, false,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    queued_messages = 1;
    observed_flag = __atomic_load_n(&condition_word, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&schedule);
    sender_checked = true;
    pthread_cond_broadcast(&schedule_changed);
    pthread_mutex_unlock(&schedule);
    // Like the SDK, omit the syscall when the condition has no waiters.
    if (observed_flag) svc_signal_process_wide_key((uint64_t)&condition_word, 1);
    expected = 0x222;
    if (!__atomic_compare_exchange_n(&mutex_word, &expected, 0, false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        assert(svc_arbitrate_unlock((uint64_t)&mutex_word) == R_OK);
    return NULL;
}
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

// The address arbiter's read-modify-writes race user space's own updates of the word (exclusive load / store, no
// kernel lock); this thread is that user space.
static uint32_t arbiter_word;
static volatile int arbiter_stop;
static unsigned long arbiter_adds;
static void *arbiter_adder(void *unused) {
    (void)unused;
    while (!arbiter_stop) { __atomic_fetch_add(&arbiter_word, 2, __ATOMIC_SEQ_CST); arbiter_adds++; }
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
    mutex_word = waiter.handle;
    condition_word = 0;
    gate_release = true;
    pthread_t sender;
    assert(pthread_create(&sender, NULL, send_during_release, NULL) == 0);
    uint32_t r = svc_wait_process_wide_key((uint64_t)&mutex_word,
        (uint64_t)&condition_word, waiter.handle, 1000000000);
    pthread_join(sender, NULL);
    assert(observed_flag == 1 && queued_messages == 1);
    assert(r == R_OK && mutex_word == waiter.handle && g_waitlist == NULL);
    mutex_release((uint64_t)&mutex_word);
    {   // A pending CancelSynchronization is for WaitSynchronization only: a condition variable wait and a polling
        // WaitSynchronization (timeout 0) leave it, the next blocking WaitSynchronization returns cancelled.
        KObject *ev = calloc(1, sizeof *ev);
        ev->type = KOBJ_EVENT;
        uint32_t h = kernel_new_handle(ev);
        int32_t idx = -1;
        waiter.cancel = true;
        mutex_word = waiter.handle;
        uint32_t r = svc_wait_process_wide_key((uint64_t)&mutex_word, (uint64_t)&condition_word, waiter.handle, 1000000);
        assert(r == R_TIMEOUT && waiter.cancel);
        assert(svc_wait_synchronization((uint64_t)&h, 1, 0, &idx) == R_TIMEOUT && waiter.cancel);
        assert(svc_wait_synchronization((uint64_t)&h, 1, -1, &idx) == R_CANCELLED && !waiter.cancel);
    }
    {   // Address arbiter updates are atomic against user space: none of its changes to the word are lost.
        pthread_t adder;
        pthread_create(&adder, NULL, arbiter_adder, NULL);
        long kernel_delta = 0;
        for (int i = 0; i < 300000; i++) {
            // decrement if less than (timeout 0: the decrement, then the timeout)
            if (svc_wait_for_address((uint64_t)&arbiter_word, ARB_DEC_WAIT_LESS, INT32_MAX, 0) == R_TIMEOUT) kernel_delta--;
            // increment if equal
            int32_t v = (int32_t)__atomic_load_n(&arbiter_word, __ATOMIC_SEQ_CST);
            if (svc_signal_to_address((uint64_t)&arbiter_word, ARB_SIGNAL_INC_EQ, v, 0) == R_OK) kernel_delta++;
        }
        arbiter_stop = 1;
        pthread_join(adder, NULL);
        assert(arbiter_word == (uint32_t)(2 * arbiter_adds + kernel_delta));
    }
    pthread_cond_destroy(&waiter.cond);
    puts("PASS: timeout returns unlocked; repeated SDK reacquire; signal returns owned; contended ownership transfer; "
         "wait flag precedes user-space unlock; cancel kept for WaitSynchronization; atomic address arbiter updates");
    return 0;
}
