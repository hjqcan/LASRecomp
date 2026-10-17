// Guest memory map and optional demand-backed alias memory. No title-specific addresses.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Kept separate from the original reservation: existing titles retain their layout.
#define VIRTUAL_ALIAS_BASE UINT64_C(0x7400000000)
#define VIRTUAL_ALIAS_SIZE UINT64_C(0x0400000000)
#define VIRTUAL_GUEST_HI   (VIRTUAL_ALIAS_BASE + VIRTUAL_ALIAS_SIZE)
#define APPLICATION_MEMORY_SIZE UINT64_C(0xCD500000)
#define KERNEL_MEMORY_OVERHEAD UINT64_C(0x04000000)

enum {
    KM_OK = 0, KM_INVALID_SIZE = 0xCA01, KM_INVALID_ADDRESS = 0xCC01,
    KM_OUT_OF_RESOURCE = 0xCE01, KM_OUT_OF_MEMORY = 0xD001,
    KM_INVALID_CURRENT_MEMORY = 0xD401, KM_INVALID_REGION = 0xDC01,
    KM_INVALID_STATE = 0xFA01, KM_LIMIT_REACHED = 0x10801,
};

// Called before guest threads start. A zero budget selects the legacy layout.
bool kernel_memory_configure(uint32_t system_resource_size);
// Threads stopped: clear bookkeeping and release the extra reservation's backing.
void kernel_memory_reset(void);
uint64_t kernel_memory_info(unsigned id);
uint64_t kernel_heap_size(void);
uint32_t kernel_set_heap_size(uint64_t size);
uint32_t kernel_map_physical(uint64_t addr, uint64_t size, bool map);
void kernel_region_add(uint64_t addr, uint64_t size, uint32_t type, uint32_t perm);
void kernel_region_remove(uint64_t addr);
void kernel_query_memory(uint64_t addr, uint64_t out);
int kernel_data_regions(uint64_t *addr, uint64_t *size, int max);
