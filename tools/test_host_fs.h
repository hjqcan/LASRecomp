// Run the production host filesystem with scratch files; unrelated game paths must not be used.
#pragma once
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include "../runtime/hle.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#ifdef _WIN32
ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    return _read(fd, buf, (unsigned)n);
}
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    return _write(fd, buf, (unsigned)n);
}
#endif
static int sync_error, write_interrupts, write_zero;
static size_t write_limit, written;
static int64_t write_fail_after = -1;
static int test_sync(int fd) {
    if (sync_error) { errno = EIO; return -1; }
    return fsync(fd);
}
static ssize_t test_write(int fd, const void *p, size_t n, off_t off) {
    if (write_interrupts) { write_interrupts--; errno = EINTR; return -1; }
    if (write_zero) return 0;
    if (write_fail_after >= 0) {
        if (written >= (size_t)write_fail_after) { errno = ENOSPC; return -1; }
        if (n > (size_t)write_fail_after - written) n = (size_t)write_fail_after - written;
    }
    if (write_limit && n > write_limit) n = write_limit;
    ssize_t result = pwrite(fd, p, n, off);
    if (result > 0) written += (size_t)result;
    return result;
}
#define fsync test_sync
#define pwrite test_write
#include "../runtime/services/fs.c"
#undef pwrite
#undef fsync
const char *g_romfs_dir;
int rt_romfs_files;
const char *rando_seed_dir(void) { return NULL; }
bool romfs_layout(const char *p, RomfsLayout *l, char *why, size_t n) { abort(); }
bool rando_romfs_override(const char *p, char *out, size_t n) { abort(); }
void romfs_file_read_hook(const char *p) { abort(); }
void enemy_random_filter(const char *p, uint64_t off, uint8_t *d, uint64_t n) { abort(); }
void rt_fatal_msg(const char *fmt, ...) { abort(); }
void rt_fatal_trace(void) { abort(); }
void save_editor_file_read(const char *p, uint64_t a, int64_t o, int64_t n) {}
static int notices;  // save-failed warnings shown (fs.c tells the player once per run)
void plat_notice(const char *text) { notices++; }
#include "../runtime/game_addresses.h"
static uint8_t live_save[0x6000];
#undef GA_LIVE_SAVE
#define GA_LIVE_SAVE ((uintptr_t)live_save)
#include "../runtime/hle/flags_ext.c"
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    Service *s = calloc(1, sizeof *s); s->name = name; s->handler = handler; s->state = state; return s;
}
void ipc_return_object(IpcResp *r, Service *s) { r->objects[r->nobjects++] = s; }
static void put_file(const char *p, unsigned byte, size_t n) {
    FILE *f = fopen(p, "wb"); assert(f);
    while (n--) assert(fputc(byte, f) != EOF);
    assert(!fclose(f));
}
static int first_byte(const char *p) {
    FILE *f = fopen(p, "rb"); assert(f); int b = fgetc(f); fclose(f); return b;
}
static IpcResp open_file(Service *s, const char *p, uint32_t mode) {
    IpcReq q = {.cmd=8, .data=(const uint8_t *)&mode, .nx=1}; q.x[0].addr = (uintptr_t)p;
    IpcResp r = {0}; filesystem(s, &q, &r); return r;
}
static void close_file(Service *s) { s->on_close(s); assert(!s->state); free(s); }
static uint32_t write_file(Service *s, const void *p, size_t n, unsigned option) {
    uint64_t args[3] = {option, 0, n}; IpcReq q = {.cmd=1, .data=(const uint8_t *)args, .na=1};
    q.a[0] = (IpcBuf){(uintptr_t)p, n}; IpcResp r = {0}; file(s, &q, &r); return r.result;
}
static uint32_t commit_mount(Service *s) {
    IpcReq q = {.cmd=10}; IpcResp r = {0}; filesystem(s, &q, &r); return r.result;
}
