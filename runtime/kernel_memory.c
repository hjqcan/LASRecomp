// Host-backed Horizon memory. Physical mappings are page-granular and idempotent:
// overlapping maps preserve existing bytes; unmaps can cut holes in any run.
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "kernel_memory.h"

#define PAGE_SIZE UINT64_C(0x1000)
#define PAGE_COUNT (VIRTUAL_ALIAS_SIZE / PAGE_SIZE)
#define TABLE_PAGES 512u
#define TABLE_COUNT (PAGE_COUNT / TABLE_PAGES)
#define BITMAP_BYTES (PAGE_COUNT / 8)
// Account for the tracking bitmap/counts plus one 4-KiB table per populated
// 2-MiB span. This is a bounded host model, not a bit-exact Horizon slab allocator.
#define TRACKING_BYTES ((BITMAP_BYTES + TABLE_COUNT * sizeof(uint16_t) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

typedef struct { uint64_t addr, size; uint32_t type, perm; } Region;
static Region regions[1024];
static unsigned nregions;
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t heap_size, physical_size, system_size, populated_tables;
static uint64_t *mapped;
static uint16_t *table_pages;
static bool extra_reserved;

static bool page_mapped(unsigned p) { return (mapped[p / 64] >> (p % 64)) & 1; }
static unsigned next_page(unsigned p, unsigned end, bool value) {
    while (p < end) {
        uint64_t word = (value ? mapped[p / 64] : ~mapped[p / 64]) & (UINT64_MAX << (p % 64));
        if (word) {
            unsigned found = (p & ~63u) + __builtin_ctzll(word);
            return found < end ? found : end;
        }
        p = (p & ~63u) + 64;
    }
    return end;
}
static unsigned run_begin(unsigned p, bool value) {
    while (p) {
        unsigned bit = (p - 1) % 64;
        uint64_t word = (value ? ~mapped[(p - 1) / 64] : mapped[(p - 1) / 64]);
        word &= UINT64_MAX >> (63 - bit);
        if (word) return ((p - 1) & ~63u) + 64 - __builtin_clzll(word);
        p = (p - 1) & ~63u;
    }
    return 0;
}
static void set_pages(unsigned lo, unsigned hi, bool value) {
    for (unsigned p = lo; p < hi; p++) {
        if (page_mapped(p) == value) continue;
        uint64_t mask = UINT64_C(1) << (p % 64);
        uint16_t *count = &table_pages[p / TABLE_PAGES];
        if (value) {
            mapped[p / 64] |= mask;
            if (!(*count)++) populated_tables++;
            physical_size += PAGE_SIZE;
        } else {
            mapped[p / 64] &= ~mask;
            if (!--(*count)) populated_tables--;
            physical_size -= PAGE_SIZE;
        }
    }
}
static void discard_pages(uint64_t addr, uint64_t size) {
    if (!plat_discard_physical(addr, size)) FATAL("cannot discard physical mapping %llx+%llx\n",
        (unsigned long long)addr, (unsigned long long)size);
}
static uint64_t system_used(void) { return system_size ? TRACKING_BYTES + populated_tables * PAGE_SIZE : 0; }
static uint64_t memory_used(void) { return heap_size + physical_size + KERNEL_MEMORY_OVERHEAD + system_size; }

bool kernel_memory_configure(uint32_t budget) {
    // Configuration and reset run with guest threads stopped.
    if (nregions || heap_size || physical_size) return false;
    if (budget && (budget % PAGE_SIZE || budget < TRACKING_BYTES || budget > 0x20000000u)) return false;
    if (budget && !mapped) {
        mapped = calloc(1, BITMAP_BYTES);
        table_pages = calloc(TABLE_COUNT, sizeof *table_pages);
        if (!mapped || !table_pages) {
            free(mapped); free(table_pages); mapped = NULL; table_pages = NULL;
            return false;
        }
    }
    if (budget && !extra_reserved) {
        if (!plat_reserve_guest_extra(GUEST_HI, VIRTUAL_GUEST_HI)) return false;
        extra_reserved = true;
    }
    system_size = budget;
    if (getenv("LARECOMP_LOG_MEMORY"))
        LOG("[memory] alias=%llx+%llx system_resource=%x\n",
            (unsigned long long)(budget ? VIRTUAL_ALIAS_BASE : ALIAS_BASE),
            (unsigned long long)(budget ? VIRTUAL_ALIAS_SIZE : ALIAS_SIZE), budget);
    return true;
}

void kernel_memory_reset(void) {
    pthread_mutex_lock(&memory_lock);
    if (extra_reserved && !plat_reset_guest(GUEST_HI, VIRTUAL_GUEST_HI))
        FATAL("cannot reset extended guest memory\n");
    if (mapped) memset(mapped, 0, BITMAP_BYTES);
    if (table_pages) memset(table_pages, 0, TABLE_COUNT * sizeof *table_pages);
    nregions = 0; heap_size = 0; physical_size = 0; system_size = 0; populated_tables = 0;
    pthread_mutex_unlock(&memory_lock);
}

