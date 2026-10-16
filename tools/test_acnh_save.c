// Exercise the production host-file IPC handlers against a scratch directory.
#include <assert.h>
#include "../runtime/services/fs.c"

// The test uses host saves only. Any accidental call into game/renderer state
// must fail, rather than silently becoming a replacement for production I/O.
const char *g_romfs_dir;
int rt_romfs_files;
void romfs_layout_free(RomfsLayout *p) { abort(); }
bool romfs_layout(const char *p, RomfsLayout *l, char *why, size_t n) { abort(); }
const char *rando_seed_dir(void) { abort(); }
bool rando_romfs_override(const char *p, char *out, size_t n) { abort(); }
void romfs_file_read_hook(const char *p) { abort(); }
void rt_fatal_msg(const char *fmt, ...) { abort(); }
void rt_fatal_trace(void) { abort(); }
bool rt_fault_watch(uint64_t a, uint64_t pc) { return false; }
void rt_crash_explain(char *out, size_t n, const char *path) { abort(); }
const char *rt_host_fn_name(uint64_t pc, uint64_t *off) { return NULL; }
void rt_fault_report(const char *what, uint64_t a, uint64_t pc, uint64_t lr) { abort(); }

Service *service_new(const char *name, ServiceHandler fn, void *state) {
    Service *s = calloc(1, sizeof *s);
    s->name = name; s->handler = fn; s->state = state;
    return s;
}
void ipc_return_object(IpcResp *r, Service *s) { r->objects[r->nobjects++] = s; }

int main(int argc, char **argv) {
    assert(argc == 2);
    HostFs fs = {0}; strlcpy(fs.root, argv[1], sizeof fs.root);
    Service root = {.state=&fs};
    uint64_t data[4] = {0,4096,0,0};
    const char path[] = "/main.dat";
    IpcReq req = {.cmd=0,.data=(uint8_t *)data,.data_size=sizeof data,
                  .nx=1,.x={{(uint64_t)(uintptr_t)path,sizeof path}}};
    IpcResp resp = {0};
    filesystem(&root,&req,&resp); assert(!resp.result);
    char host[2048];
    assert(!host_path(&fs,"/../outside",host,sizeof host));
    assert(!host_path(&fs,"/C:/outside",host,sizeof host));
    assert(!host_path(&fs,"/..\\outside",host,sizeof host));
    assert(host_path(&fs,"/a/b.dat",host,sizeof host));
    const char payload[] = "ACNH persistent save fixture";
    for (int attempt=0; attempt<128; attempt++) {
        data[0]=3; req.cmd=8; memset(&resp,0,sizeof resp);
        filesystem(&root,&req,&resp); assert(!resp.result && resp.nobjects==1);
        Service *f=resp.objects[0]; int fd=((HostFile *)f->state)->fd;
        assert(fd<64); // opening and closing repeatedly must not leak descriptors
        uint64_t io[3]={1,123,sizeof payload};
        IpcReq w={.cmd=1,.data=(uint8_t *)io,.data_size=sizeof io,
                  .na=1,.a={{(uint64_t)(uintptr_t)payload,sizeof payload}}};
        memset(&resp,0,sizeof resp); file(f,&w,&resp); assert(!resp.result);
        w.a[0].size=sizeof payload-1; memset(&resp,0,sizeof resp);
        file(f,&w,&resp); assert(resp.result); // reject source-buffer overrun
        f->on_close(f); assert(!f->state); free(f);
        char readback[sizeof payload]={0};
        data[0]=1; req.cmd=8; memset(&resp,0,sizeof resp);
        filesystem(&root,&req,&resp); assert(!resp.result);
        f=resp.objects[0];
        IpcReq r={.cmd=0,.data=(uint8_t *)io,.data_size=sizeof io,
                  .nb=1,.b={{(uint64_t)(uintptr_t)readback,sizeof readback}}};
        memset(&resp,0,sizeof resp); file(f,&r,&resp);
        assert(!resp.result && *(int64_t *)resp.data==sizeof payload && !memcmp(payload,readback,sizeof payload));
        f->on_close(f); free(f);
    }
    req.cmd=1; memset(&resp,0,sizeof resp); filesystem(&root,&req,&resp); assert(!resp.result);
    puts("PASS: save write/flush/close/reopen, 128 cycles, bounds and mounted path checks");
}
