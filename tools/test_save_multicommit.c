// Real HIPC/CMIF messages through the production dispatcher and filesystem.
#include <assert.h>
#include "../runtime/hle.h"
#include "../runtime/ipc.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/ipc.c"
#undef SERVICE
#define SERVICE(name, fn) static void fn(Service *s, IpcReq *req, IpcResp *resp)
#include "../runtime/services/fs.c"

static KObject *test_handles[64];
uint32_t kernel_new_handle(KObject *object) {
    for (unsigned i=1;i<64;i++) if (!test_handles[i]) { test_handles[i]=object; return 0x100+i; }
    abort();
}
KObject *handle_get(uint32_t handle) {
    return handle>0x100 && handle<0x140 ? test_handles[handle-0x100] : NULL;
}
const char *g_romfs_dir;
int rt_romfs_files;
static TitleProfile profile = {.version="1.0.0"};
const TitleProfile *title_profile_current(void) { return &profile; }
bool title_content_validate(const TitleProfile *p,const char *root,bool image,int *count,char *why,size_t cap) { abort(); }
bool title_is_las(void) { return false; }
void save_editor_file_read(const char *p,uint64_t a,int64_t o,int64_t n) { abort(); }
void flags_ext_save_read(const char *p,uint64_t a,int64_t o,int64_t n) { abort(); }
bool flags_ext_save_written(const char *p,uint64_t a,int64_t o,int64_t n) { abort(); }
int game_profile_active(void) { abort(); }
const char *game_profile_save_dir(int p) { abort(); }
const char *workshop_save_dir(void) { abort(); }
const char *rando_seed_dir(void) { abort(); }
bool rando_romfs_override(const char *p,char *out,size_t n) { abort(); }
bool acnh60_romfs_override(const char *p,char *out,size_t n) { abort(); }
void romfs_layout_free(RomfsLayout *p) { abort(); }
bool romfs_layout(const char *p,RomfsLayout *l,char *why,size_t n) { abort(); }
void romfs_file_read_hook(const char *p) { abort(); }
bool research_io_begin(void) { abort(); }
void research_io_end(int64_t n) { abort(); }
void rt_fatal_msg(const char *fmt,...) { abort(); }
void plat_notice(const char *text) { abort(); }
void rt_fatal_trace(void) { abort(); }
static int snapshots;
bool save_backup_create(const char *root,const char *reason,const char *version,bool deduplicate,char *why,size_t cap) {
    assert(!strcmp(reason,"Automatic save")); ++snapshots; return true;
}
#ifdef _WIN32
ssize_t pread(int fd,void *buf,size_t size,off_t offset) {
    if (_lseeki64(fd,offset,SEEK_SET)<0) return -1; return _read(fd,buf,(unsigned)size);
}
ssize_t pwrite(int fd,const void *buf,size_t size,off_t offset) {
    if (_lseeki64(fd,offset,SEEK_SET)<0) return -1; return _write(fd,buf,(unsigned)size);
}
#endif
static _Alignas(16) uint32_t message[256];
static Cpu cpu;
static void request(unsigned type,unsigned command) {
    memset(message,0,sizeof message); message[0]=type; message[1]=8;
    memcpy(message+4,"SFCI",4); message[6]=command;
}
static void domain_request(unsigned object,unsigned command,unsigned input) {
    request(4,0); message[1]=16;
    message[4]=1 | ((input ? 1u : 0u)<<8) | (16u<<16); message[5]=object;
    memcpy(message+8,"SFCI",4); message[10]=command; message[12]=input;
}
static unsigned dispatch(unsigned handle,bool domain) {
    assert(!ipc_handle_request(&cpu,handle));
    return message[domain ? 10 : 6];
}
static void close_session(unsigned handle) {
    request(2,0); assert(!ipc_handle_request(&cpu,handle));
}
static void close_domain(unsigned handle,unsigned object) {
    domain_request(object,0,0); message[4]=2; assert(!dispatch(handle,true));
}
static Service *mount(const char *root,const char *name) {
    HostFs *fs=calloc(1,sizeof *fs); fs->is_save=fs->journaled=true;
    snprintf(fs->root,sizeof fs->root,"%s/%s",root,name); assert(!mkdir(fs->root,0755));
    Service *s=service_new("IFileSystem",filesystem,fs); s->on_close=host_fs_close; return s;
}
static void put(const char *path,int byte) {
    FILE *f=fopen(path,"wb"); assert(f); assert(fputc(byte,f)==byte); assert(!fclose(f));
}
static int get(const char *path) {
    FILE *f=fopen(path,"rb"); assert(f); int b=fgetc(f); fclose(f); return b;
}
static void stage(Service *mount,const char *name,int byte,char path[1200]) {
    HostFs *fs=mount->state; char pending[1200];
    snprintf(path,1200,"%s/%s",fs->root,name); put(path,1);
    assert(pending_of(fs,path,true,pending,sizeof pending) >= 0); put(pending,byte);
    assert(get(path)==1);
}
int main(int argc,char **argv) {
    assert(argc==2); cpu.tpidrro=(uintptr_t)message;
#ifdef _WIN32
    _set_fmode(_O_BINARY);
#endif
    Service *proxy=service_new("fsp-srv",fsp_srv,NULL);
    unsigned session=session_new(proxy);
    request(4,1200); assert(!dispatch(session,false));
    assert(message[1]&0x80000000u); unsigned manager_handle=message[3];
    Service *manager=((KSession *)handle_get(manager_handle))->svc;
    assert(manager && !strcmp(manager->name,"IMultiCommitManager"));
    Service *fs=mount(argv[1],"session"); unsigned fs_handle=session_new(fs);
    char paths[10][1200],name[64];
    assert(!mkdir((snprintf(paths[0],1200,"%s/Villager0",((HostFs *)fs->state)->root),paths[0]),0755));
    for (int i=0;i<10;i++) { snprintf(name,sizeof name,"%sfile%d.dat",i<2 ? "" : "Villager0/",i); stage(fs,name,20+i,paths[i]); }
    request(4,1); message[1]|=0x80000000u; message[2]=2; message[3]=fs_handle;
    assert(!dispatch(manager_handle,false)); assert(fs->refs==2);
    close_session(fs_handle); assert(fs->state && fs->refs==1); // retained by manager
    request(4,2); assert(!dispatch(manager_handle,false) && snapshots==1);
    for (int i=0;i<10;i++) assert(get(paths[i])==20+i);
    assert(!((HostFs *)fs->state)->npending);
    request(4,1); assert(dispatch(manager_handle,false)); // missing input is not a successful no-op
    close_session(manager_handle); assert(!manager->state && !fs->state);

    // Domains must resolve input IDs in this session's domain, not as kernel handles.
    request(5,0); assert(!dispatch(session,false)); assert(message[8]==1);
    Domain *domain=((KSession *)handle_get(session))->domain;
    Service *a=mount(argv[1],"domain-a"), *b=mount(argv[1],"domain-b");
    domain->objs[2]=a; service_retain(a); domain->objs[3]=b; service_retain(b);
    domain_request(1,1200,0); assert(!dispatch(session,true));
    assert(message[4]==1); unsigned mid=message[12]; assert(mid==4);
    manager=domain->objs[mid];
    domain_request(mid,1,2); assert(!dispatch(session,true));
    domain_request(mid,1,3); assert(!dispatch(session,true));
    domain_request(mid,1,2); assert(dispatch(session,true)); // duplicate
    domain_request(mid,1,1); assert(dispatch(session,true)); // wrong service type
    domain_request(mid,1,99); assert(dispatch(session,true)); // missing domain object
    domain_request(mid,1,300); assert(dispatch(session,true)); // out-of-range object
    char pa[1200],pb[1200]; stage(a,"main.dat",31,pa); stage(b,"main.dat",32,pb);
    assert(!unlink(pb) && !mkdir(pb,0755)); // deterministic publication failure
    domain_request(mid,2,0); assert(dispatch(session,true) && snapshots==1);
    assert(get(pa)==31 && ((HostFs *)b->state)->npending==1);
    assert(!rmdir(pb)); put(pb,1);
    domain_request(mid,2,0); assert(!dispatch(session,true) && snapshots==3);
    assert(get(pb)==32 && !((HostFs *)b->state)->npending);
    close_domain(session,2); close_domain(session,3); assert(a->state && b->state);
    close_domain(session,mid); assert(!a->state && !b->state && !manager->state);
    // Oversized input tables must not write beyond the request arrays.
    domain_request(1,1200,0); message[4]=1 | (9u<<8) | (16u<<16);
    assert(ipc_handle_request(&cpu,session)==0xE401);
    puts("PASS: real session/domain multi-commit IPC, ten staged files, retained mounts, invalid inputs and failure/retry");
}
