#include <assert.h>
#include "../runtime/kernel.c"

// These unrelated kernel dependencies must never be reached by the affinity fixture.
Module g_modules[4]; int g_nmodules;
void rt_fatal_msg(const char *fmt, ...) { abort(); }
void rt_fatal_trace(void) { abort(); }
uint64_t plat_now_ns(void) { abort(); }
void plat_commit(uint64_t a,uint64_t n) { abort(); }
const char *plat_symbol(uint64_t a,uint64_t *off) { abort(); }
void crash_handler_install(void) { abort(); }
void guest_call(Cpu *c,uint64_t a) { abort(); }
int ipc_handle_request(Cpu *c,uint32_t h) { abort(); }
uint32_t ipc_connect_named_port(const char *s) { abort(); }
uint32_t vi_vsync_handle(void) { abort(); }
void plat_print_backtrace(void) { abort(); }

int main(void) {
    KThread threads[3] = {{.core=0,.preferred_core=0,.core_mask=1},
                          {.core=0,.preferred_core=0,.core_mask=1},
                          {.core=0,.preferred_core=0,.core_mask=1}};
    unsigned written = 0;
    for (int i = 0; i < 3; i++) {
        assert(!thread_set_core_mask(&threads[i],i,1ull<<i));
        assert(threads[i].preferred_core==i && threads[i].core_mask==(1ull<<i));
        written |= 1u << threads[i].core;
    }
    assert(written == 7); // a three-worker barrier must have three distinct progress slots
    assert(thread_set_core_mask(&threads[1],2,2)==R_INVALID_ENUM && threads[1].core==1);
    assert(thread_set_core_mask(&threads[1],-1,0)==R_INVALID_ENUM);
    assert(!thread_set_core_mask(&threads[1],-1,4) && threads[1].core==2);
    assert(!thread_set_core_mask(&threads[1],-3,1) && threads[1].preferred_core==-1 && threads[1].core==0);
    assert(!thread_set_core_mask(&threads[1],-2,15) && threads[1].preferred_core==0 && threads[1].core_mask==1);
    puts("PASS: distinct worker cores, affinity queries, automatic selection, default and invalid masks");
}
