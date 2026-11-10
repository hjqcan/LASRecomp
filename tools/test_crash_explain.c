// Which crashes the dialog blames on the graphics driver (runtime/crash_explain.c, #648): no game, no driver.
// Build + run: clang -std=gnu11 -O1 tools/test_crash_explain.c -o build/test-crash-explain && build/test-crash-explain
#include "../runtime/crash_explain.c"

static const char *g_module;  // what the crash's pc belongs to

bool plat_module_name(uint64_t pc, char *out, size_t n) {
    (void)pc;
    snprintf(out, n, "%s", g_module);
    return true;
}
// The game code is the one module the dialog also knows by address.
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) {
    (void)pc, (void)offset;
    return strstr(g_module, "lasgame") ? "game_fn" : NULL;
}
const GameVersion *game_version(void) { return NULL; }
uint64_t game_romfs_bytes(const GameVersion *v) { (void)v; return 0; }

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

static int fails;

static bool driver_crash(const char *module, char *dialog, size_t n) {
    g_module = module;
    g_line[0] = 0, g_pc = 0;
    rt_crash_fault("signal 11", 0x10, 0x1234);
    rt_crash_explain(dialog, n, NULL);
    return rt_fatal_is_driver();
}

static void check(const char *module, bool driver) {
    char dialog[1600];
    bool got = driver_crash(module, dialog, sizeof dialog);
    bool says = !strncmp(dialog, "Your graphics driver crashed", 28);
    if (got != driver || says != driver) {
        fails++;
        printf("FAIL %s: rt_fatal_is_driver %d, dialog \"%.40s\", expected driver %d\n", module, got, dialog, driver);
    }
}

int main(void) {
    for (size_t i = 0; i < sizeof drivers / sizeof *drivers; i++) check(drivers[i], true);
    for (size_t i = 0; i < sizeof not_drivers / sizeof *not_drivers; i++) check(not_drivers[i], false);
    char dialog[1600];
    driver_crash("C:\\Users\\Rigdon\\LAS Recomp\\extracted\\lasgame.dll", dialog, sizeof dialog);
    if (strncmp(dialog, "The game crashed", 16)) fails++, printf("FAIL game code under Rigdon: \"%.40s\"\n", dialog);
    printf("%s\n", fails ? "crash explain: FAILED" : "crash explain: all passed");
    return fails != 0;
}
