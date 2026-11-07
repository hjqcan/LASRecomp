// LAS Recomp entry: load modules, set up the kernel, run nn::init::Start on the main thread.
#include <SDL3/SDL.h>
#ifdef __ANDROID__
#include <SDL3/SDL_main.h>  // (main() becomes SDL_main, which SDL's activity runs)
#endif
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "game_addresses.h"
#include "gamelib.h"
#include "paths.h"
#include "settings.h"
#include "hle.h"
#include "kernel.h"
#include "rando.h"

const char *g_romfs_dir;

// rtld normally passes these to nn::init::Start: run every module's init/fini arrays.
static void run_array(Cpu *c, uint64_t arr, uint64_t size) {
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t fn = ((uint64_t *)arr)[i];
        if (fn) guest_call(c, fn);
    }
}

// nn::init::Start calls x2 right after nnosInitialize and x3 after nninitStartup, just before
// nnMain: SDK constructors run early, the game's run once nninitStartup has set up its heap.
// rtld runs a module's DT_INIT, which registers it (and its TLS template) with musl via
// __nnmusl_init_dso and then runs its init_array; fall back to the array if there is no DT_INIT.
static void module_init(Cpu *c, Module *m) {
    LOG("init %s (DT_INIT %llx, %llu init_array entries)\n", m->name, m->init, m->init_array_sz / 8);
    if (m->init) guest_call(c, m->init);
    else run_array(c, m->init_array, m->init_array_sz);
}

static void rtld_init_sdk(Cpu *c) { module_init(c, &g_modules[1]); }
static void rtld_init_main(Cpu *c) { module_init(c, &g_modules[0]); }

// Claim the guest address range before anything else can map there. The host allocator places
// regions at random addresses in the same window; on a collision, re-exec for a fresh layout.
// LARECOMP_REEXEC_FORCE=1 (test): the first process skips the reservation once and starts again, as when the range is
// taken (the new process has LARECOMP_REEXEC set and reserves normally).
#define REEXEC_FORCED "forced by LARECOMP_REEXEC_FORCE"
#ifdef LARECOMP_DYN_BASE
uint64_t g_guest_lo = GUEST_CANON;
// The window's start (rt.h). LARECOMP_GUEST_BASE=<hex> (test) asks for one; otherwise a start other than GUEST_CANON
// first, so a desktop DYN_BASE build always runs the game away from its own addresses. The game was made for a 39-bit
// address space, so the window ends below 2^39; 4 GB aligned keeps ADRP's page offsets the same as the game's.
#ifdef __linux__
// The lowest 4 GB-aligned start above 4 GB whose window fits below 2^39 in a gap of /proc/self/maps (0: none). On
// Android the process is forked from the zygote with its libraries already mapped, and can't re-exec for another
// layout. From LASRecompAndroid by mamcjr (MIT), android/common/guest_window.c.
static uint64_t free_window(uint64_t size) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    uint64_t prev_end = 0x100000000ull, found = 0;
    char line[512];
    for (bool more = true; more && !found;) {
        unsigned long long lo = 1ull << 39, hi = 0;
        more = fgets(line, sizeof line, f) && sscanf(line, "%llx-%llx", &lo, &hi) == 2;
        uint64_t start = (prev_end + 0xFFFFFFFFull) & ~0xFFFFFFFFull;
        if (start + size <= (more ? lo : 1ull << 39) && start + size <= (1ull << 39)) found = start;
        if (more && hi > prev_end) prev_end = hi;
    }
    fclose(f);
    return found;
}
#endif
static bool reserve_dyn_window(void) {
    uint64_t want[4] = {0, 0, GUEST_CANON + 0x100000000ull, GUEST_CANON};  // (macOS: room up to ~0x77_2000_0000)
    const char *e = getenv("LARECOMP_GUEST_BASE");
    if (e && *e) want[0] = strtoull(e, NULL, 16);
#ifdef __linux__
    want[1] = free_window(GUEST_SIZE);
#endif
    for (int i = 0; i < 4; i++) {
        uint64_t lo = want[i];
        if (!lo || (lo & 0xFFFFFFFFull) || lo + GUEST_SIZE > (1ull << 39)) continue;
        if (plat_reserve_guest(lo, lo + GUEST_SIZE)) { g_guest_lo = lo; return true; }
    }
    return false;
}
#endif
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    const char *force = getenv("LARECOMP_REEXEC_FORCE");
    bool forced = force && *force && strcmp(force, "0") && !getenv("LARECOMP_REEXEC");
    if (forced) setenv("LARECOMP_REEXEC_WHY", REEXEC_FORCED, 1);
