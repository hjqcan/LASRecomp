// A save that can't be written never reaches the game as an error (its SDK aborts on any failed fs call) and never
// touches the committed save: the failed save is dropped whole, the player is warned once, the next save works.
#include "test_host_fs.h"

static char path[1200], pending[1250];

// open slot.bin for writing, write bytes, close; every step must succeed for the game
static void save(Service *mount, const void *bytes, size_t n) {
    IpcResp opened = open_file(mount, "/slot.bin", 2); assert(!opened.result && opened.nobjects == 1);
    assert(!write_file(opened.objects[0], bytes, n, 0)); close_file(opened.objects[0]);
}
static void assert_clean(HostFs *fs) { assert(!fs->npending && access(pending, F_OK)); }

int main(int argc, char **argv) {
    assert(argc == 2);
#ifdef _WIN32
    _set_fmode(_O_BINARY);
#endif
    HostFs fs = {.journaled=true}; strlcpy(fs.root, argv[1], sizeof fs.root);
    Service mount = {.state=&fs};
    char other[1200];
    snprintf(path, sizeof path, "%s/slot.bin", argv[1]);
    snprintf(pending, sizeof pending, "%s.pending", path);
    put_file(path, 0x11, 32);

    // Staging fails (the working copy's path is blocked / the journal is full): the game still gets a file that takes
    // its writes and reads them back, the Commit succeeds, and the committed save is untouched.
    assert(!mkdir(pending, 0755));
    IpcResp opened = open_file(&mount, "/slot.bin", 2); assert(!opened.result);
    assert(((HostFile *)opened.objects[0]->state)->scratch && notices == 1);
    assert(!write_file(opened.objects[0], "s", 1, 0));
    char got = 0;
    assert(pread(((HostFile *)opened.objects[0]->state)->fd, &got, 1, 0) == 1 && got == 's');
    close_file(opened.objects[0]);
    assert(!commit_mount(&mount) && first_byte(path) == 0x11 && !fs.npending);
    assert(!rmdir(pending));
    fs.npending = PENDING_MAX;
    opened = open_file(&mount, "/slot.bin", 2); assert(!opened.result);
    assert(((HostFile *)opened.objects[0]->state)->scratch);
    assert(!write_file(opened.objects[0], "t", 1, 0)); close_file(opened.objects[0]);
    assert(first_byte(path) == 0x11);
    fs.npending = 0;
    // A missing file is still "path not found" (the game creates it first) and leaves nothing behind.
    assert(open_file(&mount, "/missing.bin", 2).result == FS_PATH_NOT_FOUND);
    snprintf(other, sizeof other, "%s/missing.bin.pending", argv[1]);
    assert(access(other, F_OK));

    // Short and interrupted writes complete the whole buffer.
    const char bytes[] = "a complete save write";
    written = 0; write_limit = 3; write_interrupts = 2;
    save(&mount, bytes, sizeof bytes);
    assert(written == sizeof bytes && !write_interrupts && first_byte(path) == 0x11);
    write_limit = 0;
    assert(!commit_mount(&mount) && first_byte(path) == 'a');
    assert_clean(&fs);

    // A failed write (disk full after a short write, a zero-byte write) or flush: the game is told it worked, the
    // Commit drops that save and the committed one stays.
    written = 0; write_fail_after = 4;
    save(&mount, "bbbbbbbbbb", 10); write_fail_after = -1;
    assert(!commit_mount(&mount) && first_byte(path) == 'a'); assert_clean(&fs);
    write_zero = 1; save(&mount, "c", 1); write_zero = 0;
    assert(!commit_mount(&mount) && first_byte(path) == 'a'); assert_clean(&fs);
    opened = open_file(&mount, "/slot.bin", 2); assert(!opened.result);
    assert(!write_file(opened.objects[0], "d", 1, 0));
    IpcReq flush = {.cmd=2}; IpcResp response = {0};
    sync_error = 1; file(opened.objects[0], &flush, &response); sync_error = 0;
    assert(!response.result); close_file(opened.objects[0]);
    assert(!commit_mount(&mount) && first_byte(path) == 'a'); assert_clean(&fs);

    // The Commit itself fails (syncing the working copy): success for the game, committed save kept, copy dropped.
    save(&mount, "e", 1);
    sync_error = 1; assert(!commit_mount(&mount)); sync_error = 0;
    assert(first_byte(path) == 'a'); assert_clean(&fs);

    // One file's replace fails: the other file of the same Commit still lands.
    save(&mount, "f", 1);
    snprintf(other, sizeof other, "%s/other.bin", argv[1]); put_file(other, 'x', 32);
    opened = open_file(&mount, "/other.bin", 2); assert(!opened.result);
    assert(!write_file(opened.objects[0], "y", 1, 0)); close_file(opened.objects[0]);
    assert(!unlink(path) && !mkdir(path, 0755));
    assert(!commit_mount(&mount) && first_byte(other) == 'y'); assert_clean(&fs);
    assert(!rmdir(path)); put_file(path, 'a', 32);
#ifdef _WIN32
    save(&mount, "g", 1);  // (a reader holding the file blocks the replace)
    HANDLE locked = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(!commit_mount(&mount) && first_byte(path) == 'a'); assert_clean(&fs);
    CloseHandle(locked);
#endif
    // Once the problem is gone, the next save is committed as usual.
    save(&mount, "h", 1);
    assert(!commit_mount(&mount) && first_byte(path) == 'h'); assert_clean(&fs);
    assert(notices == 1);  // (one warning per run)

    // Read errors and invalid read requests are not successful empty reads.
    char actual[8];
    HostFile bad = {.fd=-1}; Service bad_file = {.state=&bad};
    uint64_t args[3] = {0, 0, sizeof actual};
    IpcReq read = {.cmd=0, .data=(const uint8_t *)args, .nb=1};
    read.b[0] = (IpcBuf){(uintptr_t)actual, sizeof actual}; response = (IpcResp){0};
    file(&bad_file, &read, &response); assert(response.result);
    args[1] = (uint64_t)-1; response = (IpcResp){0};
    opened = open_file(&mount, "/slot.bin", 1); assert(!opened.result);
    file(opened.objects[0], &read, &response); assert(response.result == MAKE_RESULT(2, 6001));
    close_file(opened.objects[0]);

    // Non-journaled mounts (SD card) keep their in-place writes and still report write errors.
    fs.journaled = false;
    opened = open_file(&mount, "/slot.bin", 2); assert(!opened.result);
    assert(!write_file(opened.objects[0], "i", 1, 0));
    write_fail_after = 0; written = 0;
    assert(write_file(opened.objects[0], "j", 1, 0)); write_fail_after = -1;
    close_file(opened.objects[0]);
    assert(first_byte(path) == 'i' && !fs.npending && !commit_mount(&mount));
    puts("Save errors: staging, short writes, write/flush/commit failures (game told success, committed save kept), "
         "recovery and SD behavior passed");
}