uint64_t kernel_memory_info(unsigned id) {
    pthread_mutex_lock(&memory_lock);
    uint64_t value = 0;
    switch (id) {
    case 2: value = system_size ? VIRTUAL_ALIAS_BASE : ALIAS_BASE; break;
    case 3: value = system_size ? VIRTUAL_ALIAS_SIZE : ALIAS_SIZE; break;
    case 4: value = HEAP_BASE; break;
    case 5: value = HEAP_MAX; break;
    case 6: case 21: value = APPLICATION_MEMORY_SIZE; break;
    case 7: case 22: value = memory_used(); break;
    case 12: value = GUEST_LO; break;
    case 13: value = (system_size ? VIRTUAL_GUEST_HI : GUEST_HI) - GUEST_LO; break;
    case 16: value = system_size; break;
    case 17: value = system_used(); break;
    }
    pthread_mutex_unlock(&memory_lock);
    return value;
}
uint64_t kernel_heap_size(void) {
    pthread_mutex_lock(&memory_lock);
    uint64_t value = heap_size;
    pthread_mutex_unlock(&memory_lock);
    return value;
}

static void add_region(uint64_t addr, uint64_t size, uint32_t type, uint32_t perm) {
    if (!size) return;
    if (nregions == sizeof regions / sizeof *regions) FATAL("too many regions\n");
    unsigned i = nregions++;
    while (i && regions[i - 1].addr > addr) { regions[i] = regions[i - 1]; i--; }
    regions[i] = (Region){addr, size, type, perm};
}
static void remove_region(uint64_t addr) {
    for (unsigned i = 0; i < nregions; i++) if (regions[i].addr == addr) {
        memmove(regions + i, regions + i + 1, (--nregions - i) * sizeof *regions);
        return;
    }
}
void kernel_region_add(uint64_t addr, uint64_t size, uint32_t type, uint32_t perm) {
    pthread_mutex_lock(&memory_lock);
    plat_commit(addr, size);
    add_region(addr, size, type, perm);
    pthread_mutex_unlock(&memory_lock);
}
void kernel_region_remove(uint64_t addr) {
    pthread_mutex_lock(&memory_lock);
    remove_region(addr);
    pthread_mutex_unlock(&memory_lock);
}

uint32_t kernel_set_heap_size(uint64_t size) {
    pthread_mutex_lock(&memory_lock);
    uint32_t result = KM_OK;
    if (size > HEAP_MAX) result = KM_OUT_OF_MEMORY;
    else if (system_size && size % 0x200000) result = KM_INVALID_SIZE;
    else if (system_size && size + physical_size + KERNEL_MEMORY_OVERHEAD + system_size > APPLICATION_MEMORY_SIZE)
        result = KM_LIMIT_REACHED;
    else {
        if (system_size && size > heap_size) {
            if (!plat_commit_physical(HEAP_BASE + heap_size, size - heap_size)) {
                discard_pages(HEAP_BASE + heap_size, size - heap_size);
                result = KM_OUT_OF_MEMORY;
            }
        } else if (system_size && size < heap_size) discard_pages(HEAP_BASE + size, heap_size - size);
        else if (!system_size) plat_commit(HEAP_BASE, size);
        if (result == KM_OK) {
            remove_region(HEAP_BASE);
            add_region(HEAP_BASE, size, 5, 3);
            heap_size = size;
        }
    }
    pthread_mutex_unlock(&memory_lock);
    return result;
}

