// Production hooks: reject other titles before touching pointers or seed state,
// and retain file-load/spawn behavior for both LAS versions. No game assets.
#include <assert.h>
#include "../runtime/hle.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/hle/enemy_random.c"

static TitleProfile profile;
static const char *seed;
static unsigned seed_reads;
bool title_is_las(void) { return profile.las_extensions; }
const char *rando_seed_dir(void) { ++seed_reads; return seed; }
uint64_t renderer_present_count(void) { return 0; }
#ifdef _WIN32
ssize_t pread(int fd, void *buf, size_t size, off_t offset) {
    if (_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    return _read(fd, buf, (unsigned)size);
}
#endif

static void reject_other_titles(void) {
    const char *keys[] = {"", "acnh", "pokemon_za", "future-title"};
    unsigned reads = seed_reads;
    int loaded = g_loaded, slots = g_nslots;
    uint64_t rng = g_rng;
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; ++i) {
        profile = (TitleProfile){.key=keys[i], .version="1.0.0"};
        enemy_random_filter((const char *)(uintptr_t)1, 0, (uint8_t *)(uintptr_t)1, REC);
        rt_enemy_spawn(1);
        assert(seed_reads == reads && g_loaded == loaded && g_nslots == slots && g_rng == rng);
    }
}

int main(int argc, char **argv) {
    assert(argc == 2); seed = argv[1];
    reject_other_titles();
    char host[1024]; snprintf(host, sizeof host, "%s/romfs/Room/test.leb", seed);
    for (int version = 0; version < 2; ++version) {
        profile = (TitleProfile){.key="las", .version=version ? "1.1.0" : "1.0.0", .las_extensions=true};
        uint8_t record[REC] = {0}, spawn[0x50] = {0};
        int fd = open(host, O_RDONLY); assert(fd >= 0);
        assert(pread(fd, record, REC, 0) == REC); close(fd);
        enemy_random_filter(host, 0, record, REC);
        assert(g_loaded && g_nslots == 1 && LD16((uintptr_t)record + 0xC) == 2);
        assert(LD32((uintptr_t)record + 0x38) == 7 && record[0x84] == 1);
        memcpy(spawn, record, 8);
        rt_enemy_spawn((uintptr_t)spawn);
        assert(LD16((uintptr_t)spawn + 8) == 2 && LD32((uintptr_t)spawn + 0x10) == 7);
        assert(!g_slots[0].fresh);
        rt_enemy_spawn((uintptr_t)spawn); // retained room: re-roll on the next spawn
        assert(LD16((uintptr_t)spawn + 8) == 2);
        reject_other_titles(); // also reject with a populated LAS seed cache
        enemy_random_reset();
        assert(!g_loaded && !g_nslots && !g_files && !g_slots && !g_keys);
    }
    puts("PASS: enemy randomization stays inside LAS and works for both executable profiles");
}
