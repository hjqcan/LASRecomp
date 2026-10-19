// Real IPC handlers, with deterministic process-local test zones. No host clock changes.
#include <assert.h>
#include "../runtime/services/time.c"

Service *service_new(const char *n, ServiceHandler h, void *s) { abort(); }
void ipc_return_object(IpcResp *r, Service *s) { abort(); }
KSharedMem *kernel_create_shared_memory(uint64_t n, void (*f)(KSharedMem *), uint32_t *h) { abort(); }
uint32_t kernel_current_thread_handle(void) { return 1; }
static uint64_t test_ticks = 19200000ull * 12345 + 9600000;
uint64_t rt_ticks(void) { return test_ticks; }
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

// SDK consumers select the sequence's active slot, then match its clock UUID.
// Decode the ABI bytes independently of the service's writer.
static uint64_t read64(const uint8_t *p) { uint64_t v; memcpy(&v,p,8); return v; }
static const uint8_t *slot(const uint8_t *m, unsigned offset, unsigned size) {
    uint32_t sequence; memcpy(&sequence,m+offset,4);
    return m+offset+8+(sequence&1)*size;
}
static int read_clock(const uint8_t *m, unsigned offset, int modern, int64_t *out) {
    const uint8_t *context=slot(m,offset,0x20);
    int64_t ns=(int64_t)((__uint128_t)rt_ticks()*10000/192);
    if (modern) {
        const uint8_t *point=slot(m,0xd0,0x38);
        if (memcmp(context+16,point+40,16)) return 0xcc74;
        int64_t multiplier=(int64_t)read64(point+8);
        int64_t adjustment=((ns-(int64_t)read64(point))*multiplier >> read64(point+16))
                           +(int64_t)read64(point+24);
        int64_t limit=(int64_t)read64(point+32);
        if (multiplier<0 ? adjustment<limit : adjustment>limit) adjustment=limit;
        ns+=adjustment;
    } else {
        const uint8_t *steady=slot(m,0,0x18);
        if (memcmp(context+16,steady+8,16)) return 0xcc74;
        ns+=(int64_t)read64(steady);
    }
    *out=(int64_t)read64(context)+ns/1000000000;
    return 0;
}
static void shared_clock(void) {
    _Alignas(8) uint8_t memory[0x1000]={0};
    KSharedMem shared={.guest_addr=(uintptr_t)memory};
    int64_t before=(int64_t)time(NULL), modern, legacy;
    time_shmem_map(&shared);
    for (unsigned offset=0x38;offset<=0x80;offset+=0x48) {
        assert(!read_clock(memory,offset,0,&legacy));
        assert(!read_clock(memory,offset,1,&modern));
        assert(modern==legacy && modern>=before && modern<=(int64_t)time(NULL));
        test_ticks+=19200000ull*90;
        int64_t advanced;
        assert(!read_clock(memory,offset,1,&advanced) && advanced==modern+90);
        test_ticks-=19200000ull*90;
        // Both slots contain a valid no-adjustment point on initial publication.
        memory[0xd0]=1;
        assert(!read_clock(memory,offset,1,&advanced) && advanced==modern);
        memory[0xd0]=0;
        memory[0xd8+40]^=1;
        advanced=123;
        assert(read_clock(memory,offset,1,&advanced)==0xcc74 && advanced==123);
        memory[0xd8+40]^=1;
    }
}
static IpcResp snapshot_call(IpcReq *q) {
    IpcResp r={0}; static_service(NULL,q,&r); return r;
}
static void snapshot_tests(void) {
    _Alignas(8) uint8_t first[0xD8], second[0xD8], input[0x48]={1};
    memset(first,0xa5,sizeof first); memset(second,0xa5,sizeof second);
    test_clock_enabled=true; test_clock_offset=1709222399-steady_seconds();
    IpcReq q={.cmd=400,.data=input,.data_size=1,.nc=1,.c={{(uintptr_t)first,0xD0}}};
    IpcResp r=snapshot_call(&q); assert(!r.result && !r.data_size);
    assert(read64(first+0x40)==1709222399 && read64(first+0x48)==1709222399);
    assert(first[0x52]==2 && first[0x53]==29 && first[0x54]==23 && first[0x55]==59 && first[0x56]==59);
    assert(!memcmp(first+0x10,first+0x98,16) && !strcmp((char *)first+0xa8,local_location));
    assert(!first[0xcc] && first[0xcd]==1 && !first[0xce] && !first[0xcf]);
    assert(read64(first)+read64(first+0x90)==read64(first+0x40));
    for(unsigned i=0xd0;i<sizeof first;i++) assert(first[i]==0xa5);
    test_ticks+=19200000ull*90; q.c[0].addr=(uintptr_t)second;
    r=snapshot_call(&q); assert(!r.result && read64(second+0x40)==1709222489);
    assert(second[0x52]==3 && second[0x53]==1 && second[0x54]==0);
    q=(IpcReq){.cmd=501,.nx=2,.x={{(uintptr_t)first,0xd0},{(uintptr_t)second,0xd0}}};
    r=snapshot_call(&q); assert(!r.result && r.data_size==8 && read64(r.data)==90000000000ull);
    q.cmd=500; r=snapshot_call(&q); assert(!r.result && !read64(r.data));
    int64_t shifted=(int64_t)read64(second)+3600; memcpy(second,&shifted,8);
    r=snapshot_call(&q); assert(!r.result && read64(r.data)==3600000000000ull);
    first[0xcc]=second[0xcc]=1; r=snapshot_call(&q); assert(!r.result && !read64(r.data));
    first[0xcc]=second[0xcc]=0; second[0x98]^=1; q.cmd=501;
    r=snapshot_call(&q); assert(!r.result && read64(r.data)==90000000000ull); // network fallback
    memset(second+0x48,0,8); r=snapshot_call(&q); assert(r.result==MAKE_RESULT(116,200));
    second[0x98]^=1;
    int64_t huge=INT64_MAX; memcpy(second+0x90,&huge,8);
    r=snapshot_call(&q); assert(r.result==MAKE_RESULT(116,201));
    q.x[1].size=0xcf; assert(snapshot_call(&q).result);
    // Reusing supplied contexts must retain their timestamps and validate UUIDs.
    memcpy(input+8,first,0x40);
    q=(IpcReq){.cmd=401,.data=input,.data_size=sizeof input,.nc=1,.c={{(uintptr_t)second,0xd0}}};
    r=snapshot_call(&q); assert(!r.result && read64(second+0x40)==1709222489);
    input[8+0x10]^=1; assert(snapshot_call(&q).result==MAKE_RESULT(116,102));
    q.data_size=1; assert(snapshot_call(&q).result);
    q.cmd=400; q.c[0].size=0xcf; memset(second,0x5a,sizeof second);
    assert(snapshot_call(&q).result); for(unsigned i=0;i<sizeof second;i++) assert(second[i]==0x5a);
    test_ticks-=19200000ull*90; test_clock_enabled=false;
}
int main(void) {
    shared_clock();
    zone("CST-8");
    snapshot_tests();
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
    puts("PASS: legacy/modern shared clocks, typed snapshots and differences, clock UUIDs, advancing time, timezone IPC rules, local/UTC parity, leap-day rollover, 64-bit dates, DST gaps/folds and buffer bounds");
}
