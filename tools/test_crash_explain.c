#include <assert.h>
#include <stdlib.h>
#include "../runtime/crash_explain.c"

static TitleProfile selected;
static bool no_profile, gpu_fault, guest_fault;
const TitleProfile *title_profile_current(void) { return no_profile ? NULL : &selected; }
bool plat_module_name(uint64_t pc, char *out, size_t n) {
    if (!gpu_fault) return false;
    snprintf(out, n, "igc64.dll");
    return true;
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) { return guest_fault ? "guest_function" : NULL; }

static void check(unsigned minimum, int actual, const char *fatal, const char *expected) {
    selected.minimum_files = minimum;
    rt_romfs_files = actual;
    g_line[0] = 0; g_pc = 0;
    if (fatal) rt_fatal_msg("%s\n", fatal);
    if (gpu_fault || guest_fault) rt_crash_fault("signal 11", 0, 1);
    char out[1024];
    rt_crash_explain(out, sizeof out, "test.log");
    assert(strstr(out, expected));
    assert(strstr(out, "test.log"));
    if (actual < 0 || !minimum || (unsigned)actual >= minimum)
        assert(!strstr(out, "game files are incomplete"));
}

int main(void) {
    // Every supported adapter's own threshold, plus an unknown future title.
    const unsigned minimums[] = {3836, 3836, 66790, 22, 7};
    for (unsigned i = 0; i < sizeof minimums / sizeof *minimums; i++) {
        unsigned n = minimums[i];
        check(n, n - 1, "no function at 7001234000", "game files are incomplete");
        check(n, n, "no function at 7001234000", "Game code is not supported yet");
        check(n, n + 1, "no function at 7001234000", "Game code is not supported yet");
    }
    check(66790, -1, "no function at 7001234000", "Game code is not supported yet");
    check(0, 0, "no function at 7001234000", "Game code is not supported yet");
    no_profile = true;
    check(0, 22, "no function at 7001234000", "Game code is not supported yet");
    no_profile = false;
    check(22, 22, "library built for another version", "game code is from another version");
    check(22, 22, "no font for the game's text", "No font for the game's text");
    check(22, 22, "get_terminate", "error it couldn't recover from");
    check(22, 22, "Vulkan error", "Graphics driver error");
    check(22, 22, "cannot allocate", "Out of memory");
    gpu_fault = true; check(22, 22, NULL, "Your graphics driver crashed"); gpu_fault = false;
    guest_fault = true; check(22, 22, NULL, "The game crashed"); guest_fault = false;
    check(22, 22, "unclassified stop", "LAS Recomp stopped because of an error");
    puts("PASS: adapter-aware crash explanations and existing failure categories");
    return 0;
}
