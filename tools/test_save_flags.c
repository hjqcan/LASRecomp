// Save/commit/reopen exercises the actual filesystem and extended-flag hooks together.
#include "test_host_fs.h"

static void save_slot(Service *mount, unsigned value, unsigned flags) {
    uint8_t bytes[BODY_CRC+4]; memset(bytes, value, sizeof bytes);
    uint32_t rupees = 9999; memcpy(bytes+0x7C, &rupees, 4); memcpy(bytes+0x5540, &rupees, 4);
    memset(g_buf+256, 0, EXT); g_buf[256] = (uint8_t)flags;
    IpcResp opened = open_file(mount, "/savedata0.bin", 2); assert(!opened.result);
    assert(!write_file(opened.objects[0], bytes, sizeof bytes, 0)); close_file(opened.objects[0]);
}
static void read_slot(Service *mount, unsigned data, unsigned flags) {
    IpcResp opened = open_file(mount, "/savedata0.bin", 1); assert(!opened.result);
    uint8_t bytes[BODY_CRC+4]; uint64_t args[3] = {0, 0, sizeof bytes};
    IpcReq q = {.cmd=0, .data=(const uint8_t *)args, .nb=1}; q.b[0] = (IpcBuf){(uintptr_t)bytes, sizeof bytes};
    IpcResp r = {0}; file(opened.objects[0], &q, &r); close_file(opened.objects[0]);
    assert(!r.result && bytes[0] == data && g_nslots == 1 && g_slots[0].ext[0] == flags);
    uint32_t rupees = data == 0x11 ? 0x11111111 : 9999; assert(!memcmp(bytes+0x7C, &rupees, 4) && !memcmp(bytes+0x5540, &rupees, 4));
    assert(!strstr(g_slots[0].path, ".pending"));
}
static Service *remount(Service *old) {
    mounts_reset(); free(old->state); free(old); flags_ext_reset(); g_on = 1;  // (as a new run: no live mounts)
    Service *next = open_host_fs("save_0"); assert(next); return next;
}
int main(int argc, char **argv) {
    assert(argc == 2);
#ifdef _WIN32
    _set_fmode(_O_BINARY);
#endif
    setenv("LARECOMP_SAVE_DIR", argv[1], 1); g_on = 1;
    char dir[1100], path[1200], flags[1250], pending[1250], flags_pending[1250], orphan[1250];
    snprintf(dir, sizeof dir, "%s/save_0", argv[1]); assert(!mkdir(dir, 0755));
    snprintf(path, sizeof path, "%s/savedata0.bin", dir);
    snprintf(flags, sizeof flags, "%s.flagsx", path);
    snprintf(pending, sizeof pending, "%s.pending", path);
    snprintf(flags_pending, sizeof flags_pending, "%s.flagsx.pending", path);
    snprintf(orphan, sizeof orphan, "%s.pending.flagsx", path);
    put_file(path, 0x11, BODY_CRC+4); put_file(flags, 0x11, 256); // old short sidecars remain readable
    Service *mount = open_host_fs("save_0"); assert(mount); read_slot(mount, 0x11, 0x11);
    save_slot(mount, 0x22, 0x22);
    assert(first_byte(path) == 0x11 && first_byte(flags) == 0x11);
    assert(first_byte(pending) == 0x22 && first_byte(flags_pending) == 0x22 && access(orphan, F_OK));
    read_slot(mount, 0x22, 0x22);
    assert(!commit_mount(mount)); mount = remount(mount); read_slot(mount, 0x22, 0x22);
    assert(access(pending, F_OK) && access(flags_pending, F_OK));

    // Clearing flags must replace the old sidecar, not leave its bits set.
    save_slot(mount, 0x23, 0); assert(!commit_mount(mount));
    mount = remount(mount); read_slot(mount, 0x23, 0);
    // Process interruption before the data rename abandons both staged files.
    save_slot(mount, 0x33, 0x33); mount = remount(mount); read_slot(mount, 0x23, 0);
    assert(access(pending, F_OK) && access(flags_pending, F_OK));
    // Process interruption between the data and sidecar renames completes the same pair on mount.
    save_slot(mount, 0x44, 0x44); assert(!replace_file(pending, path));
    mount = remount(mount); read_slot(mount, 0x44, 0x44);
    assert(access(flags_pending, F_OK));

    // A sidecar publication failure (the data already replaced) succeeds for the game and keeps the staged sidecar,
    // which the next mount publishes to finish the pair.
    save_slot(mount, 0x55, 0x55); assert(!unlink(flags) && !mkdir(flags, 0755));
    assert(!commit_mount(mount) && !((HostFs *)mount->state)->npending);
    assert(first_byte(path) == 0x55 && !access(flags_pending, F_OK) && access(pending, F_OK));
    assert(!rmdir(flags)); put_file(flags, 0x44, EXT);
    mount = remount(mount); read_slot(mount, 0x55, 0x55);
    // A failure before the data rename drops the whole save: data and sidecar stay as committed.
    save_slot(mount, 0x56, 0x56); sync_error = 1; assert(!commit_mount(mount)); sync_error = 0;
    assert(access(pending, F_OK) && access(flags_pending, F_OK));
    mount = remount(mount); read_slot(mount, 0x55, 0x55);

    // If recovery cannot publish the sidecar, the mount fails and preserves it for the next attempt.
    save_slot(mount, 0x66, 0x66); assert(!replace_file(pending, path));
    assert(!unlink(flags) && !mkdir(flags, 0755));
    mounts_reset();  // (as a new run)
    assert(!open_host_fs("save_0") && !access(flags_pending, F_OK));
    assert(!rmdir(flags)); mount = remount(mount); read_slot(mount, 0x66, 0x66);

    // A second mount while the first is still open (mid-save) leaves the first one's working copies alone; once the
    // first is closed, a new mount drops what it left.
    save_slot(mount, 0x77, 0x77); assert(!access(pending, F_OK));
    Service *second = open_host_fs("save_0"); assert(second && !access(pending, F_OK));
    assert(!commit_mount(mount) && first_byte(path) == 0x77);
    save_slot(mount, 0x78, 0x78);
    second->on_close(second); free(second->state); free(second);
    assert(!access(pending, F_OK));  // (the first mount is still open)
    mount->on_close(mount);
    Service *third = open_host_fs("save_0"); assert(third && access(pending, F_OK) && first_byte(path) == 0x77);
    free(mount->state); free(mount); mount = third; flags_ext_reset(); g_on = 1; read_slot(mount, 0x77, 0x77);

    // Flags disabled: ordinary saves still commit with no sidecar, and the SD mount remains unjournaled.
    g_on = 0; IpcResp opened = open_file(mount, "/savedata0.bin", 2); assert(!opened.result);
    assert(!write_file(opened.objects[0], "v", 1, 0)); close_file(opened.objects[0]);
    assert(!commit_mount(mount) && first_byte(path) == 'v' && first_byte(flags) == 0x77);
    Service *sd = open_host_fs("sdcard"); assert(sd && !((HostFs *)sd->state)->journaled);
    free(sd->state); free(sd); free(mount->state); free(mount);
    puts("Save flags: legacy reads, staged reads, commit/reload, clearing, abandoned writes and interrupted publication passed");
}
