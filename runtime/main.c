// LAS Recomp entry: load modules, set up the kernel, run nn::init::Start on the main thread.
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gamelib.h"
#include "gameversion.h"
#include "game_profiles.h"
#include "game_shortcut.h"
#include "game_install.h"
#include "mods.h"
#include "photo.h"
#include "workshop.h"
#include "performance.h"
#include "research.h"
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

static void rtld_init_sdk(Cpu *c) { module_init(c, &g_modules[title_profile_current()->init_order[0]]); }
static void rtld_init_main(Cpu *c) {
    const TitleProfile *p=title_profile_current();
    for (unsigned i=1;i<p->module_count;i++) module_init(c,&g_modules[p->init_order[i]]);
}

// Newer SDKs accept a third callback, invoked after nnMain returns and before
// nninitFinalizeSdkModule/nnosQuickExit. Module DT_FINI owns its fini array, just
// as DT_INIT owns the constructor array; the fallback walks it in reverse.
static void rtld_fini_modules(Cpu *c) {
    const TitleProfile *p = title_profile_current();
    for (unsigned i = p->module_count; i > 0; i--) {
        Module *m = &g_modules[p->init_order[i - 1]];
        if (m->fini) guest_call(c, m->fini);
        else for (uint64_t j = m->fini_array_sz / 8; j > 0; j--) {
            uint64_t fn = ((uint64_t *)m->fini_array)[j - 1];
            if (fn) guest_call(c, fn);
        }
    }
}

static uint64_t sdk_start_entry(bool *has_finalizer) {
    uint64_t start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_");
    *has_finalizer = false;
    if (!start) {
        start = loader_find_symbol("_ZN2nn4init5StartEmmPFvvES2_S2_");
        *has_finalizer = start != 0;
    }
    return start;
}

// Claim the guest address range before anything else can map there. The host allocator places
// regions at random addresses in the same window; on a collision, re-exec for a fresh layout.
__attribute__((constructor(101))) static void reserve_guest_space(void) {
    if (plat_reserve_guest(GUEST_LO, GUEST_HI)) return;
    const char *tries = getenv("LARECOMP_REEXEC");
    int n = tries ? atoi(tries) : 0;
    if (n >= 20) {
        fprintf(stderr, "FATAL: guest address range %llx-%llx is not free\n", GUEST_LO, GUEST_HI);
        plat_exit(1);
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n + 1);
    setenv("LARECOMP_REEXEC", buf, 1);
    plat_exec(NULL);
    perror("re-exec");
    plat_exit(1);
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

static void reboot_idle(void) {  // (main thread, while guest threads stop) keep the window alive
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_QUIT) { LOG("window closed\n"); plat_exit(0); }
}

static void teardown(void) {
    uint64_t t0 = plat_now_ns();
    kernel_stop_threads(reboot_idle);
    research_finish();
    workshop_reset();
    performance_reset();
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
    char why[512];
    if (!title_profile_select(root,why,sizeof why)) FATAL("%s\n",why);
    game_profile_activate(root, rt_data_root());
    extern void save_backups_set_game_root(const char *);
    extern void rando_set_game_root(const char *);
    save_backups_set_game_root(root);
    rando_set_game_root(root);
    mods_set_game_root(root);
    photo_mode_exit();
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
    if (!gamelib_load(root, why, sizeof why)) FATAL("%s\n", why);
    char main_path[1100];
    snprintf(main_path, sizeof main_path, "%s/main", exefs);
    game_version_select(main_path);
    if (title_is_las() && game_version_current() == GAME_UNSUPPORTED) FATAL("unsupported game executable\n");
    LOG("[game] %s %s\n",title_profile_current()->name,title_profile_current()->version);
    if (title_is_las() && !mods_load_active(rt_data_root(), root, game_profile_index(root, rt_data_root()),
                          game_version_name(game_version_current()), why, sizeof why)) FATAL("%s\n", why);
    loader_load(exefs);
    if (!rando_boot()) FATAL("The selected randomizer seed could not be loaded. Choose Vanilla or a compatible seed on the start screen.\n");
    if (title_is_las() && !workshop_boot(root, why, sizeof why)) FATAL("%s\n", why);
    if (title_is_las() && !research_boot(root, workshop_save_dir(), why, sizeof why)) FATAL("%s\n", why);
    { extern void settings_load(void); settings_load(); }
    kernel_init();
    dispatch_init();
    if (title_is_las() && !research_hooks_init(why, sizeof why)) FATAL("%s\n", why);
    { extern bool coop_boot(char *,size_t); if (!coop_boot(why,sizeof why)) FATAL("%s\n",why); }

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

    bool has_finalizer;
    uint64_t start = sdk_start_entry(&has_finalizer);
    if (!start) FATAL("nn::init::Start not found\n");
    uint64_t init_fn = hle_register("<rtld_init_sdk>", rtld_init_sdk);
    uint64_t main_init_fn = hle_register("<rtld_init_main>", rtld_init_main);
    uint64_t fini_fn = has_finalizer ? hle_register("<rtld_fini_modules>", rtld_fini_modules) : 0;
    KThread *t = kernel_create_main_thread(start, stack_size, prio);
    kernel_run_main(t, t->handle, 0, init_fn, main_init_fn, fini_fn);
}

