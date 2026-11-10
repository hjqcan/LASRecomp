#include <assert.h>
#include <stdlib.h>
#include "../runtime/crash_explain.c"

static TitleProfile selected;
static bool no_profile, gpu_fault, guest_fault;
static bool las;
static const char *module_name;
static const char *const drivers[] = {
    "/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so",
    "/usr/lib/aarch64-linux-gnu/libvulkan_panfrost.so",
    "/usr/lib/aarch64-linux-gnu/libvulkan_freedreno.so",
    "/usr/lib/aarch64-linux-gnu/libvulkan_broadcom.so",
    "/usr/lib/aarch64-linux-gnu/libvulkan_asahi.so",
    "/usr/lib/aarch64-linux-gnu/libmali.so",
    "/vendor/lib64/egl/libGLES_mali.so",
    "/vendor/lib64/hw/vulkan.adreno.so",
    "/vendor/lib64/hw/vulkan.mali.so",
    "C:\\Windows\\System32\\DriverStore\\FileRepository\\iigd_dch.inf_amd64\\igd12umd64.dll",
    "C:\\Windows\\System32\\nvoglv64.dll",
    "/System/Library/Extensions/AGXMetalG13X.bundle/Contents/MacOS/AGXMetalG13X",
};
static const char *const not_drivers[] = {
    "C:\\Users\\Rigdon\\Desktop\\LAS Recomp\\extracted\\lasgame.dll",
    "D:\\Games\\BigDrive\\LAS Recomp\\extracted\\lasgame.dll",
    "/home/rigdon/libvulkan_builds/lasgame.so",
    "/mnt/vulkan.games/lasgame.so",
    "C:\\Users\\Sam\\Desktop\\LAS Recomp\\extracted\\lasgame.dll",
    "/home/rigdon/LAS Recomp/lasrecomp",
    "/usr/lib/libvulkan.so.1",           // the Vulkan loader, not a driver
    "/system/lib64/libvulkan.so",
    "C:\\Windows\\System32\\vulkan-1.dll",
};


bool title_is_las(void) { return las; }
static GameVersion version={.key="v1_0_0"};
const GameVersion *game_version(void) { return las ? &version : NULL; }
uint64_t game_romfs_bytes(const GameVersion *v) { return v ? 100 : 0; }
const TitleProfile *title_profile_current(void) { return no_profile ? NULL : &selected; }
bool plat_module_name(uint64_t pc, char *out, size_t n) {
    if (module_name) { snprintf(out, n, "%s", module_name); return true; }
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
    check(22, 22, "library built for another version", "game code was built for other game files");
    check(22, 22, "no font for the game's text", "No font for the game's text");
    check(22, 22, "get_terminate", "error it couldn't recover from");
    check(22, 22, "Vulkan error", "Graphics driver error");
    check(22, 22, "cannot allocate", "Out of memory");
    gpu_fault = true; check(22, 22, NULL, "Your graphics driver crashed"); gpu_fault = false;
    guest_fault = true; check(22, 22, NULL, "The game crashed"); guest_fault = false;
    check(22, 22, "unclassified stop", "Multi stopped because of an error");
    las = true; rt_romfs_bytes = 99;
    check(3836, 3836, "unclassified stop", "game files are damaged or changed");
    rt_romfs_bytes = 100; rt_romfs_missing = 1;
    check(3836, 3836, "unclassified stop", "game files are damaged or changed");
    las = false;
    check(22, 22, "no function at 7001234000", "Game code is not supported yet");
    rt_romfs_missing = 0;
    puts("PASS: adapter-aware crash explanations and existing failure categories");
    for (unsigned i = 0; i < sizeof drivers / sizeof *drivers; i++) {
        module_name = drivers[i];
        g_line[0] = 0; g_pc = 0;
        rt_crash_fault("signal 11", 0x10, 0x1234);
        assert(rt_fatal_is_driver());
    }
    for (unsigned i = 0; i < sizeof not_drivers / sizeof *not_drivers; i++) {
        module_name = not_drivers[i];
        g_line[0] = 0; g_pc = 0;
        rt_crash_fault("signal 11", 0x10, 0x1234);
        assert(!rt_fatal_is_driver());
    }
    module_name = NULL;
    return 0;
}