uint32_t kernel_map_physical(uint64_t addr, uint64_t size, bool map) {
    if (addr % PAGE_SIZE) return KM_INVALID_ADDRESS;
    if (!size || size % PAGE_SIZE) return KM_INVALID_SIZE;
    if (size > UINT64_MAX - addr) return KM_INVALID_REGION;
    pthread_mutex_lock(&memory_lock);
    uint32_t result = KM_OK;
    if (!system_size) { result = KM_INVALID_STATE; goto done; }
    if (addr < VIRTUAL_ALIAS_BASE || addr >= VIRTUAL_GUEST_HI || size > VIRTUAL_GUEST_HI - addr) {
        result = KM_INVALID_REGION; goto done;
    }
    for (unsigned i = 0; i < nregions; i++)
        if (addr < regions[i].addr + regions[i].size && regions[i].addr < addr + size) {
            result = KM_INVALID_CURRENT_MEMORY; goto done;
        }
    unsigned lo = (addr - VIRTUAL_ALIAS_BASE) / PAGE_SIZE, hi = lo + size / PAGE_SIZE;
    if (map) {
        uint64_t added = 0, tables = 0;
        for (unsigned p = lo; p < hi; ) {
            unsigned end = next_page(p + 1, hi, !page_mapped(p));
            if (!page_mapped(p)) added += (uint64_t)(end - p) * PAGE_SIZE;
            p = end;
        }
        for (unsigned t = lo / TABLE_PAGES; t <= (hi - 1) / TABLE_PAGES; t++)
            if (!table_pages[t]) tables++;
        if (memory_used() + added > APPLICATION_MEMORY_SIZE) { result = KM_LIMIT_REACHED; goto done; }
        if (system_used() + tables * PAGE_SIZE > system_size) { result = KM_OUT_OF_RESOURCE; goto done; }
        // Leave metadata untouched until every new span is backed. On failure,
        // roll back only previously free spans; existing mappings keep their data.
        for (unsigned p = lo; p < hi; ) {
            bool present = page_mapped(p);
            unsigned end = next_page(p + 1, hi, !present);
            if (!present && !plat_commit_physical(VIRTUAL_ALIAS_BASE + (uint64_t)p * PAGE_SIZE,
                                                  (uint64_t)(end - p) * PAGE_SIZE)) {
                for (unsigned q = lo; q < end; ) {
                    bool old = page_mapped(q);
                    unsigned qe = next_page(q + 1, end, !old);
                    if (!old) discard_pages(VIRTUAL_ALIAS_BASE + (uint64_t)q * PAGE_SIZE,
                                                    (uint64_t)(qe - q) * PAGE_SIZE);
                    q = qe;
                }
                result = KM_OUT_OF_MEMORY; goto done;
            }
            p = end;
        }
    } else {
        for (unsigned p = lo; p < hi; ) {
            bool present = page_mapped(p);
            unsigned end = next_page(p + 1, hi, !present);
            if (present) discard_pages(VIRTUAL_ALIAS_BASE + (uint64_t)p * PAGE_SIZE,
                                               (uint64_t)(end - p) * PAGE_SIZE);
            p = end;
        }
    }
    set_pages(lo, hi, map);
done:
    if (getenv("LARECOMP_LOG_MEMORY"))
        LOG("[memory] %s %llx+%llx -> %x physical=%llx heap=%llx system_used=%llx\n",
            map ? "map" : "unmap", (unsigned long long)addr, (unsigned long long)size, result,
            (unsigned long long)physical_size, (unsigned long long)heap_size, (unsigned long long)system_used());
    pthread_mutex_unlock(&memory_lock);
    return result;
}

void kernel_query_memory(uint64_t addr, uint64_t out) {
    uint64_t info[5] = {0};
    pthread_mutex_lock(&memory_lock);
    uint64_t limit = system_size ? VIRTUAL_GUEST_HI : GUEST_HI;
    uint64_t left = 0, right = limit;
    if (addr >= limit) { info[0] = limit; info[1] = 0 - limit; info[2] = 0x10; goto done; }
    for (unsigned i = 0; i < nregions; i++) {
        Region *r = &regions[i];
        if (addr >= r->addr && addr < r->addr + r->size) {
            info[0] = r->addr; info[1] = r->size; info[2] = r->type; info[3] = r->perm; goto done;
        }
        if (addr < r->addr) { right = r->addr; break; }
        left = r->addr + r->size;
    }
    if (system_size && addr >= VIRTUAL_ALIAS_BASE) {
        unsigned p = (addr - VIRTUAL_ALIAS_BASE) / PAGE_SIZE;
        bool present = page_mapped(p);
        uint64_t begin = VIRTUAL_ALIAS_BASE + (uint64_t)run_begin(p, present) * PAGE_SIZE;
        uint64_t end = VIRTUAL_ALIAS_BASE + (uint64_t)next_page(p + 1, PAGE_COUNT, !present) * PAGE_SIZE;
        if (begin > left) left = begin;
        if (end < right) right = end;
        if (present) { info[2] = 5; info[3] = 3; }
    } else if (system_size && right > VIRTUAL_ALIAS_BASE) right = VIRTUAL_ALIAS_BASE;
    info[0] = left; info[1] = right - left;
done:
    pthread_mutex_unlock(&memory_lock);
    memcpy((void *)(uintptr_t)out, info, sizeof info);
}

int kernel_data_regions(uint64_t *addr, uint64_t *size, int max) {
    pthread_mutex_lock(&memory_lock);
    int n = 0;
    for (unsigned i = 0; i < nregions && n < max; i++)
        if ((regions[i].perm & 2) && regions[i].type != 0xB && regions[i].type != 0xC) {
            addr[n] = regions[i].addr; size[n++] = regions[i].size;
        }
    if (system_size) for (unsigned p = next_page(0, PAGE_COUNT, true); p < PAGE_COUNT && n < max; ) {
        unsigned end = next_page(p + 1, PAGE_COUNT, false);
        addr[n] = VIRTUAL_ALIAS_BASE + (uint64_t)p * PAGE_SIZE; size[n++] = (uint64_t)(end - p) * PAGE_SIZE;
        p = next_page(end, PAGE_COUNT, true);
    }
    pthread_mutex_unlock(&memory_lock);
    return n;
}
