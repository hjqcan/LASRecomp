// Exercise the production wrapper with a deliberately overlapping save worker
// and query, using an SDK stand-in that rejects duplicate mounts.
#include <assert.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include "../runtime/hle/save_mount.c"

static atomic_int active, mounted, unmounted, fail_next, entered, release_first, waiting;
static const char mount_name[] = "save";
uint64_t plat_now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
void plat_sleep_until_ns(uint64_t until) { while (plat_now_ns() < until) usleep(100); }
void rt_unwind_point(void) { atomic_fetch_add(&waiting, 1); }
static void sdk_mount(Cpu *c) {
    if (atomic_exchange(&fail_next, 0)) { c->x[0] = 0x1234; return; }
    c->x[0] = atomic_exchange(&active, 1) ? 0x7802 : 0;
    atomic_fetch_add(&mounted, 1);
}
static void sdk_unmount(Cpu *c) {
    assert(atomic_exchange(&active, 0));
    atomic_fetch_add(&unmounted, 1); c->x[0] = 0;
}
uint64_t guest_sym(const char *name) { return strstr(name, "MountSaveData") ? 1 : 2; }
GuestFn dispatch_lookup(uint64_t address) { return address == 1 ? sdk_mount : sdk_unmount; }
static void mount(Cpu *c) { c->x[0] = (uintptr_t)mount_name; mount_save_serialized(c); }
static void unmount(Cpu *c) { c->x[0] = (uintptr_t)mount_name; unmount_save_serialized(c); }
static void *first(void *unused) {
    (void)unused; Cpu c = {0}; mount(&c); assert(!c.x[0]); atomic_store(&entered, 1);
    while (!atomic_load(&release_first)) usleep(100);
    assert(atomic_load(&active)); unmount(&c); return NULL;
}
static void *second(void *unused) {
    (void)unused; Cpu c = {0}; mount(&c); assert(!c.x[0]); unmount(&c); return NULL;
}
int main(void) {
    for (int i = 0; i < 50; ++i) {
        entered = release_first = waiting = mounted = unmounted = 0;
        pthread_t a, b; assert(!pthread_create(&a, NULL, first, NULL));
        while (!atomic_load(&entered)) usleep(100);
        assert(!pthread_create(&b, NULL, second, NULL));
        // Wait until the second thread is blocked in the production guard.
        while (atomic_load(&waiting) < 3) usleep(100);
        assert(mounted == 1 && unmounted == 0 && active == 1);
        release_first = 1;
        pthread_join(a, NULL); pthread_join(b, NULL);
        assert(mounted == 2 && unmounted == 2 && active == 0);
        assert(!save_mount_owner && !save_mount_depth);
    }
    Cpu c = {0}, next = {0};
    fail_next = 1; mount(&c); assert(c.x[0] == 0x1234);
    mount(&next); assert(!next.x[0]); unmount(&next);
    // Failed nested acquisition releases only its own depth.
    mount(&c); fail_next = 1; mount(&c); assert(c.x[0] == 0x1234);
    assert(save_mount_owner == &c && save_mount_depth == 1); unmount(&c);
    // Other SDK mount names keep the original behavior.
    c.x[0] = (uintptr_t)"other"; mount_save_serialized(&c);
    assert(!save_mount_owner); c.x[0] = (uintptr_t)"other"; unmount_save_serialized(&c);
    save_mount_acquire(&c); save_mount_reset();
    assert(!save_mount_owner && !save_mount_depth);
    puts("Save mount lifecycle: 50 concurrent transactions, failures, nesting and reboot reset passed");
}