#ifdef LARECOMP_DYN_BASE
    else if (reserve_dyn_window()) return;
#else
    else if (plat_reserve_guest(GUEST_LO, GUEST_HI)) return;
#endif
    else setenv("LARECOMP_REEXEC_WHY", "", 0);  // (the platform may have said what holds the range; else nothing more)
    const char *tries = getenv("LARECOMP_REEXEC");
    int n = tries ? atoi(tries) : 0;
    if (n >= 20) {
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
        exit(1);
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("LARECOMP_REEXEC", buf, 1);
    plat_exec(NULL);
    perror("re-exec");
    exit(1);
}

// ------------------------------------------------------------------ boot / in-place reboot
// Switching seeds (or vanilla) and going back to the start screen reboot the game inside this process: the window,
// GPU device, settings and shader caches stay. rt_request_reboot (menu, any thread) makes ui_run return on the
// main thread, which then: stops every guest thread (kernel_stop_threads: each unwinds at its next svc / HLE call /
// wait), resets the renderer (textures, address-keyed caches) and all per-boot host state (RT_ON_RESET hooks:
// services, HLE features, seed data, audio), empties the kernel, wipes guest memory, and boots again.
static _Atomic int g_reboot;
void rt_request_reboot(int kind) {
    LOG("[reboot] requested (%s)\n", kind == RT_REBOOT_TITLE ? "start screen" : "boot");
    g_reboot = kind;
}
int rt_reboot_requested(void) { return g_reboot; }

// LARECOMP_REBOOT_AT=<frame>:<seed | vanilla | title>[,...] (test): at that presented frame, switch to the seed /
// vanilla in place, or go back to the start screen (with LARECOMP_LAUNCHER_AUTOPLAY it plays the saved choice).
// Frames count on across reboots. Called by the presenter every frame.
void reboot_test_frame(uint64_t frame) {
    static const char *next;
    static int init;
    if (!init) { init = 1; next = getenv("LARECOMP_REBOOT_AT"); }
    long at;
    char what[128];
    if (!next || !*next || sscanf(next, "%ld:%127[^,]", &at, what) != 2 || (long)frame < at) return;
    next = strchr(next, ',');
    if (next) next++;
    if (!strcmp(what, "title")) { rt_request_reboot(RT_REBOOT_TITLE); return; }
    // (a test run: the next boot's seed without saving it - larecomp.ini is the player's choice)
    strlcpy(g_settings.rando_seed, strcmp(what, "vanilla") ? what : "", sizeof g_settings.rando_seed);
    unsetenv("LARECOMP_RANDO_SEED");
    rt_request_reboot(RT_REBOOT_GAME);
}

#ifdef __APPLE__
extern const ResetFn __start_resets[] __asm__("section$start$__DATA$__rt_reset");
extern const ResetFn __stop_resets[] __asm__("section$end$__DATA$__rt_reset");
#elif defined(__linux__)
extern const ResetFn __start_resets[], __stop_resets[];  // (ELF: defined by the linker)
#else
// PE/COFF: .rst$a < .rst$m (entries, hle.h) < .rst$z (the linker sorts by the text after '$')
__attribute__((section(".rst$a"), used)) static const ResetFn __start_resets[1];
__attribute__((section(".rst$z"), used)) static const ResetFn __stop_resets[1];
#endif

void backend_reset(void);             // renderer: GPU idle, game textures and address-keyed caches dropped
void backend_guest_memory_reset(void);  // renderer: guest memory was replaced (re-wrap the heap)
void backend_clear_window(void);      // a black frame while the game is down
void backend_quit(void);              // quitting: the render thread stopped, the GPU idle, caches kept (before exit())

static void reboot_idle(void) {  // (main thread, while guest threads stop) keep the window alive
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); backend_quit(); exit(0); }
}

static void teardown(void) {
    uint64_t t0 = plat_now_ns();
    kernel_stop_threads(reboot_idle);
    backend_clear_window();
    backend_reset();
    const ResetFn *start = __start_resets, *stop = __stop_resets;
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    int n = 0;
    for (const ResetFn *p = start; p < stop; p++)
        if (*p) { (*p)(); n++; }
    kernel_reset();
    if (!plat_reset_guest(GUEST_LO, GUEST_HI)) FATAL("cannot reset guest memory\n");
    backend_guest_memory_reset();
    g_nmodules = 0;
    g_reboot = RT_REBOOT_NONE;
    LOG("[reboot] game torn down in %llums (%d reset hooks)\n", (unsigned long long)((plat_now_ns() - t0) / 1000000), n);
}

