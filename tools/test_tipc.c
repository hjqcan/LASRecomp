// Synthetic HIPC packets exercise the production TIPC and CMIF codec together.
#include <assert.h>
#include "../runtime/ipc.c"

static KObject *handles[32];
static unsigned next_handle=1, calls, closed;
KObject *handle_get(uint32_t h) { return h<32 ? handles[h] : NULL; }
uint32_t kernel_new_handle(KObject *o) { assert(next_handle<32);handles[next_handle]=o;return next_handle++; }
void rt_fatal_msg(const char *s, ...) { (void)s;abort(); }
void rt_fatal_trace(void) { abort(); }
static void close_service(Service *s) { (void)s;closed++; }
static Service *child;
static void echo(Service *s,IpcReq *req,IpcResp *resp) {
    (void)s;calls++;
    if (req->cmd==3) {
        assert(req->pid==UINT64_C(0x1122334455667788) && req->data_size==8);
        assert(IN(uint32_t,0)==0xaabbccdd && IN(uint32_t,4)==0x12345678);
        resp->copy_handles[resp->ncopy++]=0x77;
        resp->move_handles[resp->nmove++]=0x88;
        ipc_return_object(resp,child);
        resp->result=0;OUT(uint32_t,0)=0x87654321;resp_size(resp,4);
    } else if (req->cmd==7) {
        assert(req->data_size==4 && IN(uint32_t,0)==0x11223344);
        OUT(uint32_t,0)=0x55667788;resp_size(resp,4);
    } else if (req->cmd==9) {
        assert(req->na==1 && req->nb==1 && req->nw==1 && req->ncopy==1 && req->copy_handles[0]==0x99);
        assert(req->a[0].addr==UINT64_C(0x7012345678) && req->a[0].size==0x101);
        assert(req->b[0].addr==UINT64_C(0x7112345678) && req->b[0].size==0x202);
        assert(req->w[0].addr==UINT64_C(0x7212345678) && req->w[0].size==0x303);
        assert(in_buf(req,1)==&req->w[0] && out_buf(req,1)==&req->w[0]);
        assert(req->data_size==4 && IN(uint32_t,0)==0xdeadbeef);
    } else if(req->cmd==10) {
        resp_size(resp,sizeof resp->data); // oversized TIPC response must fail safely
    } else if(req->cmd==11) {
        assert(req->domain_obj==1);
        ipc_return_object(resp,child);
        OUT(uint32_t,0)=0xabcdef01;resp_size(resp,4);
    } else if(req->cmd==12) {
        resp->nmove=8;ipc_return_object(resp,child); // too many total move handles
    } else abort();
}
static void descriptor(uint32_t *p,uint64_t addr,uint64_t size) {
    p[0]=(uint32_t)size;p[1]=(uint32_t)addr;
    p[2]=(uint32_t)((addr>>32)&15)<<28 | (uint32_t)((addr>>36)&7)<<2 | (uint32_t)(size>>32)<<24;
}
int main(void) {
    __attribute__((aligned(16))) uint32_t msg[64],before[64];
    Cpu c={0};c.tpidrro=(uintptr_t)msg;
    Service *service=service_new("echo",echo,NULL);service->on_close=close_service;
    child=service_new("child",echo,NULL);
    uint32_t h=session_new(service);
    // Raw TIPC payload starts at word 5 after a PID, without CMIF alignment.
    memset(msg,0,sizeof msg);msg[0]=19;msg[1]=0x80000002;msg[2]=1;
    msg[3]=0x55667788;msg[4]=0x11223344;msg[5]=0xaabbccdd;msg[6]=0x12345678;
    assert(!ipc_handle_request(&c,h) && calls==1);
    assert(msg[0]==0 && msg[1]==0x80000002 && msg[2]==(2|64));
    assert(msg[3]==0x77 && handle_get(msg[4]) && msg[5]==0x88);
    assert(((KSession *)handle_get(msg[4]))->svc==child && msg[6]==0 && msg[7]==0x87654321);
    // Incoming copy handle plus A/B/W map-alias descriptors.
    memset(msg,0,sizeof msg);msg[0]=25|(1u<<20)|(1u<<24)|(1u<<28);msg[1]=0x80000001;
    msg[2]=2;msg[3]=0x99;
    descriptor(msg+4,UINT64_C(0x7012345678),0x101);
    descriptor(msg+7,UINT64_C(0x7112345678),0x202);
    descriptor(msg+10,UINT64_C(0x7212345678),0x303);msg[13]=0xdeadbeef;
    assert(!ipc_handle_request(&c,h) && calls==2 && msg[1]==1 && msg[2]==0);
    // Existing CMIF request/response layout must remain unchanged.
    memset(msg,0,sizeof msg);msg[0]=4;msg[1]=9;memcpy(msg+4,"SFCI",4);msg[6]=7;msg[8]=0x11223344;
    assert(!ipc_handle_request(&c,h) && calls==3 && !memcmp(msg+4,"SFCO",4));
    assert(msg[6]==0 && msg[8]==0x55667788);
    // Same command as TIPC uses a compact result-first response.
    memset(msg,0,sizeof msg);msg[0]=23;msg[1]=1;msg[2]=0x11223344;
    assert(!ipc_handle_request(&c,h) && calls==4 && msg[1]==2 && msg[2]==0 && msg[3]==0x55667788);
    // Bounds and unsupported TIPC static/domain layouts reject before dispatch.
    const uint32_t bad[][2]={{16,64},{16|(9u<<20),0},{16|(1u<<16),0},{16,2u<<10}};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++) {
        memset(msg,0,sizeof msg);memcpy(msg,bad[i],8);memcpy(before,msg,sizeof msg);
        assert(ipc_handle_request(&c,h)!=0 && calls==4 && !memcmp(msg,before,sizeof msg));
    }
    memset(msg,0,sizeof msg);msg[0]=26;assert(ipc_handle_request(&c,h)!=0 && calls==5);
    unsigned allocated=next_handle;
    memset(msg,0,sizeof msg);msg[0]=28;
    assert(ipc_handle_request(&c,h)!=0 && calls==6 && next_handle==allocated);
    // Real service-manager registration is valid in both protocol generations.
    uint32_t sm=session_new(service_new("sm:",sm_handler,NULL));
    memset(msg,0,sizeof msg);msg[0]=16;msg[1]=0x80000000;msg[2]=1;
    assert(!ipc_handle_request(&c,sm) && msg[1]==1 && msg[2]==0);
    memset(msg,0,sizeof msg);msg[0]=17;msg[1]=1;
    assert(!ipc_handle_request(&c,sm) && msg[2]!=0); // truncated service name
    memset(msg,0,sizeof msg);msg[0]=15;
    assert(!ipc_handle_request(&c,h) && closed==1);
    // CMIF domain conversion, returned objects and object close remain usable.
    Service *root=service_new("domain",echo,NULL);
    uint32_t dh=session_new(root);
    memset(msg,0,sizeof msg);msg[0]=5;msg[1]=8;memcpy(msg+4,"SFCI",4);
    assert(!ipc_handle_request(&c,dh) && msg[8]==1);
    memset(msg,0,sizeof msg);msg[0]=16;memcpy(before,msg,sizeof msg);
    assert(ipc_handle_request(&c,dh)!=0 && calls==6 && !memcmp(msg,before,sizeof msg));
    memset(msg,0,sizeof msg);msg[0]=4;msg[1]=12;msg[4]=1|(16u<<16);msg[5]=1;
    memcpy(msg+8,"SFCI",4);msg[10]=11;
    assert(!ipc_handle_request(&c,dh) && calls==7);
    assert(msg[4]==1 && !memcmp(msg+8,"SFCO",4) && msg[10]==0);
    assert(msg[12]==0xabcdef01 && msg[13]==2 && ((KSession *)handle_get(dh))->domain->objs[2]==child);
    memset(msg,0,sizeof msg);msg[0]=4;msg[1]=12;msg[4]=2;msg[5]=2;
    assert(!ipc_handle_request(&c,dh) && !((KSession *)handle_get(dh))->domain->objs[2]);
    return 0;
}
