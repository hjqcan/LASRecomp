// Open files stay bounded: a seed's replacement RomFS files are served through a small cache of open files (it used
// to keep every one open until reboot and ran out of descriptors mid-play: saves then failed and seed-file reads came
// back as zeros). Also: a file that can't be opened reads as zeros at once (logged, never a hang), and a save whose
// working copy and scratch copy both can't be made (out of descriptors) is dropped without an error for the game.
#include "test_host_fs.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif

#define NFILES 300  // several times the cache

static int open_fds(void) {  // descriptors open in this process
    int n = 0;
#ifndef _WIN32
    for (int fd = 0; fd < 4096; fd++) n += fcntl(fd, F_GETFD) != -1;
#endif
    return n;
}

static void read_all(void) {
    uint8_t buf[100];
    for (int i = 0; i < NFILES; i++) {
        memset(buf, 0xEE, sizeof buf);
        assert(romfs_read(buf, sizeof buf, ov[i].start) == (ssize_t)sizeof buf);
        for (size_t k = 0; k < sizeof buf; k++) assert(buf[k] == (uint8_t)i);
        assert(fs_overlay_open_fds() <= OV_FDS);
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
#ifdef _WIN32
    _set_fmode(_O_BINARY);
#endif
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/seed", argv[1]);
    assert(!mkdir(dir, 0755));
    ov_cap = ov_n = NFILES;
    ov = calloc(NFILES, sizeof *ov);
    for (int i = 0; i < NFILES; i++) {
        char p[1300];
        snprintf(p, sizeof p, "%s/f%03d.bin", dir, i);
        put_file(p, (unsigned)(uint8_t)i, 100);
        ov[i] = (Overlay){0x10000 + (uint64_t)i * 0x70, 100, strdup(p), false, NULL};
    }
    int before = open_fds();
    read_all();
    read_all();  // (again: every file reopened through the cache)
    assert(fs_overlay_open_fds() == OV_FDS && open_fds() - before <= OV_FDS);

#ifndef _WIN32
    // No descriptor to spare (the limit at what's open, the cache full): reads still work - the cache gives one back.
    struct rlimit old, low;
    assert(!getrlimit(RLIMIT_NOFILE, &old));
    low = old;
    low.rlim_cur = (rlim_t)open_fds() + 1;
    assert(!setrlimit(RLIMIT_NOFILE, &low));
    read_all();

    // ...and a save then can't be staged or copied: dropped whole, the game is told it worked, the committed save
    // stays (no writes to a read-only file).
    char path[1300];
    snprintf(path, sizeof path, "%s/slot.bin", argv[1]);
    put_file(path, 0x11, 32);
    pthread_mutex_lock(&g_ov_lock);
    ov_fds_close();
    pthread_mutex_unlock(&g_ov_lock);
    low.rlim_cur = (rlim_t)open_fds();
    assert(!setrlimit(RLIMIT_NOFILE, &low));
    HostFs fs = {.journaled=true}; strlcpy(fs.root, argv[1], sizeof fs.root);
    Service mount = {.state=&fs};
    IpcResp opened = open_file(&mount, "/slot.bin", 2); assert(!opened.result && opened.nobjects == 1);
    HostFile *f = opened.objects[0]->state;
    assert(f->dropped && f->fd < 0 && notices == 1);
    assert(!write_file(opened.objects[0], "zz", 2, 0));
    uint8_t got[8]; uint64_t args[3] = {0, 0, sizeof got};
    IpcReq read = {.cmd=0, .data=(const uint8_t *)args, .nb=1};
    read.b[0] = (IpcBuf){(uintptr_t)got, sizeof got}; IpcResp response = {0};
    file(opened.objects[0], &read, &response); assert(!response.result);
    close_file(opened.objects[0]);
    assert(!commit_mount(&mount));
    assert(!setrlimit(RLIMIT_NOFILE, &old));
    assert(first_byte(path) == 0x11 && !fs.npending);
#endif

    // A file that can't be opened: zeros straight away, logged once.
    free(ov[7].path);
    ov[7].path = strdup("/nonexistent/las-recomp-test.bin");
    uint8_t buf[100];
    memset(buf, 0xEE, sizeof buf);
    assert(romfs_read(buf, sizeof buf, ov[7].start) == (ssize_t)sizeof buf);
    for (size_t k = 0; k < sizeof buf; k++) assert(buf[k] == 0);
    assert(ov[7].warned);
    assert(romfs_read(buf, sizeof buf, ov[7].start) == (ssize_t)sizeof buf);

    pthread_mutex_lock(&g_ov_lock);
    ov_fds_close();
    pthread_mutex_unlock(&g_ov_lock);
    assert(fs_overlay_open_fds() == 0 && open_fds() <= before);
    puts("RomFS replacement files: open files bounded, reads correct, unopenable files read as zeros; a save with no "
         "descriptors left is dropped cleanly - passed");
}