static void boot(const char *root) {
    char exefs[1024], romfs[1024], npdm_path[1024];
    snprintf(exefs, sizeof exefs, "%s/exefs", root);
    snprintf(romfs, sizeof romfs, "%s/romfs", root);
    snprintf(npdm_path, sizeof npdm_path, "%s/main.npdm", exefs);
    if (!g_romfs_dir) g_romfs_dir = strdup(romfs);

#ifdef __linux__
    {   // the window and graphics device first (normally made by the start screen or ui_run), so the guest heap can be
        // the driver's memory before any game thread runs (runtime/nvn/vulkan/exec.c backend_guest_heap_prepare)
        extern void ui_window_init(void);
        extern void backend_guest_heap_prepare(void);
        ui_window_init();
        backend_guest_heap_prepare();
    }
#endif
    char why[512];
    if (!game_version_select(root, why, sizeof why)) FATAL("%s\n", why);  // (the address table: game_addresses.h)
    if (!gamelib_load(root, why, sizeof why)) FATAL("%s\n", why);
    loader_load(exefs);
    rando_boot();  // the chosen seed's data patches go into the freshly loaded module
    { extern void settings_load(void); settings_load(); }
    kernel_init();
    dispatch_init();

    uint32_t stack_size = 0x100000;
    int prio = 44;
    FILE *f = fopen(npdm_path, "rb");
    if (f) {
        uint8_t meta[0x80];
        if (fread(meta, 1, sizeof meta, f) == sizeof meta && !memcmp(meta, "META", 4)) {
            prio = meta[0xE];
            stack_size = *(uint32_t *)(meta + 0x1C);
        }
        fclose(f);
    }
    LOG("main thread: prio %d, stack 0x%x\n", prio, stack_size);

    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    if (!start) FATAL("nn::init::Start not found\n");
    uint64_t init_fn = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint64_t fini_fn = hle_register("<rtld_init_main>", rtld_init_main);
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    kernel_run_main(t, t->handle, 0, init_fn, fini_fn);  // nn::init::Start(thread_handle, args_region, init, fini)
}

// The log's first line after the log's own: this build and the machine, for bug reports. Nothing that identifies the
// player (platform.h plat_system_info). Once per session: a restart of this process (the start screen re-executing
// itself for the game) appends to the same log and inherits LASRECOMP_INFO_LOGGED.
static void log_startup_info(void) {
    if (getenv("LASRECOMP_INFO_LOGGED")) return;
    setenv("LASRECOMP_INFO_LOGGED", "1", 1);
    extern const char *larecomp_version(void);  // runtime/ui/launcher.mm
    PlatSysInfo si = {0};
    plat_system_info(&si);
    char cores[96] = "";
    if (si.cores > 0) {
        int n = snprintf(cores, sizeof cores, " %d cores", si.cores);
        if (si.cores_detail[0]) n += snprintf(cores + n, sizeof cores - n, " (%s)", si.cores_detail);
        if (si.threads > si.cores) snprintf(cores + n, sizeof cores - n, " / %d threads", si.threads);
    } else if (si.threads > 0) snprintf(cores, sizeof cores, " %d threads", si.threads);
    unsigned long long tenths = (si.ram * 10 + (1ull << 29)) >> 30;  // (GB to a tenth; whole when it is: 64 GB, 14.5 GB)
    char ram[32] = "RAM unknown";
    if (si.ram && tenths % 10) snprintf(ram, sizeof ram, "%llu.%llu GB RAM", tenths / 10, tenths % 10);
    else if (si.ram) snprintf(ram, sizeof ram, "%llu GB RAM", tenths / 10);
    fprintf(stderr, "[info] LAS Recomp %s, %s, %s%s, %s%s%s\n", larecomp_version(), si.os[0] ? si.os : "unknown OS",
            si.cpu[0] ? si.cpu : "unknown CPU", cores, ram, si.package ? ", " : "", si.package ? si.package : "");
}

