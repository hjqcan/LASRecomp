// Actual recorder and render queue: a queued parent retains its nested commands
// after both handles are recycled; fences publish before execution, waits time out.
#define main storage_checks
#include "test_command_storage.c"
#undef main
#include <stdatomic.h>
#include "../runtime/nvn/rqueue.c"

volatile int g_rt_unwind;
__thread uint64_t g_blocked_ns;
static _Atomic unsigned entered, allow, completed;
static uint64_t published_guest, published_item;
uint64_t plat_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
uint64_t plat_thread_cpu_ns(void) { return plat_now_ns(); }
void plat_thread_guest(int priority) { (void)priority; }
#ifdef _WIN32
void plat_thread_perf_core(int role) { (void)role; }
#endif
uint64_t kernel_current_thread_id(void) { return 1; }
void kernel_hang_hooks(bool (*check)(uint64_t,char *,size_t),void (*dump)(const char *)) { (void)check; (void)dump; }
void nvn_sync_set_seq(uint64_t guest,uint64_t seq) { published_guest=guest; published_item=seq; }
void *backend_pool_push(void) { return NULL; }
void backend_pool_pop(void *p) { (void)p; }
uint64_t renderer_present_count(void) { return 0; }
uint64_t renderer_submit_seq(void) { return atomic_load(&completed)*10; }
uint64_t renderer_issued_seq(void) { return renderer_submit_seq(); }
void backend_present_now(NvnTexture *t,uint64_t f) { (void)t; (void)f; }
void backend_upload_now(NvnTexture *t,int k,void *p,uint32_t w,uint32_t h,uint32_t s,uint32_t o) { abort(); }
void nvn_texture_destroy(NvnTexture *t) { (void)t; abort(); }
void renderer_flush(void) {}
static unsigned inspect(uint64_t address) {
    unsigned fences=0;
    for (;;) {
        const CmdRec *r=(const void *)(uintptr_t)address;
        if(r->op==CMD_END) return fences;
        if(r->op==CMD_CALL) for(unsigned i=0;i<r->x[0];i++) fences+=inspect(((const uint64_t *)r->extra)[i]);
        if(r->op==CMD_FENCE_SYNC) { assert(r->x[0]==1234); fences++; }
        address+=r->size;
    }
}
void backend_execute(uint64_t records) {
    unsigned index=atomic_fetch_add(&entered,1)+1;
    while(atomic_load(&allow)<index) usleep(1000);
    assert(inspect(records)==1);
    atomic_fetch_add(&completed,1);
}
typedef struct { uint64_t q,seq; _Atomic bool done; int result; } Wait;
static void *waiter(void *arg) {
    Wait *w=arg; w->result=rq_wait_item_timeout(w->q,1000000000,&w->seq); atomic_store(&w->done,true); return NULL;
}
int main(void) {
    uint8_t memory[1024],control[256],parent_control[256];
    NvnPool pool={.storage=(uint64_t)(uintptr_t)memory,.size=sizeof memory};
    pool_guest=(uint64_t)(uintptr_t)&pool;
    Cpu c={0}; uint64_t guest=0;
    setup(&c,&guest,control,0,512);
    c.x[1]=1234; hle_nvnCommandBufferFenceSync(&c);
    hle_nvnCommandBufferEndRecording(&c); uint64_t child=c.x[0];
    c.x[0]=(uintptr_t)&guest; hle_nvnCommandBufferFinalize(&c);
    setup(&c,&guest,parent_control,0,512);
    c.x[1]=1; c.x[2]=(uintptr_t)&child; hle_nvnCommandBufferCallCommands(&c);
    hle_nvnCommandBufferEndRecording(&c); uint64_t parent=c.x[0];
    c.x[0]=(uintptr_t)&guest; hle_nvnCommandBufferFinalize(&c);
    nvn_command_handle_destroy(child);
    assert(rq_on());
    uint64_t first=rq_submit(parent),second=rq_submit(parent),seq=0;
    assert(published_guest==1234 && published_item==second);
    nvn_command_handle_destroy(parent);
    assert(live_lists()==0);
    assert(rq_wait_item_timeout(first,0,&seq)==2);
    g_rt_unwind=1; assert(rq_wait_item_timeout(first,1000000000,&seq)==3); g_rt_unwind=0;
    Wait a={.q=first},b={.q=second}; pthread_t ta,tb;
    pthread_create(&ta,NULL,waiter,&a); pthread_create(&tb,NULL,waiter,&b);
    atomic_store(&allow,1); pthread_join(ta,NULL);
    assert(a.result<2 && a.seq==10 && !atomic_load(&b.done));
    assert(rq_wait_item_timeout(second,1000000,&seq)==2);
    atomic_store(&allow,2); pthread_join(tb,NULL);
    assert(b.result<2 && b.seq==20);
    rq_drain(); assert(rq_wait_item_timeout(second,0,&seq)==0 && seq==20);
    puts("PASS: nested command ownership, recycled handles, queued fences, independent waiters, polling, timeout and unwind");
    return 0;
}
