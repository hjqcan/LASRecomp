// The production map manager with deterministic backing failures and shadow data.
// A separate harness exercises real OS reservation/commit/discard operations.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/rt.h"
#include "../runtime/kernel_memory.h"

#define PAGE 0x1000u
static unsigned char shadow[2 * 1024 * 1024];
static unsigned reservations, resets, commits, discards, fail_at;
static bool reserve_fails;
void rt_fatal_msg(const char *s, ...) { (void)s; abort(); }
void rt_fatal_trace(void) { abort(); }
void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
bool plat_reserve_guest_extra(uint64_t lo, uint64_t hi) {
    assert(lo == GUEST_HI && hi == VIRTUAL_GUEST_HI);
    if (reserve_fails) return false;
    reservations++; return true;
}
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    assert(lo == GUEST_HI && hi == VIRTUAL_GUEST_HI);
    memset(shadow, 0, sizeof shadow); resets++; return true;
}
static void clear_shadow(uint64_t addr, uint64_t size) {
    if (addr >= VIRTUAL_ALIAS_BASE && addr < VIRTUAL_ALIAS_BASE + sizeof shadow) {
        size_t offset = addr - VIRTUAL_ALIAS_BASE;
        if (size > sizeof shadow - offset) size = sizeof shadow - offset;
        memset(shadow + offset, 0, size);
    }
}
bool plat_commit_physical(uint64_t addr, uint64_t size) {
    commits++;
    if (fail_at && commits == fail_at) return false;
    clear_shadow(addr, size); return true;
}
bool plat_discard_physical(uint64_t addr, uint64_t size) {
    discards++; clear_shadow(addr, size); return true;
}
static void query(uint64_t at, uint64_t begin, uint64_t size, uint64_t type) {
    uint64_t info[5]; kernel_query_memory(at, (uintptr_t)info);
    assert(info[0] == begin && info[1] == size && info[2] == type && info[3] == (type == 5 ? 3 : 0));
}
static void reset(uint32_t budget) {
    kernel_memory_reset(); assert(kernel_memory_configure(budget)); fail_at = 0;
}
static void *worker(void *arg) {
    uint64_t base = VIRTUAL_ALIAS_BASE + (uintptr_t)arg * 0x20000;
    for (unsigned i = 0; i < 1000; i++) {
        assert(kernel_map_physical(base, 9 * PAGE, true) == KM_OK);
        assert(kernel_map_physical(base + PAGE, 7 * PAGE, true) == KM_OK);
        assert(kernel_map_physical(base + 3 * PAGE, PAGE, false) == KM_OK);
        uint64_t info[5]; kernel_query_memory(base, (uintptr_t)info);
        assert(info[0] <= base && base < info[0] + info[1] && info[2] == 5);
        assert(kernel_map_physical(base, 9 * PAGE, false) == KM_OK);
    }
    return NULL;
}
static void *overlap_worker(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 1000; i++) {
        assert(kernel_map_physical(VIRTUAL_ALIAS_BASE, 16 * PAGE, true) == KM_OK);
        assert(kernel_map_physical(VIRTUAL_ALIAS_BASE, 16 * PAGE, false) == KM_OK);
    }
    return NULL;
}
int main(void) {
    assert(kernel_memory_configure(0) && reservations == 0);
    assert(kernel_memory_info(2) == ALIAS_BASE && kernel_memory_info(3) == ALIAS_SIZE);
    assert(kernel_memory_info(13) == GUEST_HI - GUEST_LO && kernel_memory_info(16) == 0);
    assert(kernel_set_heap_size(HEAP_MAX) == KM_OK && kernel_heap_size() == HEAP_MAX);
    assert(kernel_set_heap_size(HEAP_MAX + PAGE) == KM_OUT_OF_MEMORY && kernel_heap_size() == HEAP_MAX);
    assert(kernel_map_physical(ALIAS_BASE, PAGE, true) == KM_INVALID_STATE);
    kernel_region_add(GUEST_LO, PAGE, 3, 5);
    uint64_t info[5]; kernel_query_memory(GUEST_LO, (uintptr_t)info);
    assert(info[0] == GUEST_LO && info[1] == PAGE && info[2] == 3 && info[3] == 5);
    kernel_region_remove(GUEST_LO);
    query(GUEST_HI, GUEST_HI, 0 - GUEST_HI, 0x10);
    kernel_memory_reset();
    assert(!kernel_memory_configure(1));
    reserve_fails = true; assert(!kernel_memory_configure(0x1000000));
    assert(kernel_memory_info(16) == 0); reserve_fails = false;
    assert(kernel_memory_configure(0x1000000) && reservations == 1);
    assert(kernel_memory_info(2) == VIRTUAL_ALIAS_BASE && kernel_memory_info(3) == VIRTUAL_ALIAS_SIZE);
    assert(kernel_memory_info(13) == VIRTUAL_GUEST_HI - GUEST_LO);
    uint64_t initial_system = kernel_memory_info(17), initial_used = kernel_memory_info(7);
    assert(initial_system > 0 && initial_system < 0x1000000);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE + 1, PAGE, true) == KM_INVALID_ADDRESS);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE, 0, true) == KM_INVALID_SIZE);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE, 1, true) == KM_INVALID_SIZE);
    assert(kernel_map_physical(UINT64_MAX - PAGE + 1, PAGE, true) == KM_INVALID_REGION);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE - PAGE, PAGE, true) == KM_INVALID_REGION);
    assert(kernel_map_physical(VIRTUAL_GUEST_HI, PAGE, true) == KM_INVALID_REGION);
    assert(kernel_map_physical(VIRTUAL_GUEST_HI - PAGE, 2 * PAGE, true) == KM_INVALID_REGION);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE, VIRTUAL_ALIAS_SIZE, true) == KM_LIMIT_REACHED);
    assert(commits == 0);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE, 4 * PAGE, true) == KM_OK);
    assert(!kernel_memory_configure(0)); // layout cannot change under live mappings
    memset(shadow, 0x5a, 4 * PAGE);
    assert(kernel_memory_info(7) == initial_used + 4 * PAGE && kernel_memory_info(17) == initial_system + PAGE);
    unsigned old_commits = commits;
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE + PAGE, PAGE, true) == KM_OK && commits == old_commits);
    assert(shadow[PAGE] == 0x5a);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE + PAGE, PAGE, false) == KM_OK);
    query(VIRTUAL_ALIAS_BASE, VIRTUAL_ALIAS_BASE, PAGE, 5);
    query(VIRTUAL_ALIAS_BASE + PAGE, VIRTUAL_ALIAS_BASE + PAGE, PAGE, 0);
    query(VIRTUAL_ALIAS_BASE + 2 * PAGE, VIRTUAL_ALIAS_BASE + 2 * PAGE, 2 * PAGE, 5);
    assert(shadow[0] == 0x5a && shadow[PAGE] == 0 && shadow[2 * PAGE] == 0x5a);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE, 5 * PAGE, true) == KM_OK);
    assert(shadow[0] == 0x5a && shadow[PAGE] == 0 && shadow[2 * PAGE] == 0x5a && shadow[4 * PAGE] == 0);
    query(VIRTUAL_ALIAS_BASE + PAGE, VIRTUAL_ALIAS_BASE, 5 * PAGE, 5);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE, 6 * PAGE, false) == KM_OK);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE, 6 * PAGE, false) == KM_OK);
    assert(kernel_memory_info(7) == initial_used && kernel_memory_info(17) == initial_system);
    // Failed second hole: preserve the middle page and leave both new holes free.
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE + PAGE, PAGE, true) == KM_OK);
    shadow[PAGE] = 0xaa; fail_at = commits + 2;
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE, 3 * PAGE, true) == KM_OUT_OF_MEMORY);
    assert(shadow[PAGE] == 0xaa && kernel_memory_info(7) == initial_used + PAGE);
    query(VIRTUAL_ALIAS_BASE, VIRTUAL_ALIAS_BASE, PAGE, 0);
    query(VIRTUAL_ALIAS_BASE + PAGE, VIRTUAL_ALIAS_BASE + PAGE, PAGE, 5);
    fail_at = 0;
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE, 3 * PAGE, true) == KM_OK);
    assert(kernel_map_physical(VIRTUAL_GUEST_HI - PAGE, PAGE, true) == KM_OK);
    query(VIRTUAL_GUEST_HI - 1, VIRTUAL_GUEST_HI - PAGE, PAGE, 5);
    query(VIRTUAL_GUEST_HI, VIRTUAL_GUEST_HI, 0 - VIRTUAL_GUEST_HI, 0x10);
    uint64_t addr[8], size[8]; assert(kernel_data_regions(addr, size, 8) == 2);
    // A foreign mapping cannot be overwritten or freed by the physical allocator.
    kernel_region_add(VIRTUAL_ALIAS_BASE + 10 * PAGE, PAGE, 6, 3);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE + 9 * PAGE, 3 * PAGE, true) == KM_INVALID_CURRENT_MEMORY);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE + 10 * PAGE, PAGE, false) == KM_INVALID_CURRENT_MEMORY);
    kernel_region_remove(VIRTUAL_ALIAS_BASE + 10 * PAGE);
    // Bounded system resources and reclamation of a completely empty table span.
    reset((uint32_t)initial_system + 2 * PAGE);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE, PAGE, true) == KM_OK);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE + 0x200000, PAGE, true) == KM_OK);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE + 0x400000, PAGE, true) == KM_OUT_OF_RESOURCE);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE, PAGE, false) == KM_OK);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE + 0x400000, PAGE, true) == KM_OK);
    // Heap and physical mappings draw from the same process budget.
    reset(0x1000000);
    assert(kernel_set_heap_size(PAGE)==KM_INVALID_SIZE && kernel_heap_size()==0);
    fail_at=commits+1;assert(kernel_set_heap_size(0x200000)==KM_OUT_OF_MEMORY && kernel_heap_size()==0);
    assert(kernel_memory_info(7)==initial_used);fail_at=0;
    uint64_t heap = (APPLICATION_MEMORY_SIZE - KERNEL_MEMORY_OVERHEAD - 0x1000000) & ~UINT64_C(0x1fffff);
    assert(kernel_set_heap_size(heap) == KM_OK);
    uint64_t remaining = APPLICATION_MEMORY_SIZE - kernel_memory_info(7);
    assert(remaining && kernel_map_physical(VIRTUAL_ALIAS_BASE, remaining, true) == KM_OK);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE + remaining, PAGE, true) == KM_LIMIT_REACHED);
    assert(kernel_set_heap_size(heap - 0x200000) == KM_OK);
    assert(kernel_map_physical(VIRTUAL_ALIAS_BASE + remaining, PAGE, true) == KM_OK);
    assert(kernel_set_heap_size(heap) == KM_LIMIT_REACHED);
    // Independent and overlapping clients share one serialized mapping state.
    reset(0x1000000);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < 4; i++) pthread_join(threads[i], NULL);
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, overlap_worker, NULL));
    for (unsigned i = 0; i < 4; i++) pthread_join(threads[i], NULL);
    assert(kernel_memory_info(7) == initial_used && kernel_memory_info(17) == initial_system);
    // Independent byte/page oracle across bitmap-word boundaries and fragmented runs.
    reset(0x1000000);
    bool model[256] = {0}; unsigned char values[256] = {0}; uint32_t random = 0x781253ab;
    for (unsigned step = 0; step < 5000; step++) {
        random = random * 1664525u + 1013904223u;
        unsigned lo = random >> 24, count = 1 + ((random >> 12) % 17);
        if (count > 256 - lo) count = 256 - lo;
        bool map = (random & 0x100) != 0;
        assert(kernel_map_physical(VIRTUAL_ALIAS_BASE + (uint64_t)lo * PAGE, count * PAGE, map) == KM_OK);
        for (unsigned p = lo; p < lo + count; p++) {
            if (!map || !model[p]) values[p] = 0;
            model[p] = map;
        }
        unsigned pages = 0;
        for (unsigned p = 0; p < 256; p++) { pages += model[p]; assert(shadow[p * PAGE] == values[p]); }
        assert(kernel_memory_info(7) == initial_used + (uint64_t)pages * PAGE);
        unsigned q = (random >> 16) & 255, left = q, right = q + 1;
        while (left && model[left - 1] == model[q]) left--;
        while (right < 256 && model[right] == model[q]) right++;
        uint64_t end = right == 256 && !model[q] ? VIRTUAL_GUEST_HI : VIRTUAL_ALIAS_BASE + (uint64_t)right * PAGE;
        query(VIRTUAL_ALIAS_BASE + (uint64_t)q * PAGE, VIRTUAL_ALIAS_BASE + (uint64_t)left * PAGE,
              end - (VIRTUAL_ALIAS_BASE + (uint64_t)left * PAGE), model[q] ? 5 : 0);
        if (model[q]) shadow[q * PAGE] = values[q] = (unsigned char)(step | 1);
    }
    reset(0);
    assert(kernel_memory_info(16) == 0 && kernel_memory_info(17) == 0 && kernel_heap_size() == 0);
    assert(reservations == 1 && resets >= 4 && kernel_data_regions(addr, size, 8) == 0);
    return 0;
}