#ifdef __APPLE__
// The Metal HUD setting (Video tab): Apple's HUD library loads only when MTL_HUD_ENABLED=1 is set as the process starts
// (macOS 27: setting it later in the process, or the layer's developerHUDProperties alone, do nothing), so with the
// setting on the app starts itself again with it, before the session log opens. From then on the setting shows and
// hides the HUD at once (runtime/nvn/metal.m ui_metal_hud_apply); with it off at launch, turning it on needs a restart.
static void metal_hud_relaunch(void) {
    if (getenv("MTL_HUD_ENABLED")) return;  // (set: by us, or by hand)
    char path[1100], dir[900];
    const char *e = getenv("LARECOMP_SETTINGS");
    if (e && *e) snprintf(path, sizeof path, "%s", e);
    else if (rt_data_root_early(dir, sizeof dir)) snprintf(path, sizeof path, "%s/larecomp.ini", dir);
    else snprintf(path, sizeof path, "larecomp.ini");  // (dev mode: the starting directory)
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    bool on = false;
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, " metal_hud = %d", &v) == 1) on = v != 0;
    }
    fclose(f);
    if (!on) return;
    setenv("MTL_HUD_ENABLED", "1", 1);
    setenv("LARECOMP_METAL_HUD", "1", 1);  // (ui_metal_hud_apply: the HUD was loaded for the setting)
    plat_exec(NULL);
    perror("metal hud re-exec");  // (failed: the game starts without the HUD)
}
#endif

int main(int argc, char **argv) {
#ifdef __APPLE__
    metal_hud_relaunch();
#endif
    plat_init();
#ifdef LARECOMP_DYN_BASE
    LOG("[boot] guest window %llx-%llx (moving: rt.h)\n", (unsigned long long)GUEST_LO, (unsigned long long)GUEST_HI);
#endif
    log_startup_info();
    if (getenv("LARECOMP_REEXEC")) {  // (reserve_guest_space started this process again)
        const char *why = getenv("LARECOMP_REEXEC_WHY"), *n = getenv("LARECOMP_REEXEC");
        if (!why) LOG("[boot] started again (try %s) (LARECOMP_REEXEC set by the caller)\n", n);
        else if (!strcmp(why, REEXEC_FORCED)) LOG("[boot] started again (try %s): %s\n", n, why);
        else LOG("[boot] started again (try %s): the guest address range was in use%s%s\n", n, *why ? ": " : "", why);
    }
#ifdef _WIN32
    setvbuf(stderr, NULL, _IONBF, 0);  // the Windows CRT treats _IOLBF as full buffering
#else
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    const char *root = "extracted";  // (in the data root)
    // LARECOMP_NO_LAUNCHER=1 (tools/game_slot.sh sets it for every test run): never the start screen - it can
    // delete game files, and a test's extracted/ may be a link to someone else's. Where it would come up (a forced
    // start screen, "Title screen" in game) the app exits instead. =0: off (start screen tests, scratch data root).
    const char *nl = getenv("LARECOMP_NO_LAUNCHER");
    bool no_launcher = nl && *nl && strcmp(nl, "0");
    bool skip_launcher = no_launcher || getenv("LARECOMP_INPUT");  // tests boot straight in
    if (getenv("LARECOMP_FORCE_LAUNCHER")) skip_launcher = false;  // (test: the start screen, then the scripted input)
    static char arg_root[4096];  // (>= PATH_MAX: Bionic's fortified realpath checks it)
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) skip_launcher = true;
        else if (argv[i][0] != '-' && !realpath(argv[i], arg_root)) root = argv[i];  // (relative to where we started)
        else if (argv[i][0] != '-') root = arg_root;
    }
    rt_paths_init();  // the data root becomes the working directory (runtime/paths.h)
    if (getenv("LARECOMP_MENU_SFX_TEST")) {  // (test: the menu sounds loaded now, each logged)
        extern void menu_sfx_set_game_dir(const char *dir);
        extern void menu_sfx_load_now(void);
        menu_sfx_set_game_dir(root);
        menu_sfx_load_now();
    }
    for (bool title = !skip_launcher;;) {
        if (title && no_launcher) {
            fprintf(stderr, "[main] LARECOMP_NO_LAUNCHER is set: not showing the start screen - exiting\n");
            exit(3);
        }
        if (title) {  // start screen (runtime/ui/launcher.mm): game files setup, seed choice, settings
            extern void launcher_run(const char *root);
            launcher_run(root);
        }
        boot(root);
        int kind = ui_run();  // window and input on the main thread; returns when a reboot is requested
        teardown();
        title = kind == RT_REBOOT_TITLE;
    }
}
