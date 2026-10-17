#include <assert.h>
#include "../runtime/services/prepo.c"
int main(void) {
    uint8_t data[24]={0}, room[16], report[32];
    memset(room,0xa5,sizeof room); memset(report,0x5a,sizeof report);
    IpcReq q={.data=data,.data_size=24,.nx=1,.x={{(uintptr_t)room,sizeof room}},
              .na=1,.a={{(uintptr_t)report,sizeof report}}};
    for(q.cmd=10100;q.cmd<=10105;q.cmd++) {
        IpcResp r={0}; prepo_service(NULL,&q,&r);
        assert(!r.result && !r.data_size && !r.nobjects && !r.ncopy && !r.nmove);
    }
    for(size_t i=0;i<sizeof room;i++) assert(room[i]==0xa5);
    for(size_t i=0;i<sizeof report;i++) assert(report[i]==0x5a);
    q.cmd=10103; q.data_size=23; IpcResp r={0}; prepo_service(NULL,&q,&r); assert(r.result);
    q.data_size=24; q.na=0; r=(IpcResp){0}; prepo_service(NULL,&q,&r); assert(r.result);
    q.cmd=10200; r=(IpcResp){0}; prepo_service(NULL,&q,&r); assert(r.result && !r.data_size);
    puts("PASS: offline report commands, input preservation, size validation and unsupported transmission");
}
