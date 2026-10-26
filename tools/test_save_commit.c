// Exercise the production filesystem and extended flags together, using scratch files and a snapshot observer.
#include <assert.h>
#include "../runtime/hle.h"
#include "../runtime/ipc.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#undef SERVICE
#define SERVICE(name, fn) static void fn(Service *s, IpcReq *req, IpcResp *resp)
#include "../runtime/services/fs.c"
#ifdef __linux__
void plat_os_write(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
#endif
bool acnh60_romfs_override(const char *p, char *out, size_t n) { abort(); }
#include "../runtime/game_addresses.h"
static uint8_t live_save[0x6000];
static const GameAddrs test_addresses = {.LIVE_SAVE=(uintptr_t)live_save};
const GameAddrs *g_ga=&test_addresses;
void pace_hitch(const char *why) { (void)why; }
#include "../runtime/hle/flags_ext.c"

static const char *save_root;
static int snapshots, automatic_snapshots, editor_reads;
static unsigned expected_data, expected_flags;
static int notices;
void plat_notice(const char *text) { (void)text; ++notices; }

#ifdef _WIN32
ssize_t pread(int fd, void *buf, size_t size, off_t offset) {
    if (_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    return _read(fd, buf, (unsigned)size);
}
ssize_t pwrite(int fd, const void *buf, size_t size, off_t offset) {
    if (_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    return _write(fd, buf, (unsigned)size);
}
#endif
GameLayout game_version_current(void) { return GAME_100; }
const char *game_version_name(GameLayout version) { (void)version; return "1.0.0"; }
int game_profile_active(void) { return 0; }
const char *game_profile_save_dir(int profile) { (void)profile; return save_root; }
const char *rando_seed_dir(void) { return NULL; }
const char *workshop_save_dir(void) { return NULL; }
void save_editor_file_read(const char *path, uint64_t address, int64_t offset, int64_t size) {
    (void)path; (void)address; (void)offset; (void)size;
    ++editor_reads;
}
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    Service *s = calloc(1, sizeof *s);
    s->name = name; s->handler = handler; s->state = state;
    return s;
}
void ipc_return_object(IpcResp *resp, Service *s) { resp->objects[resp->nobjects++] = s; }
void service_retain(Service *s) { ++s->refs; }
void service_release(Service *s) { if (!--s->refs && s->on_close) s->on_close(s); }
static int first_byte(const char *path) {
    FILE *f = fopen(path, "rb"); assert(f); int value = fgetc(f); fclose(f); return value;
}
static void put_file(const char *path, unsigned byte, size_t count) {
    FILE *f = fopen(path, "wb"); assert(f);
    for (size_t i = 0; i < count; ++i) assert(fputc(byte, f) != EOF);
    assert(!fclose(f));
}
bool save_backup_create(const char *root, const char *reason, const char *version, bool deduplicate,
                        char *why, size_t cap) {
    (void)version; (void)deduplicate; (void)why; (void)cap;
    DIR *d = opendir(root); assert(d); struct dirent *e;
    while ((e = readdir(d))) assert(!strstr(e->d_name, ".pending"));
    closedir(d); ++snapshots;
    if (!strcmp(reason, "Automatic save")) {
        char p[1200]; snprintf(p, sizeof p, "%s/savedata0.bin", root);
        assert(first_byte(p) == (int)expected_data);
        snprintf(p, sizeof p, "%s/savedata0.bin.flagsx", root);
        assert(first_byte(p) == (int)expected_flags);
        ++automatic_snapshots;
    }
    return true;
}
static IpcResp open_file(Service *mount, const char *name, uint32_t mode) {
    IpcReq req = {.cmd = 8, .data = (const uint8_t *)&mode, .nx = 1};
    req.x[0].addr = (uintptr_t)name;
    IpcResp resp = {0}; filesystem(mount, &req, &resp); return resp;
}
static void close_file(Service *s) {
    assert(s->on_close); s->on_close(s); assert(!s->state); free(s);
}
static void write_save(Service *mount, unsigned data, unsigned flags) {
    uint8_t bytes[BODY_CRC + 4]; memset(bytes, data, sizeof bytes);
    memset(g_buf + 256, 0, EXT); g_buf[256] = (uint8_t)flags;
    IpcResp opened = open_file(mount, "/savedata0.bin", 2); assert(!opened.result);
    Service *s = opened.objects[0]; HostFile *f = s->state;
    assert(!f->scratch && strstr(f->path, ".pending"));
    uint64_t args[3] = {0, 0, sizeof bytes};
    IpcReq req = {.cmd = 1, .data = (const uint8_t *)args, .na = 1};
    req.a[0] = (IpcBuf){(uintptr_t)bytes, sizeof bytes};
    IpcResp resp = {0}; file(s, &req, &resp); assert(!resp.result);
    char canonical[1100]; assert(save_path(f->path, canonical, sizeof canonical));
    assert(!strcmp(g_current, canonical)); close_file(s);
}
static uint32_t commit_mount(Service *mount) {
    IpcReq req = {.cmd = 10}; IpcResp resp = {0}; filesystem(mount, &req, &resp); return resp.result;
}
static void free_mount(Service *s) { s->on_close(s); free(s); }

int main(int argc, char **argv) {
    assert(argc == 2); save_root = argv[1]; g_on = 1;
    title_profile_preview("las", "1.0.0"); assert(title_is_las());
    unsetenv("LARECOMP_SAVE_DIR");
    unsetenv("ACNH_LOG_SAVE"); // LAS sidecars must work without optional save logging.
#ifdef _WIN32
    _set_fmode(_O_BINARY);
#endif
    char dir[1100], path[1200], sidecar[1200], pending[1200], pending_flags[1200];
    snprintf(dir, sizeof dir, "%s/save_0", save_root); assert(!mkdir(dir, 0755));
    snprintf(path, sizeof path, "%s/savedata0.bin", dir);
    snprintf(sidecar, sizeof sidecar, "%s.flagsx", path);
    snprintf(pending, sizeof pending, "%s.pending", path);
    snprintf(pending_flags, sizeof pending_flags, "%s.flagsx.pending", path);
    put_file(path, 0x11, BODY_CRC + 4); put_file(sidecar, 0x11, EXT);
    Service *mount = open_host_fs("save_0"); assert(snapshots == 1);
    write_save(mount, 0x22, 0x22);
    assert(first_byte(path) == 0x11 && first_byte(sidecar) == 0x11);
    assert(first_byte(pending) == 0x22 && first_byte(pending_flags) == 0x22);
    IpcResp opened = open_file(mount, "/savedata0.bin", 1); assert(!opened.result);
    uint8_t bytes[BODY_CRC + 4]; uint64_t args[3] = {0, 0, sizeof bytes};
    IpcReq read_req = {.cmd = 0, .data = (const uint8_t *)args, .nb = 1};
    read_req.b[0] = (IpcBuf){(uintptr_t)bytes, sizeof bytes};
    IpcResp read_resp = {0}; file(opened.objects[0], &read_req, &read_resp);
    assert(!read_resp.result && bytes[0] == 0x22 && g_slots[0].ext[0] == 0x22);
    assert(editor_reads == 1);
    close_file(opened.objects[0]); expected_data = expected_flags = 0x22;
    assert(!commit_mount(mount) && automatic_snapshots == 1);
    assert(access(pending, F_OK) && access(pending_flags, F_OK));

    // Clearing flags must replace an existing sidecar too.
    write_save(mount, 0x23, 0); expected_data = 0x23; expected_flags = 0;
    assert(!commit_mount(mount) && automatic_snapshots == 2);
    // A new mount discards uncommitted data and flags before the mount backup.
    write_save(mount, 0x33, 0x33); free_mount(mount); mount = open_host_fs("save_0");
    assert(first_byte(path) == 0x23 && first_byte(sidecar) == 0);
    assert(access(pending, F_OK) && access(pending_flags, F_OK));

    // Staging failure must never fall back to overwriting the committed file.
    HostFs *fs = mount->state; fs->npending = PENDING_MAX;
    opened = open_file(mount, "/savedata0.bin", 2); assert(!opened.result && notices == 1);
    assert(((HostFile *)opened.objects[0]->state)->scratch); close_file(opened.objects[0]);
    assert(first_byte(path) == 0x23); fs->npending = 0;
    assert(!commit_mount(mount) && automatic_snapshots == 2);
    // LAS is told success, but failed publication drops the working copy and
    // suppresses the backup. A later complete save can still succeed.
    write_save(mount, 0x44, 0x44); assert(!unlink(path) && !mkdir(path, 0755));
    assert(!commit_mount(mount) && fs->npending == 0 && automatic_snapshots == 2);
    assert(access(pending, F_OK) && access(pending_flags, F_OK));
    assert(!rmdir(path)); put_file(path, 0x23, BODY_CRC + 4);
    write_save(mount, 0x44, 0x44);
    expected_data = expected_flags = 0x44;
    assert(!commit_mount(mount) && automatic_snapshots == 3);
    // A sidecar failure after the data rename survives for recovery at the next
    // mount. Even another empty Commit must not snapshot the incomplete pair.
    write_save(mount, 0x55, 0x55); assert(!unlink(sidecar) && !mkdir(sidecar, 0755));
    assert(!commit_mount(mount) && fs->npending == 0 && automatic_snapshots == 3);
    assert(!commit_mount(mount) && automatic_snapshots == 3);
    assert(first_byte(path) == 0x55 && !rmdir(sidecar)); put_file(sidecar, 0x44, EXT);
    free_mount(mount); mount = open_host_fs("save_0");
    assert(first_byte(sidecar) == 0x55 && access(pending_flags, F_OK));
    expected_data = expected_flags = 0x55;
    assert(!commit_mount(mount) && automatic_snapshots == 4);

    // Another title still receives atomic writes, without LAS save-editor or flag hooks.
    title_profile_preview("acnh", "1.0.0"); assert(!title_is_las());
    uint32_t previous_crc = g_slots[0].crc;
    write_save(mount, 0x66, 0x66);
    assert(first_byte(path) == 0x55 && first_byte(pending) == 0x66 && access(pending_flags, F_OK));
    opened = open_file(mount, "/savedata0.bin", 1); assert(!opened.result);
    read_resp = (IpcResp){0}; file(opened.objects[0], &read_req, &read_resp);
    assert(!read_resp.result && bytes[0] == 0x66 && editor_reads == 1 && g_slots[0].crc == previous_crc);
    close_file(opened.objects[0]); expected_data = 0x66; expected_flags = 0x55;
    assert(!commit_mount(mount) && automatic_snapshots == 5 && access(pending, F_OK));
    // A title that handles write errors may repair and retry the working copy;
    // LAS's failed-write suppression must not discard that successful retry.
    opened = open_file(mount, "/savedata0.bin", 2); assert(!opened.result);
    Service *retry = opened.objects[0]; HostFile *retry_file = retry->state;
    int retry_fd = retry_file->fd; retry_file->fd = -1;
    memset(bytes, 0x77, sizeof bytes);
    IpcReq write_req = {.cmd = 1, .data = (const uint8_t *)args, .na = 1};
    write_req.a[0] = (IpcBuf){(uintptr_t)bytes, sizeof bytes};
    IpcResp write_resp = {0}; file(retry, &write_req, &write_resp); assert(write_resp.result);
    retry_file->fd = retry_fd; write_resp = (IpcResp){0};
    file(retry, &write_req, &write_resp); assert(!write_resp.result);
    close_file(retry); expected_data = 0x77;
    assert(!commit_mount(mount) && automatic_snapshots == 6 && access(pending, F_OK));
    title_profile_preview("las", "1.0.0");
    free_mount(mount);

    int before_sd = snapshots; mount = open_host_fs("sdcard");
    snprintf(path, sizeof path, "%s/sdcard/note.txt", save_root); put_file(path, 'a', 1);
    opened = open_file(mount, "/note.txt", 2); assert(!opened.result);
    HostFile *sd = opened.objects[0]->state; assert(!sd->fs && !strstr(sd->path, ".pending"));
    assert(pwrite(sd->fd, "b", 1, 0) == 1); close_file(opened.objects[0]);
    assert(first_byte(path) == 'b' && !commit_mount(mount) && snapshots == before_sd);
    free_mount(mount);
    puts("Save commit: staged reads, LAS-only flags/editor, backup order, abandoned writes, failed-save suppression, sidecar recovery, close and SD isolation passed");
}
