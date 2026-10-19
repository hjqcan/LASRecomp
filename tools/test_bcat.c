#include <assert.h>
#include <stdlib.h>
#include "../runtime/services/bcat.c"
static Service objects[4]; static unsigned object_count;
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    assert(object_count<4); Service *s=&objects[object_count++];
    *s=(Service){.name=name,.handler=handler,.state=state}; return s;
}
void ipc_return_object(IpcResp *r, Service *s) { r->objects[r->nobjects++]=s; }
void rt_fatal_msg(const char *fmt, ...) { (void)fmt; abort(); }
void rt_fatal_trace(void) { abort(); }
int main(void) {
    uint8_t names[64]={0}, buffer[96]; memset(buffer,0xa5,sizeof buffer);
    IpcReq q={0}; IpcResp r;
    for(unsigned kind=0;kind<2;kind++) {
        q.cmd=kind; r=(IpcResp){0}; bcat_storage(NULL,&q,&r);
        assert(!r.result && r.nobjects==1 && !r.data_size);
        Service *reader=r.objects[0]; unsigned size=kind==0 ? 64 : 32;
        q=(IpcReq){.data=names,.data_size=size,.nb=1,.b={{(uintptr_t)buffer,sizeof buffer}}};
        r=(IpcResp){0}; reader->handler(reader,&q,&r);
        assert(r.result==MAKE_RESULT(122,2) && !r.data_size && !r.nobjects);
        for(unsigned n=0;n<size;n++) {
            q.data_size=n; r=(IpcResp){0}; reader->handler(reader,&q,&r);
            assert(r.result==MAKE_RESULT(122,1));
        }
        for(q.cmd=1;q.cmd<=(kind==0 ? 3u : 2u);q.cmd++) {
            r=(IpcResp){0}; reader->handler(reader,&q,&r);
            assert(r.result==MAKE_RESULT(122,7) && !r.data_size && !r.nobjects && !r.ncopy && !r.nmove);
        }
        for(unsigned i=0;i<sizeof buffer;i++) assert(buffer[i]==0xa5);
    }
    q=(IpcReq){.cmd=10,.nb=1,.b={{(uintptr_t)buffer,64}}};
    r=(IpcResp){0}; bcat_storage(NULL,&q,&r);
    assert(!r.result && r.data_size==4 && !*(uint32_t *)r.data);
    for(unsigned i=64;i<sizeof buffer;i++) assert(buffer[i]==0xa5);
    puts("PASS: empty delivery-cache file/directory objects, missing entries, unopened reads and enumeration bounds");
}
