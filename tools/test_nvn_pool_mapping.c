#include <assert.h>
#include "../runtime/hle.h"
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#include "../runtime/nvn/objects.c"
void rt_fatal_msg(const char *fmt, ...) { (void)fmt; abort(); }
void rt_fatal_trace(void) { abort(); }
uint64_t rq_current_item(void) { return 0; }
void backend_texture_destroyed(NvnTexture *t) { (void)t; abort(); }

int main(void) {
    uint8_t storage[8192]={0};
    NvnPool physical={.storage=(uintptr_t)storage,.size=sizeof storage,.flags=0x125};
    NvnPool virtual_pool={.size=4096,.flags=0x2a1};
    uint64_t src=(uintptr_t)&physical, dst=(uintptr_t)&virtual_pool;
    struct { uint64_t pool, physical_offset, virtual_offset, size; uint32_t storage_class, pad; }
        request={(uintptr_t)&src,4096,0,4096,0,0};
    Cpu c={0};
#define MAP() do { c.x[0]=(uintptr_t)&dst; c.x[1]=1; c.x[2]=(uintptr_t)&request; hle_nvnMemoryPoolMapVirtual(&c); } while(0)
    MAP(); assert(c.x[0]==1 && virtual_pool.storage==(uintptr_t)(storage+4096));
    storage[4101]=17; assert(*(uint8_t *)(uintptr_t)(virtual_pool.storage+5)==17);
    *(uint8_t *)(uintptr_t)(virtual_pool.storage+6)=23; assert(storage[4102]==23);
    MAP(); assert(c.x[0]==1);
    request.physical_offset=0; MAP(); assert(!c.x[0]); // no stale existing texture addresses
    request.physical_offset=4097; MAP(); assert(!c.x[0]);
    request.physical_offset=UINT64_MAX; MAP(); assert(!c.x[0]);
    request.physical_offset=4096; request.virtual_offset=1; MAP(); assert(!c.x[0]);
    request.virtual_offset=0; request.size=4095; MAP(); assert(!c.x[0]);
    request.size=4096; request.pool=0; MAP(); assert(!c.x[0]);
    assert(virtual_pool.storage==(uintptr_t)(storage+4096));
    puts("PASS: full virtual-to-physical pool alias, shared storage, repeat map, bounds and unsupported mappings");
}
