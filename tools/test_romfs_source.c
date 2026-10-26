// Exercise the production RomFS opener and reads with synthetic folder/image data.
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
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

const char *g_romfs_dir;
static TitleProfile profile;
static jmp_buf failed;
static char fatal[2048];
const TitleProfile *title_profile_current(void) { return &profile; }
void rt_fatal_msg(const char *fmt, ...) {
    va_list args; va_start(args, fmt); vsnprintf(fatal, sizeof fatal, fmt, args); va_end(args);
}
void rt_fatal_trace(void) { longjmp(failed, 1); }
#ifdef _WIN32
ssize_t pread(int fd, void *buf, size_t size, off_t offset) {
    if (_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    return _read(fd, buf, (unsigned)size);
}
#endif

int main(int argc, char **argv) {
    assert(argc == 5);
#ifdef _WIN32
    _fmode = _O_BINARY;  // match plat_init: RomFS bytes must not undergo CRT text conversion
#endif
    g_romfs_dir = argv[1];
    const bool las = !strcmp(argv[2], "las");
    profile = (TitleProfile){.key=argv[2], .version="1.0.0", .minimum_files=2,
        .romfs_root_count=1, .image_satisfies_count=!las, .las_extensions=las};
    profile.romfs_roots[0] = las ? "region_common" : !strcmp(argv[2], "acnh") ? "Movie" : "Data";
    bool reject = !strcmp(argv[3], "reject");
    if (setjmp(failed)) {
        fprintf(stderr, "%s", fatal);
        assert(reject);
        assert(strstr(fatal, "cannot read the game data"));
        assert(strstr(fatal, ".bin"));
        assert(strstr(fatal, argv[4]));
        return 0;
    }
    base_open();
    assert(!reject);
    unsigned char header[8] = {0};
    assert(base_pread(header, sizeof header, 0) == sizeof header && header[0] == 0x50);
    if (!strcmp(argv[3], "image")) {
        assert(g_base_fd >= 0);
        char data[8];
        assert(base_pread(data, sizeof data, 0x1000) == sizeof data);
        assert(!memcmp(data, "original", 8));
    } else {
        assert(g_base_fd == -1 && g_base.nfiles == 2);
        for (int i = 0; i < g_base.nfiles; ++i) {
            char data[32] = {0};
            assert(g_base.files[i].size < sizeof data);
            assert(base_pread(data, g_base.files[i].size, g_base.files[i].start) == g_base.files[i].size);
            const char *name = strrchr(g_base.files[i].path, '/') + 1;
            assert(!strcmp(data, !strcmp(name, "a.bin") ? "movie-payload" : "sound-payload"));
        }
    }
    base_close();
    assert(g_base_fd == -1 && !g_base_open);
    puts("PASS: production RomFS source and data reads");
    return 0;
}
