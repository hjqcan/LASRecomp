#include <assert.h>
#include "../runtime/services/olsc.c"

static IpcResp call(Service *s, unsigned command, uint8_t *data, unsigned size) {
    IpcReq q={.cmd=command,.data=data,.data_size=size}; IpcResp r={0};
    olsc_user(s,&q,&r); return r;
}
int main(void) {
    Service first={0}, second={0};
    uint8_t request[24]={1}, saved[24]; memcpy(saved,request,sizeof saved);
    IpcResp r=call(&first,13,request,16); assert(r.result==OLSC_NOT_INITIALIZED);
    r=call(&first,0,NULL,0); assert(!r.result && !r.data_size);
    r=call(&first,13,request,16);
    assert(!r.result && r.data_size==1 && r.data[0]==2);
    assert(!r.nobjects && !r.ncopy && !r.nmove && !memcmp(request,saved,sizeof saved));
    assert(call(&second,13,request,16).result==OLSC_NOT_INITIALIZED);
    for(unsigned n=0;n<16;n++) assert(call(&first,13,request,n).result==OLSC_NULL_ARGUMENT);
    request[0]=0; assert(call(&first,13,request,16).result==OLSC_NULL_ARGUMENT);
    request[8]=1; assert(!call(&first,14,request,24).result);
    for(unsigned n=0;n<24;n++) assert(call(&first,14,request,n).result==OLSC_NULL_ARGUMENT);
    request[0]=1; assert(call(&first,14,request,24).result==OLSC_UNSUPPORTED);
    const unsigned unsupported[]={10,15,16,18,19,22,25,26,27,999};
    for(unsigned i=0;i<sizeof unsupported/sizeof *unsupported;i++) {
        r=call(&first,unsupported[i],request,24);
        assert(r.result==OLSC_UNSUPPORTED && !r.data_size && !r.nobjects && !r.ncopy && !r.nmove);
    }
    puts("PASS: offline cloud-backup status, session initialization, input bounds, disabled setting and unsupported cloud requests");
}