int main(int argc, char **argv) {
    plat_init();
#ifdef _WIN32
    setvbuf(stderr, NULL, _IONBF, 0);  // the Windows CRT treats _IOLBF as full buffering
#else
    setvbuf(stderr, NULL, _IOLBF, 0);
#endif
    const char *root = NULL;  // no explicit CLI path: use the selected installation
    bool skip_launcher = getenv("LARECOMP_NO_LAUNCHER") || getenv("LARECOMP_INPUT");  // tests boot straight in
    static char arg_root[1024];
    int launch_profile = -1;
    bool create_shortcut = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) skip_launcher = true;
        else if (!strcmp(argv[i], "--create-shortcut")) create_shortcut = true;
        else if (!strcmp(argv[i], "--game")) {
            if (++i >= argc || (launch_profile = game_profile_from_id(argv[i])) < 0) {
                fprintf(stderr, "Unknown game installation. Available launch IDs:\n");
                for (int p = 1; p < GAME_PROFILE_COUNT; ++p) fprintf(stderr, "  %s\n", game_profile_id(p));
                return 2;
            }
        } else if (!strcmp(argv[i], "--data-root")) {
            if (++i >= argc) { fprintf(stderr, "--data-root needs a folder path.\n"); return 2; }
            setenv("LARECOMP_DATA_ROOT", argv[i], 1);
        }
        else if (argv[i][0] != '-' && !realpath(argv[i], arg_root)) root = argv[i];  // (relative to where we started)
        else if (argv[i][0] != '-') root = arg_root;
    }
    rt_paths_init();  // the data root becomes the working directory (runtime/paths.h)
    settings_load();
    if (launch_profile >= 0) root = game_profile_dir(launch_profile);
    if (!root) root = game_profile_dir(g_settings.game_profile);
    static char installed_root[2048];
    char install_error[512];
    if (game_install_resolve(root, installed_root, sizeof installed_root, install_error, sizeof install_error)) root = installed_root;
    else { fprintf(stderr, "%s\n", install_error); skip_launcher = false; }
    if (create_shortcut) {
        char shortcut[2048], why[512];
        if (!game_shortcut_create(root, shortcut, sizeof shortcut, why, sizeof why)) { fprintf(stderr, "%s\n", why); return 1; }
        printf("%s\n", shortcut); return 0;
    }
    if (skip_launcher && !gamelib_present(root)) skip_launcher = false;
    for (bool title = !skip_launcher;;) {
        if (title) {  // start screen (runtime/ui/launcher.mm): game files setup, seed choice, settings
            extern const char *launcher_run(const char *root);
            root = launcher_run(root);
        }
        boot(root);
        int kind = ui_run();  // window and input on the main thread; returns when a reboot is requested
        teardown();
        title = kind == RT_REBOOT_TITLE;
    }
}
