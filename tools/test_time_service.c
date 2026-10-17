// Real IPC handlers, with deterministic process-local test zones. No host clock changes.
#include <assert.h>
#include "../runtime/services/time.c"

Service *service_new(const char *n, ServiceHandler h, void *s) { abort(); }
void ipc_return_object(IpcResp *r, Service *s) { abort(); }
KSharedMem *kernel_create_shared_memory(uint64_t n, void (*f)(KSharedMem *), uint32_t *h) { abort(); }
uint32_t kernel_current_thread_handle(void) { return 1; }
uint64_t rt_ticks(void) { return 0; }
void rt_fatal_msg(const char *fmt, ...) { (void)fmt; abort(); }
void rt_fatal_trace(void) { abort(); }
void research_external(int kind, uint64_t pc, uint32_t thread, uint64_t index, uint64_t value) {
    (void)kind; (void)pc; (void)thread; (void)index; (void)value;
}

static void zone(const char *name) {
#ifdef _WIN32
    _putenv_s("TZ", name); _tzset();
#else
    setenv("TZ", name, 1); tzset();
#endif
}
static uint8_t local_rule[TIME_RULE_SIZE + 16], utc_rule[TIME_RULE_SIZE];
static IpcResp call(IpcReq *req) {
    IpcResp r={0}; timezone_service(NULL, req, &r); return r;
}
static void load_rule(const char *name, uint8_t *rule) {
    char location[0x24]={0}; snprintf(location, sizeof location, "%s", name);
    IpcReq q={.cmd=4,.data=(uint8_t *)location,.data_size=sizeof location,
              .nb=1,.b={{(uintptr_t)rule,TIME_RULE_SIZE}}};
    assert(!call(&q).result);
}
static IpcResp calendar(int64_t t, uint8_t *rule) {
    IpcReq q={.cmd=rule?100:101,.data=(uint8_t *)&t,.data_size=8,
              .na=rule?1:0,.a={{(uintptr_t)rule,TIME_RULE_SIZE}}};
    return call(&q);
}
static IpcResp posix(const uint8_t *cal, uint8_t *rule, int64_t *out, size_t n) {
    IpcReq q={.cmd=rule?201:202,.data=cal,.data_size=8,
              .na=rule?1:0,.a={{(uintptr_t)rule,TIME_RULE_SIZE}},
              .nc=1,.c={{(uintptr_t)out,n}}};
    return call(&q);
}
int main(void) {
    zone("CST-8");
    memset(local_rule,0xcc,sizeof local_rule);
    IpcReq q={.cmd=0}; IpcResp r=call(&q);
    assert(r.data_size==0x24 && !strcmp((char *)r.data,local_location));
    load_rule((char *)r.data,local_rule); load_rule("UTC",utc_rule);
    for(size_t i=TIME_RULE_SIZE;i<sizeof local_rule;i++) assert(local_rule[i]==0xcc);
    // 2024 leap day, across midnight in UTC+8, and far-future 64-bit timestamps.
    const int64_t samples[]={1709222399,1709222400,1792195200,2851142400};
    for(size_t i=0;i<sizeof samples/sizeof *samples;i++) {
        IpcResp a=calendar(samples[i],local_rule), b=calendar(samples[i],NULL);
        assert(!a.result && !b.result && a.data_size==0x20 && !memcmp(a.data,b.data,0x20));
        assert(*(int32_t *)(a.data+28)==8*3600);
        int64_t out[3]={0,0,0x12345678}; r=posix(a.data,local_rule,out,16);
        assert(!r.result && *(uint32_t *)r.data==1 && out[0]==samples[i] && out[2]==0x12345678);
        r=posix(a.data,NULL,out,16); assert(!r.result && out[0]==samples[i]);
        a=calendar(samples[i],utc_rule); assert(!a.result && !*(int32_t *)(a.data+28));
        r=posix(a.data,utc_rule,out,16); assert(!r.result && out[0]==samples[i]);
    }
    IpcResp before=calendar(samples[0],local_rule), after=calendar(samples[1],local_rule);
    assert(before.data[3]==29 && before.data[4]==23 && after.data[3]==1 && after.data[4]==0);
    uint8_t bad[8]={0xe8,0x07,2,30,12,0,0,0}; int64_t out[3]={99,88,77};
    r=posix(bad,local_rule,out,16); assert(!r.result && !*(uint32_t *)r.data && out[0]==99);
    r=posix(after.data,local_rule,out,7); assert(r.result && out[0]==99);
    local_rule[0]^=1; assert(calendar(samples[0],local_rule).result); local_rule[0]^=1;
    // Windows CRT's EST5EDT and POSIX's explicit rule describe these same US boundaries.
#ifdef _WIN32
    zone("EST5EDT");
#else
    zone("EST5EDT,M3.2.0/2,M11.1.0/2");
#endif
    uint8_t spring[8]={0xe8,0x07,3,10,2,30,0,0};
    uint8_t autumn[8]={0xe8,0x07,11,3,1,30,0,0};
    r=posix(spring,local_rule,out,16); assert(!r.result && !*(uint32_t *)r.data);
    r=posix(autumn,local_rule,out,16); assert(!r.result && *(uint32_t *)r.data==2 && out[1]-out[0]==3600 && out[2]==77);
    puts("PASS: timezone IPC rules, local/UTC parity, leap-day rollover, 64-bit dates, DST gaps/folds and buffer bounds");
}
