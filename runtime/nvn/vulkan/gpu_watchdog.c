// A driver call can fail to return even when Vulkan was given a timeout.
// Monitor only calls expected to make GPU progress, never shader compilation.
#include "gpu_watchdog.h"
#include "../../platform.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifndef GPU_WATCHDOG_TIMEOUT_NS
#define GPU_WATCHDOG_TIMEOUT_NS (20ull * 1000000000ull)
#endif
#ifndef GPU_WATCHDOG_POLL_NS
#define GPU_WATCHDOG_POLL_NS 100000000ull
#endif
#define GPU_WATCHDOG_SLOTS 64
typedef struct {
    _Atomic uint64_t started;  // 0: free, UINT64_MAX: reserved
    _Atomic(const char *) operation;
    _Atomic uint64_t sequence;
} Guard;
static Guard guards[GPU_WATCHDOG_SLOTS];
static pthread_once_t once = PTHREAD_ONCE_INIT;

static uint64_t awake_now_ns(void) {
#ifdef _WIN32
    // A suspended PC is not a stuck driver. This clock excludes sleep and
    // hibernation; the platform's high-resolution QPC clock does not.
    ULONGLONG ticks;
    if (QueryUnbiasedInterruptTime(&ticks)) return ticks * 100ull;
#endif
    return plat_now_ns();  // Linux CLOCK_MONOTONIC also excludes suspend.
}

static void stop_process(void) {
    fflush(stderr);
    plat_terminate(1);  // no atexit handlers or additional calls to the stalled driver
    abort();
}

static void *watch(void *unused) {
    (void)unused;
    for (;;) {
        uint64_t now = awake_now_ns();
        for (unsigned i = 0; i < GPU_WATCHDOG_SLOTS; i++) {
            Guard *g = &guards[i];
            uint64_t started = atomic_load_explicit(&g->started, memory_order_acquire);
            if (!started || started == UINT64_MAX || now < started || now - started < GPU_WATCHDOG_TIMEOUT_NS) continue;
            const char *operation = atomic_load_explicit(&g->operation, memory_order_relaxed);
            uint64_t sequence = atomic_load_explicit(&g->sequence, memory_order_relaxed);
            // A completion or a new call may have replaced this observation.
            if (!atomic_compare_exchange_strong_explicit(&g->started, &started, UINT64_MAX,
                                                        memory_order_acq_rel, memory_order_relaxed)) continue;
            fprintf(stderr, "FATAL: Vulkan driver stalled in %s (submission %llu) for %.1f s; stopping without GPU cleanup\n",
                    operation, (unsigned long long)sequence, (double)(now - started) / 1e9);
            stop_process();
        }
        plat_sleep_until_ns(plat_now_ns() + GPU_WATCHDOG_POLL_NS);
    }
    return NULL;
}

static void start_watch(void) {
    pthread_t thread;
    int error = pthread_create(&thread, NULL, watch, NULL);
    if (error) {
        fprintf(stderr, "FATAL: cannot start Vulkan driver watchdog (%d)\n", error);
        stop_process();
    }
    pthread_detach(thread);
}

unsigned vk_watchdog_begin(const char *operation, uint64_t sequence) {
    pthread_once(&once, start_watch);
    for (unsigned i = 0; i < GPU_WATCHDOG_SLOTS; i++) {
        uint64_t free = 0;
        Guard *g = &guards[i];
        if (!atomic_compare_exchange_strong_explicit(&g->started, &free, UINT64_MAX,
                                                    memory_order_acquire, memory_order_relaxed)) continue;
        atomic_store_explicit(&g->operation, operation, memory_order_relaxed);
        atomic_store_explicit(&g->sequence, sequence, memory_order_relaxed);
        uint64_t now = awake_now_ns();
        atomic_store_explicit(&g->started, now ? now : 1, memory_order_release);
        return i + 1;
    }
    fprintf(stderr, "FATAL: too many concurrent Vulkan calls for the driver watchdog\n");
    stop_process();
    return 0;
}

void vk_watchdog_end(unsigned guard) {
    if (!guard || guard > GPU_WATCHDOG_SLOTS) abort();
    atomic_store_explicit(&guards[guard - 1].started, 0, memory_order_release);
}
