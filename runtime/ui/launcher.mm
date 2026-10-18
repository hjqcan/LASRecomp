// Start screen, shown before the game boots (user 2026-10-08, styled after Dusklight's launcher): PLAY (with
// ‹ Vanilla / seed › picker), SETTINGS, QUIT, game-files status at the lower left. Nothing of the game ships with
// Multi: on first launch the user picks the folder with their own game files (v1.0.0), we check the main
// module's build id, copy the files and build the game's data image; PLAY stays disabled until that worked. The background is a frame of the game's own title movie (from the user's files) once extracted.
//
// Skipped with --play, LARECOMP_NO_LAUNCHER=1, or any scripted test run (LARECOMP_INPUT): tests boot straight in.
// The in-game menu's "Title screen" comes back here without leaving the process (runtime/main.c reboots the game).
#ifdef __APPLE__
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#endif
#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <atomic>
#include <filesystem>

#include "imgui.h"
#include "localization.h"
#include "imgui_internal.h"  // (SetKeyOwner)
#include "option_rows.h"
#include "osk.h"
#include "pad_nav.h"
#ifdef __APPLE__
#include "imgui_impl_metal.h"
#else
#include "imgui_impl_vulkan.h"
#endif
#include "imgui_impl_sdl3.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include "platform.h"
#include "paths.h"
#include "gamebuild.h"
#include "gameversion.h"
#include "game_profiles.h"
#include "game_shortcut.h"
#include "game_install.h"
#include "title_profile.h"
#include "rando_profile.h"
#include "mods.h"
#include "acnh_speedfix.h"
#include "workshop.h"
#include "build_cache.h"
#include "gamelib.h"
#include "romfs_build.h"
#include "settings.h"
#include "input_bind.h"
#include "nvn/shader_prep.h"
void save_backups_set_game_root(const char *root);
void rando_set_game_root(const char *root);
bool controls_ui_took_back(void);  // ui/controls_ui.mm: B went back a step on Configure inputs
void ui_window_init(void);
SDL_Window *ui_window(void);
#ifdef __APPLE__
void *ui_metal_layer(void);
void *ui_metal_device(void);
#else
// runtime/nvn/vulkan: window frames and ImGui plumbing
bool vk_frame_begin(const float clear[4]);
void vk_frame_end(const char *shot_path);
void *vk_frame_cmd(void);
bool vk_imgui_backend_init(void);
void vk_imgui_backend_shutdown(void);
uint64_t vk_imgui_texture_rgba(const void *rgba, int w, int h);
#endif
}
#ifdef __APPLE__
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// (test) BGRA texture -> PNG (the game renderer's saver isn't up yet before the game boots)
static void save_png(id<MTLTexture> tex, const char *path) {
    size_t w = tex.width, h = tex.height;
    std::vector<uint8_t> px(w * h * 4);
    [tex getBytes:px.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs,
                                             kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dest) {  // (NULL when the file can't be created, e.g. its folder isn't there: CFRelease(NULL) traps)
        CGImageDestinationAddImage(dest, img, NULL);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
    } else fprintf(stderr, "[launcher] can't write %s\n", path);
    CFRelease(url), CGImageRelease(img), CGContextRelease(ctx), CGColorSpaceRelease(cs);
}
#endif

// The release tag this build is (also names the release files; Makefile / tools/bundle_macos.sh / make_dmg.sh read
// it): keep enhanced-beta<n>.<revision> so installed clients still find updates; the display name is Multi.
// A test build (a pre-release for a few players to try a fix)
// is test-<name>: the start screen says "Test build <name>", and it previews LARECOMP_TEST_OF - the update check
// offers that release (or a later one) as soon as it's out, so its players move to it.
#define LARECOMP_VERSION "enhanced-beta5.3"
#define LARECOMP_TEST_OF "enhanced-beta5.3"
extern "C" const char *larecomp_version(void) { return LARECOMP_VERSION; }  // (the shader caches' tag)

// ------------------------------------------------------------------ update check
// The start screen asks GitHub for the project's releases once per launch (a background curl, which macOS and
// Windows 10+ ship) and offers a newer Multi release by beta and revision number. Other release channels are
// ignored. Offline / no curl / anything unexpected: nothing is shown.
#define RELEASES_URL "https://api.github.com/repos/hjqcan/LASRecomp/releases?per_page=100"
#define RELEASE_PAGE "https://github.com/hjqcan/LASRecomp/releases/tag/"

// Multi keeps the legacy release-tag channel for installed clients; upstream tags never replace fork features.
static long long tag_rank(const std::string &t) {
    int beta, revision; char end;
    if (sscanf(t.c_str(), "enhanced-beta%d.%d%c", &beta, &revision, &end) == 2 &&
        beta > 0 && revision > 0 && revision < 65536)
        return ((long long)beta << 16) + revision;
    return -1;
}
static std::string tag_label(const std::string &t, bool) {
    if (t.rfind("enhanced-beta", 0) == 0) return "Multi Beta " + t.substr(13);
    if (!t.compare(0, 5, "test-")) return "Test build " + t.substr(5);
    return t;
}

static std::string g_update_tag;  // a newer release's tag (g_update_lock)
static pthread_mutex_t g_update_lock = PTHREAD_MUTEX_INITIALIZER;

static void *update_thread(void *) {
    std::string body;
    if (const char *t = getenv("LARECOMP_UPDATE_TAGS")) {  // (test) "beta_2,v1.0.0": these releases, no network
        for (const char *p = t; *p;) {
            const char *c = strchr(p, ',');
            body += "\"tag_name\": \"" + std::string(p, c ? c - p : strlen(p)) + "\"\n";
            p = c ? c + 1 : p + strlen(p);
        }
    } else {
#ifdef _WIN32
        const char *sr = getenv("SystemRoot");
        std::string curl = std::string(sr ? sr : "C:\\Windows") + "\\System32\\curl.exe";
#else
        std::string curl = "/usr/bin/curl";
#endif
        std::string out = std::string(rt_data_root()) + "/cache/releases.json";
        mkdir((std::string(rt_data_root()) + "/cache").c_str(), 0755);
        const char *av[] = {curl.c_str(), "-fsSL", "--max-time", "15", "-H", "Accept: application/vnd.github+json",
                            "-A", "Multi/" LARECOMP_VERSION, RELEASES_URL, NULL};
        PlatProc p = plat_spawn(av, out.c_str(), false);
        if (!p || plat_proc_wait(p, true) != 0) { remove(out.c_str()); return NULL; }
        if (FILE *f = fopen(out.c_str(), "rb")) {
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0 && body.size() < (8u << 20)) body.append(buf, n);
            fclose(f);
        }
        remove(out.c_str());
    }
    std::string best;
    // (a test build ranks just below the release it previews; other test builds rank -1: never offered)
    long long best_rank = strncmp(LARECOMP_VERSION, "test-", 5) ? tag_rank(LARECOMP_VERSION) : tag_rank(LARECOMP_TEST_OF) - 1;
    for (size_t at = 0; (at = body.find("\"tag_name\"", at)) != std::string::npos;) {
        size_t q1 = body.find('"', body.find(':', at) + 1), q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string tag = body.substr(q1 + 1, q2 - q1 - 1);
        if (tag_rank(tag) > best_rank) best_rank = tag_rank(tag), best = tag;
        at = q2;
    }
    if (!best.empty()) fprintf(stderr, "[launcher] update available: %s (this build %s)\n", best.c_str(), LARECOMP_VERSION);
    pthread_mutex_lock(&g_update_lock);
    g_update_tag = best;
    pthread_mutex_unlock(&g_update_lock);
    return NULL;
}

// ------------------------------------------------------------------ game files
static std::string g_root;  // selected installation: <root>/exefs, <root>/romfs
static int g_profile = -1, g_profile_request = -1;
static int g_speedfix_ready_profile = -1;
static std::string build_cache_root() {
    char pending[2048];
    if (acnh60_base_path(g_root.c_str(), pending, sizeof pending)) return g_root;
    return g_profile >= 0 && game_install_pending_path(game_profile_install_dir(g_profile), pending, sizeof pending) ? pending : g_root;
}

static bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

// The game's RomFS (v1.0.0) has exactly ROMFS_FILES files: a copy with fewer is incomplete (a copy or unzip that
// stopped partway) and the game crashes on the first one it misses (issues #24, #35). Only the number ships.
enum { ROMFS_FILES = 3836 };
static int count_files(const std::string &dir, int depth = 0) {
    DIR *d = depth < 12 ? opendir(dir.c_str()) : NULL;
    if (!d) return 0;
    int n = 0;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;  // (".", "..", .DS_Store)
        std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        n += S_ISDIR(st.st_mode) ? count_files(p, depth + 1) : S_ISREG(st.st_mode) ? 1 : 0;
    }
    closedir(d);
    return n;
}
// The copied romfs' file count, counted once (the checks below run every frame): -1 = count again (game files set
// up, deleted)
static int g_romfs_count = -1;
static std::atomic<bool> g_build_cache_changed{true};
static void game_files_changed(void) { g_romfs_count = -1; g_build_cache_changed = true; }
// The setup page redraws every frame; read the build cache's disk state only after the files/build state changes.
static bool g_resumable, g_has_build_cache;
static void read_build_cache(void) {
    if (!g_build_cache_changed.exchange(false)) return;
    const std::string root = build_cache_root();
    g_resumable = build_cache_resumable(root.c_str());
    g_has_build_cache = build_cache_present(root.c_str());
}
static bool resumable_build(void) { read_build_cache(); return g_resumable; }
static bool has_build_cache(void) { read_build_cache(); return g_has_build_cache; }

static std::string incomplete(int have) {
    char b[96];
    snprintf(b, sizeof b, "Game files incomplete (%d of %d files)", have, (int)ROMFS_FILES);
    return b;
}

// The copied game files alone ("" = there), without the game code built from them
static std::string check_copied_files(const std::string &root) {
    char published[2048], resolve_error[512];
    if (!game_install_resolve(root.c_str(), published, sizeof published, resolve_error, sizeof resolve_error)) return resolve_error;
    if (!file_exists(root + "/exefs/main")) return "No game files yet";
    char why[512];
    const TitleProfile *actual = title_profile_validate(root.c_str(), why, sizeof why);
    if (!actual) return why;
    int slot = game_profile_index(root.c_str(), rt_data_root());
    const char *expected = game_profile_version(slot), *title = game_profile_title(slot);
    if ((expected && strcmp(expected, actual->version)) || (title && strcmp(title, actual->key)))
        return "The game version does not match this installation slot.";
    if (!title_content_validate(actual, (root + "/romfs").c_str(), file_exists(root + "/romfs.bin"),
                                &g_romfs_count, why, sizeof why)) return why;
    return "";
}

// "" = ready to play, else what is wrong
static std::string check_game_files(const std::string &root) {
    std::string why = check_copied_files(root);
    if (why.empty() && !gamelib_present(root.c_str())) return "Game code not built yet";
    return why;
}

// What to do about a check_game_files() problem (the line under it on the start screen)
static const char *status_hint(const std::string &why) {
    if (why.rfind("Game files incomplete", 0) == 0)
        return "Some of the game's files are missing, usually from a copy or unzip that stopped partway. Copy your game "
               "files again, then choose that folder and set them up again.";
    if (why == "Wrong version of the game") return "Multi supports the verified v1.0.0 and v1.1.0 executable builds.";
    if (why == "Game code not built yet") return "Set up the game files to build it (a few minutes, only once).";
    return "Set up the game files to play.";
}

// ------------------------------------------------------------------ setup (background thread)
// The player brings their own game files: a folder holding exefs/ and romfs/. They're copied into the data root (APFS
// clones / hard links where the file system allows); the game reads romfs/ in place (runtime/services/fs.c).
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_src;           // the picked folder
static std::string g_update_archive;    // optional NSP/NSZ update
static std::string g_keys, g_title_keys; // selected local paths, never written to settings
static GameArchiveInfo g_archive_info[2];
static bool g_inspected;
static bool g_pending_inspect;
static int g_detected_profile = -1;
static bool is_archive(const std::string &p) {
    return p.size() > 4 && (!SDL_strcasecmp(p.c_str() + p.size() - 4, ".nsz") ||
                            !SDL_strcasecmp(p.c_str() + p.size() - 4, ".nsp") ||
                            !SDL_strcasecmp(p.c_str() + p.size() - 4, ".xci") ||
                            !SDL_strcasecmp(p.c_str() + p.size() - 4, ".xcz"));
}
static std::string g_job;           // current setup step / result for the status line
static std::string g_job_hint;      // a failure: what to do about it (shown under it)
static bool g_busy, g_job_failed, g_job_inspection;
static bool g_install_published;
static std::atomic<bool> g_cancel_build{false};
static Uint64 g_build_started;
static bool g_confirm_delete;       // game-files page: "Delete the game files?" shown

static void set_job(const std::string &s, bool failed = false, const std::string &hint = "") {
    pthread_mutex_lock(&g_lock);
    g_job = s;
    g_job_failed = failed;
    g_job_hint = hint;
    pthread_mutex_unlock(&g_lock);
}

// Setup failures: a short headline and what to do next (also LARECOMP_LAUNCHER_SHOT=<f>:fail<1-6>, screenshots)
enum { FAIL_NOT_FOUND = 1, FAIL_UNUSABLE, FAIL_COPY, FAIL_VERSION, FAIL_MISSING, FAIL_BUILD };
static void set_failure(int k, int have = -1) {
    static const char *const head[] = {"", "No game files found in that folder", "Those are game packages, not game files",
                                       "Setup didn't finish", "Wrong version of the game", "Your game files are incomplete",
                                       "The game code didn't build"};
    static const char *const hint[] = {
        "",
        "Choose the folder that has the game's exefs and romfs folders in it.",
        "Choose a supported base archive or a folder containing exefs and romfs.",
        "Make sure there's about 12 GB free and try again.",
        "Multi supports the verified v1.0.0 and v1.1.0 executable builds.",
        "Some of the game's files are missing - usually a copy or unzip that stopped partway. Copy the game files "
        "again so none are missing, then choose the folder again.",
        "Make sure there's about 2 GB free and try again. If it keeps happening, report it on GitHub."};
    if (k < 1 || k > 6) return;
    if (k == FAIL_MISSING && have >= 0) {
        char b[320];
        snprintf(b, sizeof b, "The romfs folder has %d of the game's %d files - some are missing, usually from a copy or "
                 "unzip that stopped partway. Copy the game files again so none are missing, then choose the folder "
                 "again.", have, (int)ROMFS_FILES);
        set_job(head[k], true, b);
        return;
    }
    set_job(head[k], true, hint[k]);
}

static bool is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// exefs: main and main.npdm; romfs: content roots declared by a reviewed adapter. Found
// in the picked folder or a few levels below it (some folder layouts nest them).
static void find_game(const std::string &dir, int depth, std::string &exefs, std::string &romfs, bool &unusable) {
    if (depth > 4 || (!exefs.empty() && !romfs.empty())) return;
    if (exefs.empty() && file_exists(dir + "/main") && file_exists(dir + "/main.npdm")) exefs = dir;
    bool content_root = false;
    for (unsigned i = 0; i < title_profile_count(); ++i)
        content_root |= title_content_root(title_profile_at(i), dir.c_str());
    if (romfs.empty() && content_root) romfs = dir;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> sub;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n[0] == '.') continue;
        std::string ext = n.size() > 4 ? n.substr(n.size() - 4) : "";
        for (auto &ch : ext) ch = (char)tolower((unsigned char)ch);
        if (ext == ".xci" || ext == ".nsp" || ext == ".nca" || ext == ".nsz" || ext == ".xcz") unusable = true;
        if (!content_root && is_dir(dir + "/" + n)) sub.push_back(dir + "/" + n);
    }
    closedir(d);
    std::sort(sub.begin(), sub.end());
    for (auto &s : sub) find_game(s, depth + 1, exefs, romfs, unusable);
}

static bool build_id_ok(const std::string &main_path) {
    return title_profile_find(main_path.c_str()) != NULL;
}

static bool prepare_install_parent(void) {
    try {
        if (game_profile_index(g_root.c_str(), rt_data_root()) < 0) return false;
        auto parent = std::filesystem::u8path(g_root).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        return true;
    } catch (...) { return false; }
}

// progress (the overlay's bar): the romfs copy (bytes)
static std::atomic<unsigned long long> g_done, g_total;
// The bar: copying is the first tenth, building the game code (runtime/gamebuild.c) the rest
static unsigned long long g_phase_base;
static std::atomic<float> g_build_frac{-1};  // -1: not building
static bool g_copy_skipped;                  // (only the game code to build: the bar is all of that)
static bool g_auto_update;                   // started by itself: game files there, game code from an older version
static void setup_progress(uint64_t done, uint64_t) { g_done = g_phase_base + done; }
static void build_progress(double f, const char *step) {
    g_build_frac = (float)f;
    static const char *last;
    if (step != last) last = step, set_job(step);
}
// Shader preparation, after the game files are ready (runtime/nvn/shader_prep.h): its own bar, and Skip
static std::atomic<float> g_prep_frac{-1};  // -1: not preparing
static volatile int g_prep_skip;
static bool g_prep_skip_ask;  // Skip pressed: "are you sure?" in front of the bar (LARECOMP_LAUNCHER_SHOT=<f>:skipask)
static void prep_progress(int done, int total) { g_prep_frac = total ? (float)done / (float)total : 0.0f; }
static bool can_prepare_shaders(void) {
    const TitleProfile *title = title_profile_find((g_root + "/exefs/main").c_str());
    return title && !strcmp(title->key, "las");
}
static void prepare_shaders(bool background = false) {
    if (!can_prepare_shaders()) return;
    g_prep_skip = 0, g_prep_frac = 0;
    if (!background) set_job("Preparing shaders");
    shader_prep_run((g_root + "/romfs").c_str(), prep_progress, &g_prep_skip);
    g_prep_frac = -1;
}

// Every later start: the same preparation in the background as soon as the start screen is up (quick: the system's
// shader caches already hold the compiled code). Only a PLAY before it is done shows its bar (g_prep_wait).
static std::atomic<bool> g_bg_prep{false};
static bool g_prep_wait;
static void start_background_prep(void) {
    if (g_bg_prep || !can_prepare_shaders() || getenv("LARECOMP_LAUNCHER_BUILD_ONLY")) return;
    g_bg_prep = true;
    pthread_t th;
    if (!pthread_create(&th, NULL, [](void *) -> void * { prepare_shaders(true); g_bg_prep = false; return NULL; }, NULL))
        pthread_detach(th);
    else g_bg_prep = false;
}
// Before the game files change (setup, delete): the background preparation stops reading them
static void stop_background_prep(void) {
    if (!g_bg_prep) return;
    g_prep_skip = 1;
    while (g_bg_prep) SDL_Delay(5);
}

static float extract_progress(void) {
    if (float p = g_prep_frac; p >= 0) return p > 1 ? 1 : p;
    unsigned long long t = g_total, d = g_done;
    float b = g_build_frac, f = b >= 0 ? (g_copy_skipped ? b : 0.1f + 0.9f * b) : t ? 0.1f * (float)((double)d / (double)t) : 0;
    return f < 0 ? 0 : f > 0.99f ? 0.99f : f;
}

// Builds the game code from the copied files (a few minutes); false = failed (the job line says so)
static bool build_cancelled(void) {
    if (const char *s = getenv("LARECOMP_BUILD_CANCEL_AFTER"))
        if ((SDL_GetTicks() - g_build_started) / 1000.0 >= atof(s)) g_cancel_build = true;
    return g_cancel_build.load();
}

static void imported_for_build() {
    pthread_mutex_lock(&g_lock);
    g_src.clear(); g_update_archive.clear(); g_inspected = false;
    pthread_mutex_unlock(&g_lock);
}

static bool build_game_code(const std::string &target) {
    g_cancel_build = false;
    g_build_started = SDL_GetTicks();
    int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
    GameBuildOpts o = {build_progress, cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1, build_cancelled};
    if (const char *s = getenv("LARECOMP_BUILD_JOBS")) {
        char *end = nullptr;
        long jobs = strtol(s, &end, 10);
        if (*s && !*end && jobs >= 1 && jobs <= 64) o.jobs = (int)jobs;
    }
    char why[256] = "";
    g_build_frac = 0;
    bool ok = gamebuild_run(target.c_str(), &o, why, sizeof why);
    if (!ok) {
        fprintf(stderr, "[launcher] game code: %s\n", why);
        if (g_cancel_build) set_job("Compilation cancelled. Completed files are kept.");
        else set_job("The game code didn't build", true, why);
    }
    return ok;
}

static void import_progress(const char *message) { set_job(message); }

static bool validate_candidate_content(const char *root, char *why, size_t size) {
    const TitleProfile *p = title_profile_validate(root, why, size);
    if (!p) return false;
    const char *title = game_profile_title(g_profile), *version = game_profile_version(g_profile);
    if ((title && strcmp(title, p->key)) || (version && strcmp(version, p->version))) {
        snprintf(why, size, "The game version does not match this installation slot"); return false;
    }
    if (!p->installable && !getenv("LARECOMP_ALLOW_DEVELOPMENT_GAMES")) {
        snprintf(why, size, "This game adapter is still in development."); return false;
    }
    int count = -1;
    std::string folder = root;
    return title_content_validate(p, (folder + "/romfs").c_str(), file_exists(folder + "/romfs.bin"), &count, why, size);
}
static bool validate_candidate(const char *root, char *why, size_t size) {
    return validate_candidate_content(root, why, size) && gamelib_verify(root, why, size);
}
static void *extract_thread(void *) {
    stop_background_prep();
    std::string src, update, keys, title_keys;
    pthread_mutex_lock(&g_lock);
    g_job_inspection = false;
    src = g_src; update = g_update_archive; keys = g_keys; title_keys = g_title_keys;
    pthread_mutex_unlock(&g_lock);
    g_cancel_build = false;
    g_done = 0; g_total = 0; g_phase_base = 0; g_build_frac = -1; g_copy_skipped = src.empty();
    GameInstall *install = nullptr;
    bool published = false;
    char why[512] = "";
    std::string exefs, romfs, target;
    bool unusable = false;
    char speedfix_base[2048];
    bool speedfix = acnh60_base_path(g_root.c_str(), speedfix_base, sizeof speedfix_base);
    if (gamelib_tables()) {
        set_job("Restart Multi before changing the game files.", true,
                "The current game code is still loaded. Your files have not been changed.");
        goto done;
    }
    if (speedfix && !src.empty()) {
        set_job("Switch to normal 30 FPS before replacing game files.", true); goto done;
    }
    if (g_profile < 0 || speedfix) {
        // External extracted folders retain the existing build-only workflow. No imported data is replaced.
        if (!src.empty()) { set_job("Choose a game installation before setting up files.", true); goto done; }
        std::string problem = check_copied_files(g_root);
        if (!problem.empty()) { set_job("Game files incomplete.", true, problem.c_str()); goto done; }
        if (!build_game_code(g_root)) goto done;
        if (!gamelib_verify(g_root.c_str(), why, sizeof why)) { set_job("Game code build failed.", true, why); goto done; }
        set_job("Game files ready.");
        goto done;
    }
    {
        const char *slot = game_profile_install_dir(g_profile);
        bool pending = game_install_pending(slot);
        install = game_install_begin(slot, !src.empty(), why, sizeof why);
        if (!install) { set_job("Cannot prepare the game installation folder.", true, why); goto done; }
        target = game_install_candidate(install);
        if (src.empty()) {
            if (!pending) {
                set_job("Preparing the game installation...");
                if (!game_install_clone(install, g_root.c_str(), why, sizeof why)) {
                    set_job("Could not prepare the game installation.", true, why); goto done;
                }
            } else if (!game_install_resume_clone(install, why, sizeof why)) {
                set_job("Could not resume the game installation copy.", true, why); goto done;
            }
        } else if (is_archive(src)) {
            set_job(update.empty() ? "Importing the base game..." : "Importing the base game and update...");
            if (!gameimport_run(src.c_str(), update.c_str(), keys.c_str(), title_keys.c_str(), target.c_str(),
                                game_profile_title(g_profile), game_profile_version(g_profile), import_progress, why, sizeof why)) {
                set_job("Game import failed.", true, why); goto done;
            }
        } else {
            set_job("Looking for the game files...");
            find_game(src, 0, exefs, romfs, unusable);
            if (exefs.empty() || romfs.empty()) { set_failure(unusable ? FAIL_UNUSABLE : FAIL_NOT_FOUND); goto done; }
            std::string source_root = exefs.substr(0, exefs.find_last_of("/\\"));
            const TitleProfile *picked = title_profile_find((exefs + "/main").c_str());
            int count = -1;
            if (!picked || !title_content_validate(picked, romfs.c_str(), file_exists(source_root + "/romfs.bin"), &count, why, sizeof why)) {
                set_job("Game files incomplete.", true, why); goto done;
            }
            g_total = romfs_dir_bytes(romfs.c_str());
            set_job("Copying the game files...");
            if (!romfs_copy_tree(exefs.c_str(), (target + "/exefs").c_str(), nullptr) ||
                !romfs_copy_tree(romfs.c_str(), (target + "/romfs").c_str(), setup_progress)) {
                set_failure(FAIL_COPY); goto done;
            }
            if (file_exists(source_root + "/romfs.bin")) {
                std::error_code error;
                std::filesystem::copy_file(std::filesystem::u8path(source_root + "/romfs.bin"),
                                          std::filesystem::u8path(target + "/romfs.bin"), error);
                if (error) { set_failure(FAIL_COPY); goto done; }
            }
            g_phase_base = g_total.load();
        }
    }
    if (!validate_candidate_content(target.c_str(), why, sizeof why)) {
        set_job("Game files incomplete.", true, why); goto done;
    }
    imported_for_build();
    if (!build_game_code(target)) goto done;
    set_job("Verifying the completed installation...");
    if (!game_install_publish(install, game_profile_id(g_profile), validate_candidate, why, sizeof why)) {
        set_job("Could not finish the game installation.", true, why); goto done;
    }
    published = true;
    set_job("Game files ready.");
#ifdef _WIN32
    {
        char shortcut[2048];
        if (game_shortcut_create(target.c_str(), shortcut, sizeof shortcut, why, sizeof why))
            set_job("Game ready. Its desktop shortcut has been created.");
        else set_job("Game ready. Desktop shortcut could not be created.", false, why);
    }
#endif
done:
    game_install_close(install);
    pthread_mutex_lock(&g_lock);
    g_install_published = published;
    g_busy = false;
    pthread_mutex_unlock(&g_lock);
    return nullptr;
}

static void *speedfix_thread(void *) {
    stop_background_prep();
    g_cancel_build=false;g_build_started=SDL_GetTicks();g_build_frac=-1;g_copy_skipped=true;
    char why[512]="";
    bool ready=false;
    const int profile=g_profile;
    const std::string base=game_profile_base_dir(profile), target=base+"/.acnh60";
    const TitleProfile *title=title_profile_validate(base.c_str(),why,sizeof why);
    if (gamelib_tables()) set_job("Restart Multi before changing frame-rate mode.",true);
    else if (!title || strcmp(title->key,"acnh") || strcmp(title->version,"3.0.1"))
        set_job("Install the normal 3.0.1 game files first.",true,why);
    else {
        GameBuildOpts options={build_progress,1,build_cancelled};
        bool prepared=file_exists(target);
        if (!prepared) {
            set_job("Preparing 60 FPS + SpeedFix...");
            prepared=gameprepare_acnh60(base.c_str(),&options,why,sizeof why);
        }
        if (!prepared || !acnh60_profile(title,target.c_str(),why,sizeof why) ||
            !acnh60_validate(target.c_str(),why,sizeof why))
            set_job("Could not prepare 60 FPS + SpeedFix.",true,why);
        else if (gamelib_verify(target.c_str(),why,sizeof why) || build_game_code(target)) {
            if (gamelib_verify(target.c_str(),why,sizeof why)) {
                set_job("60 FPS + SpeedFix is ready.");ready=true;
            } else set_job("Game code build failed.",true,why);
        }
    }
    pthread_mutex_lock(&g_lock);
    if (ready) g_speedfix_ready_profile=profile;
    g_busy=false;
    pthread_mutex_unlock(&g_lock);
    return nullptr;
}

static void *inspect_thread(void *) {
    pthread_mutex_lock(&g_lock);
    std::string src = g_src, update = g_update_archive, keys = g_keys, title_keys = g_title_keys;
    g_job_inspection = true;
    pthread_mutex_unlock(&g_lock);
    GameArchiveInfo info[2] = {};
    char why[512] = "";
    set_job("Checking archive metadata...");
    bool ok = prepare_install_parent() && gameimport_inspect(src.c_str(), update.c_str(), keys.c_str(), title_keys.c_str(),
                                 g_root.c_str(), info, why, sizeof why);
    set_job(ok ? "Metadata checked. Executable and data checks run during import." : "Archive check failed.", !ok, why);
    pthread_mutex_lock(&g_lock);
    if (ok) {
        memcpy(g_archive_info, info, sizeof info);
        const GameArchiveInfo &final = update.empty() ? info[0] : info[1];
        g_detected_profile = game_profile_match(final.game_id, final.version);
    }
    g_inspected = ok;
    g_busy = false;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

// ------------------------------------------------------------------ folder dialog (SDL calls back on any thread)
static void SDLCALL picked(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_lock);
    g_src = files[0];
    g_update_archive.clear();
    g_inspected = false;
    pthread_mutex_unlock(&g_lock);
}

static void SDLCALL picked_archive(void *update, const char *const *files, int) {
    if (!files || !files[0]) return;
    pthread_mutex_lock(&g_lock);
    if (update == (void *)1) g_update_archive = files[0];
    else if (update == (void *)2) g_keys = files[0];
    else if (update == (void *)3) g_title_keys = files[0];
    else g_src = files[0];
    g_inspected = false;
    g_pending_inspect = is_archive(g_src);
    pthread_mutex_unlock(&g_lock);
}

// ------------------------------------------------------------------ the user's own Nintendo fonts (not shipped)
// runtime/hle/pl.c loads fonts/ first, else a close substitute of the requested language. Import copies recognized
// files only: the console's own names for the shared fonts (Standard, Chinese Simplified / its Ext, Chinese
// Traditional, Korean and NintendoExt).
static const char *FONT_STD[] = {"nintendo_udsg-r_std_003.bfttf", "nintendo_udsg-r_std_003.ttf",
                                 "nintendo_udsgr_std_003.ttf", "nintendo_udsgr_std_003.bfttf"};
static const char *FONT_LANG[] = {"nintendo_udsg-r_org_zh-cn_003.bfttf", "nintendo_udsg-r_org_zh-cn_003.ttf",
                                  "nintendo_udsg-r_ext_zh-cn_003.bfttf", "nintendo_udsg-r_ext_zh-cn_003.ttf",
                                  "nintendo_udjxh-db_zh-tw_003.bfttf", "nintendo_udjxh-db_zh-tw_003.ttf",
                                  "nintendo_udsg-r_ko_003.bfttf", "nintendo_udsg-r_ko_003.ttf"};
static const char *FONT_EXT[] = {"nintendo_ext_003.bfttf", "nintendo_ext_003.ttf"};
static std::vector<std::string> g_font_picks;
static std::string g_font_msg;

static bool have_font(const char *const *names, int n) {
    for (int i = 0; i < n; i++)
        if (file_exists(std::string("fonts/") + names[i])) return true;
    return false;
}

static void SDLCALL picked_fonts(void *, const char *const *files, int) {
    if (!files) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; files[i]; i++) g_font_picks.push_back(files[i]);
    pthread_mutex_unlock(&g_lock);
}

static void import_fonts() {
    pthread_mutex_lock(&g_lock);
    std::vector<std::string> picks;
    picks.swap(g_font_picks);
    pthread_mutex_unlock(&g_lock);
    if (picks.empty()) return;
    mkdir("fonts", 0755);
    int ok = 0, skipped = 0;
    for (auto &src : picks) {
        std::string base = src.substr(src.find_last_of("/\\") + 1), low = base;  // (Windows paths use '\\' too)
        for (auto &c : low) c = (char)tolower(c);
        bool known = false;
        for (auto n : FONT_STD) known |= low == n;
        for (auto n : FONT_LANG) known |= low == n;
        for (auto n : FONT_EXT) known |= low == n;
        if (!known) { skipped++; continue; }
        FILE *in = fopen(src.c_str(), "rb"), *out = fopen(("fonts/" + low).c_str(), "wb");
        if (in && out) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
            ok++;
        }
        if (in) fclose(in);
        if (out) fclose(out);
    }
    char result[256];
    snprintf(result, sizeof result, ui_tr("Imported %d font files; %d not recognized. Used from the next game start."), ok, skipped);
    g_font_msg = result;
}

static void remove_fonts() {
    for (auto n : FONT_STD) unlink((std::string("fonts/") + n).c_str());
    for (auto n : FONT_LANG) unlink((std::string("fonts/") + n).c_str());
    for (auto n : FONT_EXT) unlink((std::string("fonts/") + n).c_str());
    g_font_msg = "Imported fonts removed: the substitute fonts are used.";
}

// ------------------------------------------------------------------ background: a title movie frame
#ifdef __APPLE__
static id<MTLTexture> g_bg;
#define BG_TEXID ((ImTextureID)(__bridge void *)g_bg)
typedef id<MTLDevice> GpuDevice;
#else
static uint64_t g_bg;  // ImGui texture id (Vulkan descriptor set)
#define BG_TEXID ((ImTextureID)g_bg)
typedef void *GpuDevice;
#endif
static int g_bg_w, g_bg_h;

static void load_background(GpuDevice dev) {
    std::string path = g_root + "/romfs/region_common/movie/title.webm";
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path.c_str(), NULL, NULL) < 0) return;
    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (si >= 0 && avformat_find_stream_info(fmt, NULL) >= 0) {
        AVStream *st = fmt->streams[si];
        const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
        AVCodecContext *cc = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(cc, st->codecpar);
        if (codec && avcodec_open2(cc, codec, NULL) == 0) {
            AVPacket *pkt = av_packet_alloc();
            AVFrame *fr = av_frame_alloc();
            int64_t want = (int64_t)(6.0 / av_q2d(st->time_base));  // a few seconds in: the island, not black
            av_seek_frame(fmt, si, want, AVSEEK_FLAG_BACKWARD);
            bool got = false;
            while (!got && av_read_frame(fmt, pkt) >= 0) {
                if (pkt->stream_index == si && avcodec_send_packet(cc, pkt) == 0)
                    while (avcodec_receive_frame(cc, fr) == 0)
                        if (fr->best_effort_timestamp >= want) { got = true; break; }
                av_packet_unref(pkt);
            }
            if (got) {
                int w = fr->width, h = fr->height;
                std::vector<uint8_t> rgba((size_t)w * h * 4);
                SwsContext *sws = sws_getContext(w, h, (AVPixelFormat)fr->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
                uint8_t *dst[1] = {rgba.data()};
                int dst_ls[1] = {w * 4};
                sws_scale(sws, fr->data, fr->linesize, 0, h, dst, dst_ls);
                sws_freeContext(sws);
#ifdef __APPLE__
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:w height:h mipmapped:NO];
                g_bg = [dev newTextureWithDescriptor:td];
                [g_bg replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:w * 4];
#else
                (void)dev;
                g_bg = vk_imgui_texture_rgba(rgba.data(), w, h);
#endif
                g_bg_w = w, g_bg_h = h;
            }
            av_frame_free(&fr);
            av_packet_free(&pkt);
        }
        avcodec_free_context(&cc);
    }
    avformat_close_input(&fmt);
}

// ------------------------------------------------------------------ seeds
static std::vector<std::string> list_seeds() {
    std::vector<std::string> out;
    if (!title_is_las()) return out;
    const TitleProfile *selected = title_profile_find((g_root + "/exefs/main").c_str());
    if (selected && strcmp(selected->key, "las")) return out;
    if (DIR *d = opendir("rando")) {
        while (struct dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string dir = std::string("rando/") + e->d_name;
            if (rando_seed_name_valid(e->d_name) && rando_seed_compatible(dir.c_str(), game_version_current()) &&
                file_exists(dir + "/settings.json") && file_exists(dir + "/romfs")) out.push_back(e->d_name);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ------------------------------------------------------------------ screen
static ImFont *g_font_title, *g_font_menu, *g_font_small;
enum Page { MAIN, SETTINGS, SETUP };  // SETTINGS: the in-game menu's tabs (runtime/ui/menu.mm)
static int g_refocus = MAIN;  // back on the main page: select the item of the page we came from (controllers)

// A big menu line: gold + a marker when focused / hovered (keyboard, gamepad or mouse), grey when disabled
static bool g_menu_small;  // sub pages: buttons in the small font
void menu_tabs_draw(float scale, ImFont *font, ImFont *font_big, bool in_game, void (*prelaunch)(float));  // menu.mm
void menu_tabs_focus(bool first_tab);
extern int g_tab_request_ext;
extern "C" void menu_apply_window_requests(void *sdl_window);
extern "C" int g_start_screen;  // runtime/hle/warp.c: the start screen is up (no game loaded: don't read its memory)
// The Randomizer tab's Play / Play Now on the start screen (rando_tab.mm): start the game with that seed
static std::string g_play_seed;
static bool g_play_seed_req, g_play_pending;
extern "C" void launcher_play_seed(const char *seed) {
    g_play_seed = seed ? seed : "";
    g_play_seed_req = true;
}
static float g_menu_gap = 0.35f;  // space under each item, in line heights

// The first PLAY ever (settings.menu_tip_seen): a note on opening the Multi menu in game (it isn't obvious),
// OK starts the game. Returns true once OK is pressed.
static bool g_tip_test;  // (LARECOMP_LAUNCHER_SHOT=<frame>:tip)
// First PLAY ever, before the menu note: authentic 16:9 or the unlocked (fill the window) aspect ratio.
static bool aspect_prompt(float s) {
    bool done = false;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fminf(ds.x * 0.8f, 760 * s), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * s, 28 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16 * s);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.03f, 0.04f, 0.05f, 0.94f));
    bool open = ImGui::BeginPopupModal("##aspect", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (open) {
        ImGui::SetWindowFontScale(1.25f);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(ui_tr("How would you like to play?"));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", ui_tr("You can change this any time in Settings."));
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14 * s));
        float bw = 300 * s, gap = 20 * s;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 2 * bw - gap) * 0.5f);
        int pick = -1;
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(ui_label("Authentic 16:9").c_str(), ImVec2(bw, 44 * s))) pick = ASPECT_ORIGINAL;
        ImGui::SameLine(0, gap);
        if (ImGui::Button(ui_label("Unlocked (fill the screen)").c_str(), ImVec2(bw, 44 * s))) pick = ASPECT_UNLOCKED;
        if (pick >= 0) {
            g_settings.aspect = pick;
            g_settings.aspect_asked = true;
            settings_save();
            ImGui::CloseCurrentPopup();
            done = true;
        }
        ImGui::EndPopup();
    }
    return done;
}

static bool menu_tip(float s) {
    bool go = false;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fminf(ds.x * 0.8f, 760 * s), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * s, 28 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16 * s);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.03f, 0.04f, 0.05f, 0.94f));
    bool open = ImGui::BeginPopupModal("##menutip", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (open) {
        ImGui::SetWindowFontScale(1.25f);
        // the controller's home button first (what players reach for: the PS button on a DualSense), the combo second
        static const char *const home[] = {"Home button", "Xbox button", "PlayStation button"};  // (bind_label_family)
        static const char *const pad[] = {"Minus + Plus", "View + Menu", "Create + Options"};
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(ui_tr("Multi has its own menu for settings, the randomizer, cosmetics and more."));
        ImGui::Spacing();
        int fam = bind_label_family() % 3;
        ImGui::Text(ui_tr("Open it in game with F1 or Esc, or on a controller with the %s (or %s together)."), ui_tr(home[fam]), pad[fam]);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14 * s));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 180 * s) * 0.5f);
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(ui_label("OK").c_str(), ImVec2(180 * s, 44 * s)) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
            g_settings.menu_tip_seen = true;
            settings_save();
            ImGui::CloseCurrentPopup();
            go = true;
        }
        ImGui::EndPopup();
    }
    return go;
}

// Start-screen text: a soft drop shadow, then the text with a top-to-bottom colour gradient (top == bottom: flat)
static void text_fx(ImDrawList *dl, ImFont *font, float size, ImVec2 p, ImU32 top, ImU32 bottom, const char *t,
                    float shadow) {
    if (shadow > 0)
        for (float d : {shadow, shadow * 0.5f}) dl->AddText(font, size, ImVec2(p.x + d, p.y + d), IM_COL32(4, 8, 30, 110), t);
    int v0 = dl->VtxBuffer.Size;
    dl->AddText(font, size, p, top, t);
    if (top == bottom) return;
    float y0 = p.y + size * 0.2f, y1 = p.y + size * 0.85f;
    for (int i = v0; i < dl->VtxBuffer.Size; i++) {
        float f = (dl->VtxBuffer[i].pos.y - y0) / (y1 - y0);
        f = f < 0 ? 0 : f > 1 ? 1 : f;
        ImVec4 a = ImGui::ColorConvertU32ToFloat4(top), b = ImGui::ColorConvertU32ToFloat4(bottom);
        dl->VtxBuffer[i].col = ImGui::ColorConvertFloat4ToU32(ImVec4(a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f,
                                                                     a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f));
    }
}

// A four-point sparkle (PLAY's twinkle): k = 0..1 grows and fades it
static void sparkle(ImDrawList *dl, ImVec2 c, float r, float k, float spin) {
    if (k <= 0) return;
    ImVec2 pts[8];
    for (int i = 0; i < 8; i++) {
        float a = spin + i * 3.14159265f / 4, rr = (i & 1 ? r * 0.22f : r) * k;
        pts[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr);
    }
    dl->AddCircleFilled(c, r * 0.55f * k, IM_COL32(255, 230, 150, (int)(60 * k)));  // soft glow
    dl->AddConvexPolyFilled(pts, 8, IM_COL32(255, 248, 215, (int)(235 * k)));
}

// split .. split_end: that part of the label in its own colour (PLAY's choice: Vanilla in vanilla cream)
static bool menu_item(const char *label, bool enabled, bool *hovered = NULL, const char *split = NULL, ImU32 col2 = 0,
                      const char *split_end = NULL) {
    ImGui::PushFont(g_menu_small ? g_font_small : g_font_menu);
    ImGui::BeginDisabled(!enabled);
    ImVec2 at = ImGui::GetCursorScreenPos();
    // (the ID from the label's fixed part: PLAY's changes with the seed shown, and a new ID loses the selection)
    ImGui::PushID(split ? "play" : label);
    if (!split) label = ui_tr(label);
    ImVec2 sz = ImGui::CalcTextSize(label);
    // (EnableNav: otherwise an InvisibleButton can't be reached with the d-pad / arrow keys - ImGui 1.91)
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the hover band shows the selection, no box)
    bool clicked = ImGui::InvisibleButton("##b", ImVec2(sz.x + 40, sz.y + 6), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool hot = ImGui::IsItemFocused() || ImGui::IsItemHovered();
    ImGui::PopID();
    if (hovered) *hovered = hot;
    ImU32 col = !enabled ? IM_COL32(170, 175, 200, 140) : hot ? IM_COL32(255, 214, 64, 255) : IM_COL32(250, 250, 255, 255);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hot && enabled) {  // a shadow band behind the item (like the version corner's), feathered, with a warm glow
        float x0 = ImGui::GetWindowPos().x, x1 = at.x + sz.x * 0.75f, x2 = at.x + sz.x + sz.y * 3.5f;
        float f = sz.y * 0.22f, y0 = at.y - f * 0.4f, y1 = y0 + f, y3 = at.y + sz.y + f * 0.2f, y2 = y3 - f;
        ImU32 c = IM_COL32(6, 10, 34, 150), g = IM_COL32(255, 190, 80, 34), n = IM_COL32(6, 10, 34, 0), ng = IM_COL32(255, 190, 80, 0);
        for (int pass = 0; pass < 2; pass++) {  // dark band, then the glow over it
            ImU32 a = pass ? g : c, z = pass ? ng : n;
            dl->AddRectFilledMultiColor(ImVec2(x0, y1), ImVec2(x1, y2), a, a, a, a);
            dl->AddRectFilledMultiColor(ImVec2(x1, y1), ImVec2(x2, y2), a, z, z, a);
            dl->AddRectFilledMultiColor(ImVec2(x0, y0), ImVec2(x1, y1), z, z, a, a);  // (feathered top / bottom)
            dl->AddRectFilledMultiColor(ImVec2(x1, y0), ImVec2(x2, y1), z, z, z, a);
            dl->AddRectFilledMultiColor(ImVec2(x0, y2), ImVec2(x1, y3), a, a, z, z);
            dl->AddRectFilledMultiColor(ImVec2(x1, y2), ImVec2(x2, y3), a, z, z, z);
        }
    }
    ImU32 col_b = hot && enabled ? IM_COL32(255, 160, 50, 255) : col;
    if (split && enabled) {
        std::string head(label, split - label);
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), at, col, col_b, head.c_str(), sz.y * 0.05f);
        std::string mid = split_end ? std::string(split, split_end - split) : std::string(split);
        ImVec2 p2(at.x + ImGui::CalcTextSize(label, split).x, at.y);
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), p2, col2, col2, mid.c_str(), sz.y * 0.05f);
        if (split_end)
            text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + ImGui::CalcTextSize(label, split_end).x, at.y),
                    col, col, split_end, sz.y * 0.05f);
    } else
        text_fx(dl, ImGui::GetFont(), ImGui::GetFontSize(), at, col, col_b, label, enabled ? sz.y * 0.05f : 0);
    if (g_menu_gap >= 0) ImGui::Dummy(ImVec2(0, sz.y * g_menu_gap));  // line spacing
    else ImGui::SetCursorPosY(ImGui::GetCursorPosY() + sz.y * g_menu_gap);  // (tighter than the font's line height)
    ImGui::EndDisabled();
    ImGui::PopFont();
    return clicked;
}

// Controllers, as in the in-game menu (runtime/ui/menu.mm): read straight from SDL, the controller's own layout
// (pad_nav.h: confirm on the bottom button for Xbox / PlayStation pads, the right one for Nintendo pads); d-pad / left
// stick move. (ImGui's own SDL gamepad handling is switched off: per the user, it didn't work on the start screen.)

static void launcher_pad_open_all(void) {
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
}

// true when the event was a controller event (handled here)
static bool launcher_pad_event(const SDL_Event &e) {
    ImGuiIO &io = ImGui::GetIO();
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        SDL_OpenGamepad(e.gdevice.which);
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        ImGuiKey k = ui_pad_key(e.gbutton.which, e.gbutton.button, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        if (k != ImGuiKey_None) io.AddKeyEvent(k, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        float v = e.gaxis.value / 32767.0f;
        // the left stick also works as the d-pad (ImGui moves between items with the d-pad only; the stick scrolls):
        // pressed past half way, released under 0.3
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            static bool held[4];  // left, right, up, down
            bool x = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX;
            int neg = x ? 0 : 2, pos = neg + 1;
            const ImGuiKey keys[4] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadDpadUp,
                                      ImGuiKey_GamepadDpadDown};
            for (int i : {neg, pos}) {
                float m = i == neg ? -v : v;
                bool now = held[i] ? m > 0.3f : m > 0.5f;
                if (now != held[i]) { held[i] = now; io.AddKeyEvent(keys[i], now); }
            }
        }
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, v < -0.3f, v < -0.3f ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, v > 0.3f, v > 0.3f ? v : 0);
        } else if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, v < -0.3f, v < -0.3f ? -v : 0);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, v > 0.3f, v > 0.3f ? v : 0);
        }
        return true;
    }
    default: return false;
    }
}

// Returns when PLAY is chosen (seed stored in g_settings.rando_seed); QUIT exits.
// Settings' first tab, on the start screen only: the extracted game files (size, delete) and the user's own Switch
// fonts (import / back to the substitute)
// Open a folder in Finder / Explorer (relative paths are in the data root, the working directory)
static void reveal_folder(const std::string &dir) {
    std::string path = dir;
    bool abs = !path.empty() && (path[0] == '/' || path[0] == '\\' || (path.size() > 1 && path[1] == ':'));
    if (!abs) path = std::string(rt_data_root()) + "/" + path;
    std::string url = "file://";
    if (path[0] != '/') url += "/";  // (Windows: file:///C:/...)
    for (char ch : path) {
        if (ch == '\\') ch = '/';
        if (ch == ' ') url += "%20";
        else if (ch == '%') url += "%25";
        else if (ch == '#') url += "%23";
        else url += ch;
    }
    SDL_OpenURL(url.c_str());
}

static SDL_Window *g_launcher_win;
// The progress overlay in front of the start screen (input waits): the setup, an update's build, or the shader
// preparation (with Skip) - the setup's, or the background one when PLAY was pressed before it finished
static void draw_busy_overlay(ImVec2 ds, const std::string &job, bool test_busy, bool prep_wait = false, bool archive = false) {
    float sc = ds.y / 1080.0f;
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    ImVec2 o = ImGui::GetMainViewport()->Pos;
    fg->AddRectFilled(o, ImVec2(o.x + ds.x, o.y + ds.y), IM_COL32(0, 0, 0, 170));
    float f = test_busy && g_prep_frac < 0 ? 0.42f : prep_wait && g_prep_frac < 0 ? 0.99f : extract_progress();
    // (while the game code is built: what's happening, on whose files - runtime/gamebuild.c)
    bool preparing = g_prep_frac >= 0 || prep_wait, recompiling = g_build_frac >= 0 && !preparing;
    bool importing = archive && !preparing && !recompiling && !test_busy;
    const char *title = ui_tr(preparing ? "Preparing shaders" : g_auto_update ? "Updating the game"
                      : recompiling ? "Recompiling the game" : "Setting up the game files");
    const char *const notes[] = {ui_tr("This only needs to be done once and may take a while.")};
    int nnotes = recompiling && !g_auto_update ? 1 : 0;
    float tfs = g_font_menu->FontSize * 0.75f, sfs = g_font_small->FontSize * 0.85f;
    ImVec2 ts = g_font_menu->CalcTextSizeA(tfs, FLT_MAX, 0, title);
    float line_h = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, "A").y;
    float pw = fminf(ds.x * 0.6f, 900 * sc), bar_y = 36 * sc + ts.y + 30 * sc;
    for (int i = 0; i < nnotes; i++)  // (wide enough for its text, within the window)
        pw = fmaxf(pw, g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, notes[i]).x + 96 * sc);
    pw = fminf(fmaxf(pw, ts.x + 96 * sc), ds.x - 32 * sc);
    float ph = bar_y + 24 * sc + 26 * sc + line_h + (nnotes ? 14 * sc + nnotes * line_h : 0) + 40 * sc +
               (preparing ? line_h + 28 * sc : recompiling ? 54 * sc : 0);  // Skip / cancel compilation
    ImVec2 a(o.x + (ds.x - pw) * 0.5f, o.y + (ds.y - ph) * 0.5f), b(a.x + pw, a.y + ph);
    fg->AddRectFilled(a, b, IM_COL32(15, 23, 20, 250), 14 * sc);
    fg->AddRect(a, b, IM_COL32(236, 194, 84, 140), 14 * sc, 0, 1.5f * sc);
    fg->AddText(g_font_menu, tfs, ImVec2((a.x + b.x - ts.x) * 0.5f, a.y + 36 * sc), IM_COL32(236, 194, 84, 255), title);
    ImVec2 ba(a.x + 48 * sc, a.y + bar_y), bb(b.x - 48 * sc, a.y + bar_y + 24 * sc);
    fg->AddRectFilled(ba, bb, IM_COL32(255, 255, 255, 25), 12 * sc);
    if (importing) {
        float x = (bb.x - ba.x) * 0.375f * (1 + sinf((float)ImGui::GetTime() * 2));
        fg->AddRectFilled(ImVec2(ba.x + x, ba.y), ImVec2(ba.x + x + (bb.x - ba.x) * 0.25f, bb.y),
                          IM_COL32(236, 194, 84, 255), 12 * sc);
    } else if (f > 0) fg->AddRectFilled(ba, ImVec2(ba.x + (bb.x - ba.x) * f, bb.y), IM_COL32(236, 194, 84, 255), 12 * sc);
    char line[200];
    if (preparing) snprintf(line, sizeof line, "%d%%", (int)(f * 100));  // (the title says it)
    else if (importing) snprintf(line, sizeof line, "%s", ui_tr(job.c_str()));
    else snprintf(line, sizeof line, "%s   %d%%", ui_tr(test_busy ? "Setting up the game files..." : job.c_str()),
                  (int)(f * 100));
    ImVec2 ls = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, line);
    fg->AddText(g_font_small, sfs, ImVec2((a.x + b.x - ls.x) * 0.5f, bb.y + 26 * sc), IM_COL32(220, 216, 205, 255), line);
    if (recompiling) {
        char cancel[192];
        snprintf(cancel, sizeof cancel, "%s  (Esc / %s)", ui_tr(g_cancel_build ? "Stopping..." : "Cancel compilation"),
                 ui_pad_back_label());
        ImVec2 cs = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, cancel);
        float cw = fmaxf(300 * sc, cs.x + 44 * sc);
        ImVec2 ca((a.x + b.x - cw) * 0.5f, b.y - 66 * sc), cb(ca.x + cw, ca.y + 44 * sc);
        ImGui::SetNextWindowPos(ca);
        ImGui::SetNextWindowSize(ImVec2(cw, 44 * sc));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##build_cancel", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
        if (ImGui::InvisibleButton("##cancel_build", ImVec2(cw, 44 * sc), ImGuiButtonFlags_EnableNav) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            g_cancel_build = true;
        bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        ImGui::End();
        ImGui::PopStyleVar();
        fg->AddRectFilled(ca, cb, hot ? IM_COL32(80, 80, 55, 255) : IM_COL32(45, 55, 45, 255), 8 * sc);
        fg->AddText(g_font_small, sfs, ImVec2((ca.x + cb.x - cs.x) * 0.5f, (ca.y + cb.y - cs.y) * 0.5f),
                    IM_COL32(236, 194, 84, 255), cancel);
    }
    for (int i = 0; i < nnotes; i++) {
        ImVec2 ns = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, notes[i]);
        fg->AddText(g_font_small, sfs, ImVec2((a.x + b.x - ns.x) * 0.5f, bb.y + 26 * sc + (i + 1) * line_h + 14 * sc),
                    IM_COL32(150, 148, 140, 255), notes[i]);
    }
    if (preparing) {  // Skip: the game then compiles what it needs as it goes (back / Esc too)
        const char *sk = ui_tr(g_prep_skip ? "Skipping..." : "Skip");
        ImVec2 ks = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, sk);
        ImVec2 ka((a.x + b.x - ks.x) * 0.5f - 22 * sc, b.y - 40 * sc - line_h - 10 * sc);
        ImVec2 kb(ka.x + ks.x + 44 * sc, ka.y + line_h + 16 * sc);
        bool hover = !g_prep_skip && !g_prep_skip_ask && ImGui::IsMouseHoveringRect(ka, kb, false);
        fg->AddRect(ka, kb, IM_COL32(236, 194, 84, hover ? 255 : 140), 8 * sc, 0, 1.5f * sc);
        fg->AddText(g_font_small, sfs, ImVec2(ka.x + 22 * sc, ka.y + 8 * sc),
                    hover ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255), sk);
        if (!g_prep_skip && !g_prep_skip_ask &&
            ((hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ||
             ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            g_prep_skip_ask = true;
    }
    if (preparing && g_prep_skip_ask && !g_prep_skip) {  // what Skip means, before it happens
        const char *q = ui_tr("Shaders will compile as you play. New effects may briefly pause the game.");
        const char *yes = ui_tr("Yes, skip"), *no = ui_tr("No, wait");
        ImVec2 qs = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, q);
        ImVec2 ys = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, yes), ns = g_font_small->CalcTextSizeA(sfs, FLT_MAX, 0, no);
        float bw = fmaxf(ys.x, ns.x) + 64 * sc, bh = line_h + 20 * sc, gap = 24 * sc;
        float qw = fminf(fmaxf(qs.x, 2 * bw + gap) + 96 * sc, ds.x - 32 * sc), qh = 40 * sc + line_h + 32 * sc + bh + 36 * sc;
        fg->AddRectFilled(a, b, IM_COL32(0, 0, 0, 120), 14 * sc);  // (the bar behind it dimmed)
        ImVec2 qa(o.x + (ds.x - qw) * 0.5f, o.y + (ds.y - qh) * 0.5f), qb(qa.x + qw, qa.y + qh);
        fg->AddRectFilled(qa, qb, IM_COL32(15, 23, 20, 255), 14 * sc);
        fg->AddRect(qa, qb, IM_COL32(236, 194, 84, 200), 14 * sc, 0, 1.5f * sc);
        fg->AddText(g_font_small, sfs, ImVec2((qa.x + qb.x - qs.x) * 0.5f, qa.y + 40 * sc), IM_COL32(236, 232, 220, 255), q);
        float by = qb.y - 36 * sc - bh, bx = (qa.x + qb.x) * 0.5f;
        ImVec2 ya(bx - gap * 0.5f - bw, by), yb(bx - gap * 0.5f, by + bh), na(bx + gap * 0.5f, by), nb(bx + gap * 0.5f + bw, by + bh);
        bool hy = ImGui::IsMouseHoveringRect(ya, yb, false), hn = ImGui::IsMouseHoveringRect(na, nb, false);
        fg->AddRect(ya, yb, IM_COL32(236, 194, 84, hy ? 255 : 140), 8 * sc, 0, 1.5f * sc);
        fg->AddRectFilled(na, nb, IM_COL32(236, 194, 84, hn ? 255 : 210), 8 * sc);  // (waiting: the default)
        fg->AddText(g_font_small, sfs, ImVec2((ya.x + yb.x - ys.x) * 0.5f, ya.y + 10 * sc),
                    hy ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255), yes);
        fg->AddText(g_font_small, sfs, ImVec2((na.x + nb.x - ns.x) * 0.5f, na.y + 10 * sc), IM_COL32(15, 23, 20, 255), no);
        bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        // A / Enter: the highlighted default (wait); B / Esc: also wait; Yes only by clicking it or Y / Space
        if ((hy && click) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false))
            g_prep_skip = 1, g_prep_skip_ask = false;
        else if ((hn && click) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
                 ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                 ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            g_prep_skip_ask = false;
    }
}

static bool g_files_deleted;  // set here: the start screen goes back to setting the game files up
static void profile_picker(float sc) {
    pthread_mutex_lock(&g_lock); bool locked = g_busy; pthread_mutex_unlock(&g_lock);
    locked = locked || gamelib_tables();
    ImGui::BeginDisabled(locked);
    ImGui::SetNextItemWidth(620 * sc);
    const char *label = g_profile < 0 ? "Custom installation" : game_profile_name(g_profile);
    if (ImGui::BeginCombo(ui_label("Game and version").c_str(), ui_tr(label))) {
        for (int i = 0; i < GAME_PROFILE_COUNT; ++i)
            if (ImGui::Selectable(ui_tr(game_profile_name(i)), g_profile == i)) g_profile_request = i;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", ui_tr(locked ? "Restart Multi to switch games." :
        "Each installation keeps its own game files, build cache and Vanilla saves. Existing saves stay with Existing installation."));
    ImGui::Spacing();
}

static void prelaunch_tab(float sc) {
    const ImVec4 gold(0.93f, 0.76f, 0.33f, 1);
    auto heading = [&](const char *t) {
        ImGui::PushFont(g_font_menu);
        ImGui::TextColored(gold, "%s", ui_tr(t));
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
    };
    heading("Game version");
    profile_picker(sc);
    heading("Game files");
    static std::string measured_root;
    static double gb = -1;
    if (measured_root != g_root) { measured_root = g_root; gb = -1; }
    if (gb < 0) gb = rt_game_files_size(g_root.c_str()) / 1e9;
#ifdef _WIN32
    if (ImGui::Button(ui_label("Show in Explorer").c_str())) reveal_folder(g_root);
    if (check_game_files(g_root).empty()) {
        ImGui::SameLine();
        if (ImGui::Button(ui_label("Create desktop shortcut").c_str())) {
            char shortcut[2048], reason[512];
            bool ok = game_shortcut_create(g_root.c_str(), shortcut, sizeof shortcut, reason, sizeof reason);
            set_job(ok ? "Desktop shortcut created." : "Could not create the desktop shortcut.", !ok, ok ? "" : reason);
        }
    }
#elif defined(__linux__)
    if (ImGui::Button(ui_label("Open folder").c_str())) reveal_folder(g_root);
#else
    if (ImGui::Button(ui_label("Show in Finder").c_str())) reveal_folder(g_root);
#endif
    ImGui::SameLine();
    static bool shot_del = getenv("LARECOMP_LAUNCHER_SHOT") && strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":askdelete");
    if (ImGui::Button(ui_label("Delete game files").c_str()) || shot_del) ImGui::OpenPopup("##delfiles"), shot_del = false;  // (test: :askdelete)
    static std::string cache_message;
    if (file_exists(build_cache_root() + "/.gamebuild")) {
        ImGui::SameLine();
        ImGui::BeginDisabled(game_profile_index(g_root.c_str(), rt_data_root()) < 0);
        if (ImGui::Button(ui_label("Clear compilation cache").c_str())) {
            char why[512] = "";
            bool ok = build_cache_clear(build_cache_root().c_str(), why, sizeof why);
            cache_message = ok ? "Compilation cache cleared. Installed game files and saves are kept." : why;
            gb = -1;
        }
        ImGui::EndDisabled();
    }
    if (!cache_message.empty()) ImGui::TextWrapped("%s", ui_tr(cache_message.c_str()));
    if (ImGui::BeginPopupModal("##delfiles", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushFont(g_font_menu);
        ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), "%s", ui_tr("Are you sure?"));
        ImGui::PopFont();
        ImGui::Text(ui_tr("This deletes Multi's copy of the game files (%.1f GB)."), gb);
        ImGui::TextDisabled("%s", ui_tr("Saves, seeds and settings are kept. Your own game files aren't touched."));
        if (rt_dev_mode()) {  // (a source checkout: the files are the project's own extracted/)
            bool abs = !g_root.empty() && (g_root[0] == '/' || (g_root.size() > 1 && g_root[1] == ':'));
            std::string where = abs ? g_root : std::string(rt_data_root()) + "/" + g_root;
            ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), ui_tr("Dev checkout: this is %s"), where.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button(ui_label("Delete").c_str(), ImVec2(220 * sc, 0))) {
            stop_background_prep();
            bool ok = rt_remove_game_files(g_root.c_str());  // (runtime/paths.c: only what the extraction made)
            game_files_changed();
            set_job(ok ? "Game files deleted." : "Could not delete the game files (not in Multi's data folder?).", !ok);
            g_files_deleted = true, gb = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(ui_label("Cancel").c_str(), ImVec2(220 * sc, 0)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Spacing();
    ImGui::Spacing();
    heading("Game font");
    import_fonts();
    bool fs = have_font(FONT_STD, 4), fl = have_font(FONT_LANG, 8), fe = have_font(FONT_EXT, 2);
    ImGui::Text(ui_tr("Text: %s    Chinese / Korean: %s    Button icons: %s"),
                ui_tr(fs ? "Nintendo font (imported)" : "substitute (this OS's, per language)"),
                ui_tr(fl ? "Nintendo font (imported)" : "substitute"), ui_tr(fe ? "imported" : "substitute"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", ui_tr("Fonts aren't included. For the original look, import your own copies of "
                        "nintendo_udsg-r_std_003.bfttf (text), the same-named files for Chinese and Korean "
                        "(nintendo_udsg-r_org_zh-cn_003, nintendo_udsg-r_ext_zh-cn_003, nintendo_udjxh-db_zh-tw_003, "
                        "nintendo_udsg-r_ko_003) and nintendo_ext_003.bfttf (button icons). "
                        ".ttf versions with the same names work too."));
    ImGui::PopTextWrapPos();
    if (!g_font_msg.empty()) ImGui::Text("%s", ui_tr(g_font_msg.c_str()));
    static const SDL_DialogFileFilter font_f[] = {{"Switch fonts", "bfttf;ttf"}};
    if (ImGui::Button(ui_label("Import fonts").c_str())) SDL_ShowOpenFileDialog(picked_fonts, NULL, g_launcher_win, font_f, 1, NULL, true);
    if (fs || fl || fe) {
        ImGui::SameLine();
        if (ImGui::Button(ui_label("Use substitute font").c_str())) remove_fonts();
    }
}

extern "C" const char *launcher_run(const char *root) {
    char resolved[2048], resolve_error[512];
    g_root = game_install_resolve(root, resolved, sizeof resolved, resolve_error, sizeof resolve_error) ? resolved : root;
    root = g_root.c_str();
    g_profile = game_profile_index(root, rt_data_root());
    game_profile_activate(root, rt_data_root());
    char profile_error[512];
    if (!title_profile_select(root, profile_error, sizeof profile_error))
        title_profile_preview(game_profile_title(g_profile), game_profile_version(g_profile));
    save_backups_set_game_root(root);
    rando_set_game_root(root);
    mods_set_game_root(root);
    g_start_screen = 1;
    static bool update_checked;  // (once per launch, not again after a "Title screen" reboot)
    if (!update_checked && !getenv("LARECOMP_NO_UPDATE_CHECK")) {
        update_checked = true;
        pthread_t t;
        if (pthread_create(&t, NULL, update_thread, NULL) == 0) pthread_detach(t);
    }
    ui_window_init();
    SDL_Window *win = ui_window();
    g_launcher_win = win;
#ifdef __APPLE__
    CAMetalLayer *layer = (__bridge CAMetalLayer *)ui_metal_layer();
    id<MTLDevice> dev = (__bridge id<MTLDevice>)ui_metal_device();
    id<MTLCommandQueue> queue = [dev newCommandQueue];
#else
    GpuDevice dev = NULL;
#endif

    IMGUI_CHECKVERSION();
    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();  // the in-game menu's, when back here after a reboot
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;
    // fonts belong to this context: back here via "Title screen" (in-place reboot) the previous run's are gone with
    // its context, so load them again (a stale pointer here aborted in PushFont - user, 0.1 RC)
    g_font_small = g_font_menu = g_font_title = NULL;
    // Baloo 2 SemiBold (OFL, shipped: third_party/fonts/Baloo2) - rounded, game-like; it runs small, hence the sizes
    char baloo[1024];
    std::string own = rt_resource("Baloo2-SemiBold.ttf", "third_party/fonts/Baloo2/Baloo2-SemiBold.ttf", baloo, sizeof baloo) ? baloo : "";
    const char *fonts[] = {getenv("LARECOMP_LAUNCHER_FONT") ? getenv("LARECOMP_LAUNCHER_FONT") : "",  // (test)
                           own.c_str(), "fonts/nintendo_udsgr_std_003.ttf", "/System/Library/Fonts/Avenir Next.ttc",
                           "/System/Library/Fonts/Helvetica.ttc", "C:/Windows/Fonts/segoeui.ttf"};
    static const ImWchar ranges[] = {0x20, 0xFF, 0x2039, 0x203A, 0x2192, 0x2192, 0x2713, 0x2713, 0x25B8, 0x25B8, 0};  // + ‹ › → ✓ ▸
    for (const char *p : fonts) {
        if (g_font_menu || !*p || !file_exists(p)) continue;
        bool b2 = p == own.c_str() || getenv("LARECOMP_LAUNCHER_FONT");
        g_font_small = io.Fonts->AddFontFromFileTTF(p, 30 * (b2 ? 1.75f : 1), NULL, ranges);
        g_font_menu = io.Fonts->AddFontFromFileTTF(p, 52 * (b2 ? 1.85f : 1), NULL, ranges);
        g_font_title = io.Fonts->AddFontFromFileTTF(p, 92 * (b2 ? 2.3f : 1), NULL, ranges);
    }
    if (!g_font_menu) g_font_small = g_font_menu = g_font_title = io.Fonts->AddFontDefault();
    // The large title font only draws the Latin logo; keep CJK in the actual UI fonts.
    for (ImFont *font : {g_font_small, g_font_menu}) ui_merge_language_font(font);
    ui_add_user_font();
    ImGuiStyle &st = ImGui::GetStyle();
    st.Colors[ImGuiCol_Header] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.08f);
    st.Colors[ImGuiCol_HeaderActive] = ImVec4(1, 1, 1, 0.14f);
    st.Colors[ImGuiCol_NavCursor] = ImVec4(0.95f, 0.78f, 0.35f, 1);
    st.FrameRounding = st.WindowRounding = 10;
#ifdef __APPLE__
    ImGui_ImplSDL3_InitForMetal(win);
    ImGui_ImplMetal_Init(dev);
#else
    ImGui_ImplSDL3_InitForVulkan(win);
    vk_imgui_backend_init();
#endif
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, NULL, 0);  // (controllers: launcher_pad_event)
    launcher_pad_open_all();
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    // an older version's game data image: the game reads the romfs/ folder itself now - 5.8 GB back
    const TitleProfile *startup_title = title_profile_find((g_root + "/exefs/main").c_str());
    if (startup_title && !strcmp(startup_title->key, "las") && check_copied_files(g_root).empty() && file_exists(g_root + "/romfs.bin") &&
        rt_remove_game_file(g_root.c_str(), "romfs.bin"))
        fprintf(stderr, "[launcher] removed the old game data image (romfs.bin)\n");
    std::string status = check_game_files(g_root);
    game_version_select((g_root + "/exefs/main").c_str());
    if (status.empty()) load_background(dev);
    std::vector<std::string> seeds = list_seeds();
    std::vector<std::string> choices = {"Vanilla"};
    choices.insert(choices.end(), seeds.begin(), seeds.end());
    int choice = 0;
    for (size_t i = 0; i < seeds.size(); i++)
        if (seeds[i] == g_settings.rando_seed) choice = (int)i + 1;
    Page page = status.empty() ? MAIN : SETUP;
    bool play = false;
    // LARECOMP_LAUNCHER_SHOT=<frame>[:<page>] (test): save build/launcher.png at that frame and exit; page: settings[:<tab>] / askdelete / setup / extracting / preparing
    long shot_at = getenv("LARECOMP_LAUNCHER_SHOT") ? atol(getenv("LARECOMP_LAUNCHER_SHOT")) : 0, frame = 0;
    if (shot_at) {
#ifdef __APPLE__
        layer.framebufferOnly = NO;
#endif
        const char *pg = strchr(getenv("LARECOMP_LAUNCHER_SHOT"), ':');
        if (pg && (!strncmp(pg + 1, "settings", 8) || !strcmp(pg + 1, "askdelete"))) page = SETTINGS, menu_tabs_focus(true);
        if (pg && !strcmp(pg + 1, "setup")) page = SETUP;
        if (pg && !strcmp(pg + 1, "preparing")) page = SETUP, g_prep_frac = 0.42f, set_job("Preparing shaders");
        if (pg && !strcmp(pg + 1, "skipask")) page = SETUP, g_prep_frac = 0.42f, set_job("Preparing shaders"), g_prep_skip_ask = true;
        if (pg && !strncmp(pg + 1, "fail", 4)) page = SETUP, set_failure(atoi(pg + 5));  // (the failure messages)
        if (pg && !strncmp(pg + 1, "settings", 8) && pg[9] == ':') g_tab_request_ext = atoi(pg + 10);  // settings:<tab>
        if (pg && !strcmp(pg + 1, "delete")) page = SETUP, g_confirm_delete = true;
        if (pg && !strcmp(pg + 1, "tip")) g_settings.menu_tip_seen = false, g_tip_test = true;
        if (pg && !strcmp(pg + 1, "aspect")) g_settings.aspect_asked = g_settings.menu_tip_seen = false, g_tip_test = true;
    }

    static int runs;  // (test: =once - only the first time, so a "Title screen" reboot lands on the start screen)
    const char *ap = getenv("LARECOMP_LAUNCHER_AUTOPLAY");
    if (ap && status.empty() && !(runs && !strcmp(ap, "once"))) play = true;  // (test: PLAY at once)
    if (getenv("LARECOMP_LAUNCHER_PREPARE") && status.empty() && !runs) {  // (test) the setup's shader preparation
        g_busy = true;                                                      // alone, then PLAY with AUTOPLAY
        page = SETUP, play = false;
        pthread_t th;
        pthread_create(&th, NULL, [](void *) -> void * {
            prepare_shaders();
            set_job("Game files ready.");
            pthread_mutex_lock(&g_lock);
            g_busy = false;
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }, NULL);
        pthread_detach(th);
    }
    runs++;
    if (const char *t = SDL_getenv("LARECOMP_IMPORT_KEYS")) g_keys = t;
    if (const char *t = SDL_getenv("LARECOMP_IMPORT_TITLE_KEYS")) g_title_keys = t;
    if (const char *t = SDL_getenv("LARECOMP_LAUNCHER_SOURCE")) g_src = t; // screenshot/test selection, no import
    if (const char *t = SDL_getenv("LARECOMP_LAUNCHER_UPDATE_NSZ")) g_update_archive = t;  // UTF-8, including on Windows
    if (getenv("LARECOMP_LAUNCHER_INSPECT") && is_archive(g_src)) { // test the real metadata worker and its UI
        g_busy = true; page = SETUP; play = false;
        pthread_t th;
        if (!pthread_create(&th, NULL, inspect_thread, NULL)) pthread_detach(th);
        else g_busy = false;
    }
    const char *setup_source = SDL_getenv("LARECOMP_LAUNCHER_EXTRACT");
    if (getenv("LARECOMP_LAUNCHER_BUILD_ONLY")) setup_source = ""; // run the normal resume action, without reimporting
    bool exit_after_setup = setup_source != NULL;  // (test) set up, log the result, exit
    if (!setup_source) setup_source = SDL_getenv("LARECOMP_LAUNCHER_SETUP");
    if (getenv("LARECOMP_LAUNCHER_MINIMIZED")) SDL_MinimizeWindow(win); // background setup regression
    if (setup_source) {
        // Automated imports use the same progress screen as interactive setup.
        // Pumping events alone leaves a black window throughout recompilation.
        g_src = setup_source;
        g_busy = true;
        page = SETUP;
        play = false;
        pthread_t th;
        if (pthread_create(&th, NULL, extract_thread, NULL)) {
            fprintf(stderr, "[launcher] could not start setup thread\n");
            plat_exit(1);
        }
        pthread_detach(th);
    }
    // An update: the game files are there and only the game code is out of date - rebuilt at once, by itself (only
    // the pieces that changed: runtime/gamebuild.c), with the progress bar; then the start screen as usual. A failure
    // stays on the setup page with what went wrong.
    if (!g_busy && !status.empty() && check_copied_files(g_root).empty() && gamebuild_available() &&
        !getenv("LARECOMP_LAUNCHER_DELETE")) {
        fprintf(stderr, "[launcher] game code out of date: updating it\n");
        g_src.clear();
        g_busy = true, g_auto_update = true;
        page = SETUP;
        pthread_t th;
        pthread_create(&th, NULL, extract_thread, NULL);
        pthread_detach(th);
    }
    if (getenv("LARECOMP_LAUNCHER_DELETE")) {  // (test) what DELETE GAME FILES does, logged; then the start screen
        double gb = rt_game_files_size(g_root.c_str()) / 1e9;
        stop_background_prep();
        bool ok = rt_remove_game_files(g_root.c_str());
        if (ok && g_profile >= 0) g_root = game_profile_dir(g_profile);
        game_files_changed();
        status = check_game_files(g_root);
        fprintf(stderr, "[launcher] delete %.1f GB: %s -> %s\n", gb, ok ? "ok" : "refused", status.c_str());
        if (!status.empty()) { g_bg = 0; page = SETUP; }
    }
    // the shader preparation of every later start: in the background (first setups and updates do theirs above)
    if (status.empty() && !g_busy && !getenv("LARECOMP_LAUNCHER_PREPARE")) start_background_prep();
    // (test) LARECOMP_LAUNCHER_PLAY_AT=<s>: PLAY that many seconds in; LARECOMP_LAUNCHER_SKIP_AT=<s>: Skip then
    const char *play_at_env = getenv("LARECOMP_LAUNCHER_PLAY_AT"), *skip_at_env = getenv("LARECOMP_LAUNCHER_SKIP_AT");
    uint64_t loop_t0 = SDL_GetTicks(), last_frame = loop_t0, bg_gap = 0, bg_frames = 0;
    bool bg_logged = false;
    for (;;) {
        uint64_t now = SDL_GetTicks();
        if (g_bg_prep) bg_frames++, bg_gap = std::max(bg_gap, now - last_frame);
        else if (!bg_logged && bg_frames) {  // (how the start screen kept up meanwhile)
            bg_logged = true;
            fprintf(stderr, "[launcher] background shader preparation: %llu frames meanwhile, longest %llu ms\n",
                    (unsigned long long)bg_frames, (unsigned long long)bg_gap);
        }
        last_frame = now;
        if (play_at_env && status.empty() && now - loop_t0 >= (uint64_t)(atof(play_at_env) * 1000)) play = true, play_at_env = NULL;
        if (skip_at_env && g_prep_wait && now - loop_t0 >= (uint64_t)(atof(skip_at_env) * 1000)) g_prep_skip = 1, skip_at_env = NULL;
        // PLAY while that still runs: its bar (with Skip) until it's done
        if (play && g_bg_prep) {
            play = false, g_prep_wait = true;
            fprintf(stderr, "[launcher] PLAY: waiting for the shader preparation\n");
        }
        if (g_prep_wait && !g_bg_prep) {
            g_prep_wait = false, play = true;
            fprintf(stderr, "[launcher] shader preparation %s: playing\n", g_prep_skip ? "skipped" : "done");
        }
        if (play) break;
        menu_apply_window_requests(win);  // (the Video tab's Fullscreen)
        // the seed PLAY would start, kept current (Settings > Gameplay greys out what that seed forces)
        snprintf(g_settings.rando_seed, sizeof g_settings.rando_seed, "%s", choice ? choices[choice].c_str() : "");
        workshop_selection(g_root.c_str(),g_settings.rando_seed);
        if (g_files_deleted) {  // (Settings > Prelaunch deleted them)
            g_files_deleted = false;
            if (g_profile >= 0) g_root = game_profile_dir(g_profile);
            status = check_game_files(g_root);
            if (!status.empty()) { g_bg = 0; bind_capture_cancel(); settings_save(); page = SETUP; }
        }
        if (g_play_seed_req) {  // the Randomizer tab's Play: that seed, then PLAY as if pressed (first-launch questions)
            g_play_seed_req = false;
            seeds = list_seeds();
            choices = {"Vanilla"};
            choices.insert(choices.end(), seeds.begin(), seeds.end());
            choice = 0;
            for (size_t i = 0; i < seeds.size(); i++)
                if (seeds[i] == g_play_seed) choice = (int)i + 1;
            bind_capture_cancel();
            settings_save();
            page = MAIN;
            g_play_pending = true;
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (page == SETTINGS && bind_capture_event(&e)) continue;  // (waiting for a new binding: controls_ui.mm)
            if (!launcher_pad_event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) plat_exit(exit_after_setup ? 1 : 0);
        }
        // extraction finished: re-check, load the background
        pthread_mutex_lock(&g_lock);
        bool busy = g_busy;
        bool published = !busy && g_install_published;
        if (published) g_install_published = false;
        bool setup_failed = g_job_failed, inspection = g_job_inspection;
        bool auto_selected = !busy && g_detected_profile >= 0;
        if (auto_selected) { g_profile_request = g_detected_profile; g_detected_profile = -1; }
        if (!busy && g_speedfix_ready_profile>=0) {
            g_settings.acnh_speedfix60=true;
            g_profile_request=g_speedfix_ready_profile;g_speedfix_ready_profile=-1;
        }
        bool inspect_now = !busy && g_pending_inspect && !gamelib_tables();
        if (inspect_now) { g_pending_inspect = false; g_busy = busy = true; }
        pthread_mutex_unlock(&g_lock);
        if (inspect_now) {
            pthread_t thread;
            if (!pthread_create(&thread, NULL, inspect_thread, NULL)) pthread_detach(thread);
            else { pthread_mutex_lock(&g_lock); g_busy = busy = false; pthread_mutex_unlock(&g_lock); }
        }
        if (published) {
            g_root = game_profile_dir(g_profile);
            game_files_changed();
            game_profile_activate(g_root.c_str(), rt_data_root());
            char profile_error[512];
            if (!title_profile_select(g_root.c_str(), profile_error, sizeof profile_error))
                title_profile_preview(game_profile_title(g_profile), game_profile_version(g_profile));
            save_backups_set_game_root(g_root.c_str());
            rando_set_game_root(g_root.c_str());
            mods_set_game_root(g_root.c_str());
            game_version_select((g_root + "/exefs/main").c_str());
            status = check_game_files(g_root);
        }
        if (g_profile_request >= 0 && !busy && !gamelib_tables()) {
            stop_background_prep();
            g_profile = g_profile_request; g_profile_request = -1;
            g_settings.game_profile = g_profile;
            g_root = game_profile_dir(g_profile);
            game_files_changed();
            game_profile_activate(g_root.c_str(), rt_data_root());
            char profile_error[512];
            if (!title_profile_select(g_root.c_str(), profile_error, sizeof profile_error))
                title_profile_preview(game_profile_title(g_profile), game_profile_version(g_profile));
            save_backups_set_game_root(g_root.c_str());
            rando_set_game_root(g_root.c_str());
            mods_set_game_root(g_root.c_str());
            game_version_select((g_root + "/exefs/main").c_str());
            status = check_game_files(g_root);
            seeds = list_seeds(); choices = {"Vanilla"}; choices.insert(choices.end(), seeds.begin(), seeds.end()); choice = 0;
            g_settings.rando_seed[0] = 0;
            pthread_mutex_lock(&g_lock);
            if (!auto_selected) {
                g_src.clear(); g_update_archive.clear(); g_inspected = false; g_job.clear(); g_job_hint.clear();
            }
            pthread_mutex_unlock(&g_lock);
            g_bg = 0;
            if (status.empty()) { load_background(dev); start_background_prep(); }
            settings_save();
            g_confirm_delete = false;
        }
        if (exit_after_setup && !busy) {
            fprintf(stderr, "[launcher] setup: %s -> %s\n", g_job.c_str(), check_game_files(g_root).empty() ? "READY" : "NOT READY");
            if (!g_job_hint.empty()) fprintf(stderr, "[launcher] details: %s\n", g_job_hint.c_str());
            plat_exit(setup_failed || !check_game_files(g_root).empty() ? 1 : 0);
        }
        static bool was_busy;
        if (was_busy && !busy) g_build_cache_changed = true;
        if (was_busy && !busy && !inspection) {
            status = check_game_files(g_root);
            game_version_select((g_root + "/exefs/main").c_str());
            seeds = list_seeds();
            choices = {"Vanilla"};
            choices.insert(choices.end(), seeds.begin(), seeds.end());
            choice = 0;
            if (status.empty() && !setup_failed) { load_background(dev); page = MAIN; start_background_prep(); }
            if (status.empty() && ap && getenv("LARECOMP_LAUNCHER_PREPARE")) play = true;
            g_auto_update = false;
        }
        was_busy = busy;

        // SDL reports a zero ImGui display size while minimized even when the
        // Vulkan swapchain retains its old extent. Keep polling the worker and
        // events, but do not acquire a frame or create zero-sized UI controls.
        ImGui_ImplSDL3_NewFrame();
        if (io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) { SDL_Delay(16); continue; }
#ifdef __APPLE__
        int w, h;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        layer.drawableSize = CGSizeMake(w, h);
        @autoreleasepool {
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) { SDL_Delay(16); continue; }
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0.04, 0.05, 0.06, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            ImGui_ImplMetal_NewFrame(rp);
#else
        {
            // one frame per refresh of the window's display: the swapchain presents without waiting (mailbox, for the
            // game's own pacing: device.c), so the start screen alone would draw as fast as the GPU goes
            static uint64_t next_frame;
            const SDL_DisplayMode *dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(win));
            uint64_t period = (uint64_t)(1e9 / (dm && dm->refresh_rate > 1 ? dm->refresh_rate : 60.0f)), now = plat_now_ns();
            bool on_time = next_frame > now && next_frame - now <= period;  // (else late, or the rate changed: start over)
            if (on_time) plat_sleep_until_ns(next_frame);
            next_frame = (on_time ? next_frame : now) + period;
            static const float clear_col[4] = {0.04f, 0.05f, 0.06f, 1};
            if (!vk_frame_begin(clear_col)) { SDL_Delay(16); continue; }
            ImGui_ImplVulkan_NewFrame();
#endif
            // the backend (manual mode, no gamepads of its own) clears HasGamepad every frame, and ImGui then drops the
            // controller presses launcher_pad_event fed in and turns gamepad navigation off: set it again while a
            // controller is connected
            if (SDL_HasGamepad()) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            ImGui::NewFrame();

            ImVec2 ds = io.DisplaySize;
            float s = ds.y / 720.0f;  // layout scale (fonts are built for ~2x)
            ImGui::GetIO().FontGlobalScale = s * 0.5f;
            ImDrawList *bg = ImGui::GetBackgroundDrawList();
            if (g_bg) {  // cover the window, keep the aspect
                float ia = (float)g_bg_w / g_bg_h, wa = ds.x / ds.y;
                ImVec2 uv0(0, 0), uv1(1, 1);
                if (wa > ia) { float v = ia / wa; uv0.y = (1 - v) / 2; uv1.y = 1 - uv0.y; }
                else { float u = wa / ia; uv0.x = (1 - u) / 2; uv1.x = 1 - uv0.x; }
                bg->AddImage(BG_TEXID, ImVec2(0, 0), ds, uv0, uv1);
            } else {
                bg->AddRectFilledMultiColor(ImVec2(0, 0), ds, IM_COL32(18, 34, 40, 255), IM_COL32(40, 70, 64, 255),
                                            IM_COL32(12, 20, 24, 255), IM_COL32(8, 12, 16, 255));
            }
            {  // a shadow banner fading in from the left behind the menu, and one from the right behind the version
                const float stops[][2] = {{0.0f, 225}, {0.3f, 215}, {0.44f, 165}, {0.55f, 95}, {0.64f, 35}, {0.74f, 0}};
                for (int i = 0; i + 1 < (int)(sizeof stops / sizeof stops[0]); i++) {
                    ImU32 a = IM_COL32(6, 10, 34, (int)stops[i][1]), b = IM_COL32(6, 10, 34, (int)stops[i + 1][1]);
                    bg->AddRectFilledMultiColor(ImVec2(ds.x * stops[i][0], 0), ImVec2(ds.x * stops[i + 1][0], ds.y), a, b, b, a);
                }
                float y0 = ds.y - 190 * s, y1 = ds.y - 135 * s, y2 = ds.y - 40 * s, y3 = ds.y, x0r = ds.x * 0.5f;
                ImU32 c0 = IM_COL32(6, 10, 34, 0), c1 = IM_COL32(6, 10, 34, 235);
                bg->AddRectFilledMultiColor(ImVec2(x0r, y0), ImVec2(ds.x, y1), c0, c0, c1, c0);  // (feathered top)
                bg->AddRectFilledMultiColor(ImVec2(x0r, y1), ImVec2(ds.x, y2), c0, c1, c1, c0);
                bg->AddRectFilledMultiColor(ImVec2(x0r, y2), ImVec2(ds.x, y3), c0, c1, c0, c0);  // (feathered bottom)
            }

            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ds);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
            ImGui::Begin("##launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
            const bool waiting = g_prep_wait;  // (the menu waits under the bar)
            ImGui::BeginDisabled(waiting);
            ImGui::SetScrollX(0);  // (the start screen never scrolls; a long seed name or a stray nav move mustn't shift it)
            ImGui::SetScrollY(0);
            float x0 = 90 * s;
            ImGui::SetCursorPos(ImVec2(x0, 120 * s));
            ImGui::PushFont(g_font_title);
            if (page == MAIN) {  // gold to orange, shadowed (not behind the Settings / Game Files windows)
                const char *t = "Multi";
                ImVec2 p = ImGui::GetCursorScreenPos();
                text_fx(ImGui::GetWindowDrawList(), ImGui::GetFont(), ImGui::GetFontSize(), p, IM_COL32(255, 232, 120, 255),
                        IM_COL32(255, 138, 48, 255), t, 3 * s);
                ImGui::Dummy(ImGui::CalcTextSize(t));
            }
            ImGui::PopFont();
            ImGui::SetCursorPos(ImVec2(x0, 285 * s));
            // typing in a text field / the on-screen keyboard (osk.h): B / Esc are for it, not "back" (from the frame
            // before too, so the Esc that ends the typing doesn't also leave the page)
            static bool typed_before;
            bool typing = typed_before || ui_text_busy();

            if (page == MAIN) {
                {  // the four items close together, the block anchored where QUIT always was (the gap to the status
                   // line stays)
                    ImGui::PushFont(g_font_menu);
                    float h = ImGui::CalcTextSize("A").y, sp = ImGui::GetStyle().ItemSpacing.y;
                    ImGui::PopFont();
                    auto pitch = [&](float gap) { return h + 6 + gap * h + (gap >= 0 ? 2 : 1) * sp; };
                    g_menu_gap = -0.18f;
                    ImGui::SetCursorPosY(285 * s + 3 * pitch(0.35f) - 3 * pitch(g_menu_gap));
                }
                bool hov = false;
                char label[192];
                snprintf(label, sizeof label, "%s    \xe2\x80\xb9 %s%s \xe2\x80\xba", ui_tr("PLAY"), choice ? ui_tr("Rando: ") : "",
                         choice ? choices[choice].c_str() : ui_tr("Vanilla"));
                ImGui::SetCursorPosX(x0);
                // a long seed name may run past the other items (to 80% of the window); beyond that the line shrinks
                ImGui::PushFont(g_font_menu);
                float fit = (ds.x * 0.8f - x0) / ImGui::CalcTextSize(label).x;
                ImGui::PopFont();
                if (fit < 1) ImGui::SetWindowFontScale(fit < 0.55f ? 0.55f : fit);
                // the ‹ › arrows are clickable with the mouse: they cycle Vanilla / seeds instead of starting the game
                int arrow = 0;
                {
                    ImGui::PushFont(g_font_menu);
                    ImVec2 at = ImGui::GetCursorScreenPos();
                    const char *lt = strstr(label, "\xe2\x80\xb9"), *rt = strstr(label, "\xe2\x80\xba");
                    float h = ImGui::CalcTextSize(label).y, pad = h * 0.35f;
                    float lx = at.x + ImGui::CalcTextSize(label, lt).x, rx = at.x + ImGui::CalcTextSize(label, rt).x;
                    float aw = ImGui::CalcTextSize("\xe2\x80\xb9").x;
                    ImVec2 m = ImGui::GetIO().MousePos;
                    bool in_y = m.y >= at.y - pad && m.y <= at.y + h + pad;
                    if (in_y && m.x >= lx - pad && m.x <= lx + aw + pad) arrow = -1;
                    if (in_y && m.x >= rx - pad && m.x <= rx + aw + pad) arrow = 1;
                    ImGui::PopFont();
                }
                static bool focused_once;
                if (!focused_once) { ImGui::SetKeyboardFocusHere(); focused_once = true; }
                ImVec2 play_at = ImGui::GetCursorScreenPos();
                const char *name = strstr(label, "\xe2\x80\xb9") + 4, *name_end = strstr(label, " \xe2\x80\xba");
                bool pressed = menu_item(label, status.empty(), &hov, name,
                                         choice ? IM_COL32(250, 250, 255, 255) : IM_COL32(246, 232, 176, 255),
                                         name_end);  // (the choice keeps its colour when hovered: "Rando: <seed>" white,
                                                     //  "Vanilla" vanilla cream; the arrows stay white)
                if (status.empty()) {  // PLAY twinkles now and then while the game is ready
                    ImGui::PushFont(g_font_menu);
                    float w = ImGui::CalcTextSize("PLAY").x, h = ImGui::CalcTextSize("PLAY").y;
                    ImGui::PopFont();
                    float t = (float)ImGui::GetTime(), ph = fmodf(t, 3.6f), ph2 = fmodf(t + 0.45f, 3.6f);
                    float k1 = ph < 0.9f ? sinf(ph / 0.9f * 3.14159265f) : 0, k2 = ph2 < 0.7f ? sinf(ph2 / 0.7f * 3.14159265f) : 0;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    sparkle(dl, ImVec2(play_at.x + w + h * 0.05f, play_at.y + h * 0.22f), h * 0.2f, k1, t * 0.6f);
                    sparkle(dl, ImVec2(play_at.x + w - h * 0.32f, play_at.y + h * 0.08f), h * 0.11f, k2 * 0.8f, -t * 0.8f);
                }
                ImGui::SetWindowFontScale(1.0f);
                if (pressed && arrow && ImGui::IsMouseReleased(0)) {  // (a mouse click on an arrow: cycle)
                    choice = (choice + arrow + (int)choices.size()) % (int)choices.size();
                    pressed = false;
                }
                // first PLAY ever: the aspect ratio question, then how to reach our menu, then the game
                if (g_play_pending && status.empty()) pressed = true, g_play_pending = false;
                if (pressed) {
                    if (!g_settings.aspect_asked) ImGui::OpenPopup("##aspect");
                    else if (!g_settings.menu_tip_seen) ImGui::OpenPopup("##menutip");
                    else play = true;
                }
                if (g_tip_test) ImGui::OpenPopup(g_settings.aspect_asked ? "##menutip" : "##aspect"), g_tip_test = false;
                if (aspect_prompt(s)) {
                    if (g_settings.menu_tip_seen) play = true;
                    else ImGui::OpenPopup("##menutip");
                }
                if (menu_tip(s)) play = true;
                if (hov && status.empty()) {
                    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft))
                        choice = (choice + (int)choices.size() - 1) % (int)choices.size();
                    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight))
                        choice = (choice + 1) % (int)choices.size();
                    // left / right are ours while PLAY is selected: ImGui's navigation would otherwise take them too,
                    // find nothing beside PLAY and scroll the window (PLAY loses its highlight, the cycling stops)
                    ImGuiID own = ImGui::GetID("##play_lr");
                    for (ImGuiKey k : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight})
                        ImGui::SetKeyOwner(k, own);
                }
                ImGui::SetCursorPosX(x0);
                if (g_refocus == SETTINGS) ImGui::SetKeyboardFocusHere(), g_refocus = MAIN;  // (back from Settings: it
                if (menu_item("GAME VERSION", !busy)) { menu_tabs_focus(true); page = SETTINGS; }
                ImGui::SetCursorPosX(x0);
                if (menu_item("SETTINGS", true)) {                                             //  stays selected)
                    static bool opened;
                    menu_tabs_focus(!opened);  // (first time this session: Prelaunch; later: the selected tab)
                    opened = true;
                    page = SETTINGS;
                }
                if (!status.empty() || busy) {  // (once set up, the game files are kept up in Settings > Prelaunch)
                    ImGui::SetCursorPosX(x0);
                    if (g_refocus == SETUP) ImGui::SetKeyboardFocusHere(), g_refocus = MAIN;
                    if (menu_item("SET UP GAME FILES", !busy)) page = SETUP;
                }
                ImGui::SetCursorPosX(x0);
                if (menu_item("QUIT", true)) plat_exit(0);
                g_menu_gap = 0.35f;
            } else if (page == SETTINGS) {
                // the in-game menu's tabs (runtime/ui/menu.mm), in a window like the in-game one; tabs that need a
                // loaded game say so
                float sc = ds.y / 1080.0f, w = fminf(ds.x * 0.9f, 1500 * sc), h = ds.y - 2 * 18 * sc;  // (all the height: a slim margin)
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ds.x,
                                                          ImGui::GetWindowPos().y + ds.y), IM_COL32(0, 0, 0, 110));
                ImVec2 wp = ImGui::GetWindowPos();
                ImGui::SetCursorScreenPos(ImVec2(wp.x + (ds.x - w) * 0.5f, wp.y + (ds.y - h) * 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.09f, 0.08f, 0.96f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * sc);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * sc, 24 * sc));
                ImGui::BeginChild("##settings", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NavFlattened);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                ImGui::PushFont(g_font_small);
                ImGui::PushFont(g_font_menu);
                ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", ui_tr("Settings"));
                ImGui::PopFont();
                bool back;
                {  // close: a drawn X in the corner (B / Esc too)
                    float bx = 44 * sc;
                    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
                    back = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
                    ImGui::PopStyleColor();
                    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
                    ImU32 col = hot ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255);
                    float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * sc * 1.5f;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * sc);
                    dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
                    dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
                }
                bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);  // (before the pages run, like the menu)
                menu_tabs_draw(sc, g_font_small, g_font_menu, false, prelaunch_tab);
                int cap_dev;
                bind_capture_active(&cap_dev, NULL, NULL);
                if (!popup && !typing && cap_dev < 0 && !controls_ui_took_back() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                    back = true;
                ImGui::PopFont();
                ImGui::EndChild();
                if (back) {
                    bind_capture_cancel();
                    settings_save();
                    page = MAIN, g_refocus = SETTINGS;
                    std::string cur = choices[choice];  // seeds made / renamed / deleted in the Randomizer tab
                    seeds = list_seeds();
                    choices = {"Vanilla"};
                    choices.insert(choices.end(), seeds.begin(), seeds.end());
                    choice = 0;
                    for (size_t i = 0; i < choices.size(); i++)
                        if (choices[i] == cur) choice = (int)i;
                }
            } else {
                // SETUP: the game files, in a window like Settings (option rows: runtime/ui/option_rows.h) - choose the
                // folder with the game files, set up; once set up, delete. Each row's explanation on the right.
                float sc = ds.y / 1080.0f, w = fminf(ds.x * 0.9f, 1500 * sc), h = ds.y - 2 * 18 * sc;
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetWindowPos(), ImVec2(ImGui::GetWindowPos().x + ds.x,
                                                          ImGui::GetWindowPos().y + ds.y), IM_COL32(0, 0, 0, 110));
                ImVec2 wp = ImGui::GetWindowPos();
                ImGui::SetCursorScreenPos(ImVec2(wp.x + (ds.x - w) * 0.5f, wp.y + (ds.y - h) * 0.5f));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.09f, 0.08f, 0.96f));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * sc);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * sc, 24 * sc));
                ImGui::BeginChild("##setup", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NavFlattened);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                ImGui::PushFont(g_font_small);
                ImGui::PushFont(g_font_menu);
                ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1), "%s", ui_tr("Game Files"));
                ImGui::PopFont();
                bool back = false;
                if (!busy) {  // close: the drawn X in the corner, as Settings (B / Esc too)
                    float bx = 44 * sc;
                    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bx);
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
                    back = ImGui::InvisibleButton("##close", ImVec2(bx, bx), ImGuiButtonFlags_EnableNav);
                    ImGui::PopStyleColor();
                    bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
                    ImU32 col = hot ? IM_COL32(236, 194, 84, 255) : IM_COL32(220, 216, 205, 255);
                    float m = bx * 0.3f, t = (hot ? 3.0f : 2.4f) * sc * 1.5f;
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    if (hot) dl->AddRectFilled(p, ImVec2(p.x + bx, p.y + bx), IM_COL32(255, 255, 255, 20), 8 * sc);
                    dl->AddLine(ImVec2(p.x + m, p.y + m), ImVec2(p.x + bx - m, p.y + bx - m), col, t);
                    dl->AddLine(ImVec2(p.x + bx - m, p.y + m), ImVec2(p.x + m, p.y + bx - m), col, t);
                }
                ImGui::Spacing();

                pthread_mutex_lock(&g_lock);
                std::string src = g_src, update_archive = g_update_archive, keys = g_keys, title_keys = g_title_keys,
                            job = g_job, job_hint = g_job_hint;
                GameArchiveInfo info[2]; memcpy(info, g_archive_info, sizeof info);
                bool inspected = g_inspected;
                bool failed = g_job_failed;
                pthread_mutex_unlock(&g_lock);
                bool ready = status.empty() && !busy;  // (set up: the window offers to delete them)
                static double files_gb = -1;
                // chosen: "Selected" with a green check (long paths ran over the label); the path is in the
                // explanation, and choosing again opens the picker again
                bool source_is_archive = is_archive(src);
                std::string src_v = src.empty() || source_is_archive ? "Choose..." : "Selected";
                auto check_left_of_value = [&](const std::string &v) {
                    if (v != "Selected") return;
                    ImVec2 rb = ImGui::GetItemRectMax(), ra = ImGui::GetItemRectMin();
                    float fs = ImGui::GetFontSize(), vw = ImGui::CalcTextSize(ui_tr(v.c_str())).x;
                    float x = rb.x - 16 * sc - vw - fs * 0.95f, cy = (ra.y + rb.y) * 0.5f, u = fs * 0.62f;
                    ImU32 g = IM_COL32(120, 220, 120, 255);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    dl->AddLine(ImVec2(x, cy), ImVec2(x + u * 0.35f, cy + u * 0.35f), g, fs * 0.11f);
                    dl->AddLine(ImVec2(x + u * 0.35f, cy + u * 0.35f), ImVec2(x + u, cy - u * 0.4f), g, fs * 0.11f);
                };
                std::string src_d = "Choose your extracted game folder containing exefs and romfs. The game is identified from its executable modules.";
                src_d = ui_tr(src_d.c_str());
                if (!src.empty() && !source_is_archive) src_d += "\n\n" + src;
                // the step that's running / its result, under the rows and in the Set up row's explanation
                const std::string &job_line = job;
                std::string ex_d = "Copies the game files Multi needs and recompiles the game from them. This only "
                                   "needs to be done once and may take a while. Your own files aren't changed.";
                if (gamelib_tables()) ex_d = "Restart Multi before changing the game files.";
                ex_d = ui_tr(ex_d.c_str());
                if (!job.empty()) ex_d += "\n\n" + std::string(ui_tr(job.c_str())) + (job_hint.empty() ? "" : ".\n" + job_hint);

                // the controller's cursor starts on the first row (not the X), a few frames while the window lays out
                static int seen_frame = -10, focus_first;  // (not drawn last frame: the window has just opened)
                if (seen_frame != ImGui::GetFrameCount() - 1) focus_first = 4;
                seen_frame = ImGui::GetFrameCount();
                auto first_row = [&] {
                    if (focus_first > 0 && focus_first--) ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                };
                profile_picker(sc);
                opt_begin("##files", sc);
                {
                    if (g_profile>0 && !strcmp(game_profile_title(g_profile),"acnh") &&
                        !strcmp(game_profile_version(g_profile),"3.0.1")) {
                        opt_section("Frame rate");
                        const char *modes[]={"Normal 30 FPS","60 FPS + SpeedFix (experimental)"};
                        int mode=g_settings.acnh_speedfix60?1:0;
                        if (busy || gamelib_tables() || !check_copied_files(game_profile_base_dir(g_profile)).empty()) opt_disable_next();
                        const char *hint=gamelib_tables()?"Restart Multi before changing frame-rate mode.":
                            "60 FPS + SpeedFix keeps normal game speed when your PC sustains 60 FPS. Below 60 FPS the game slows down. First use prepares and compiles a separate copy; both modes use the same 3.0.1 save. ACNH 60 FPS + SpeedFix by tmouh (CC BY-NC 4.0).";
                        if (opt_choice("Frame-rate mode",&mode,modes,2,0,hint)) {
                            if (!mode) {g_settings.acnh_speedfix60=false;g_profile_request=g_profile;}
                            else {
                                pthread_mutex_lock(&g_lock);g_busy=busy=true;g_job_failed=false;pthread_mutex_unlock(&g_lock);
                                pthread_t thread;
                                if (!pthread_create(&thread,NULL,speedfix_thread,NULL)) pthread_detach(thread);
                                else {pthread_mutex_lock(&g_lock);g_busy=busy=false;pthread_mutex_unlock(&g_lock);
                                      set_job("Could not start the build worker.",true);}
                            }
                        }
                    }
                    opt_section("Your copy of the game");
                    if (busy) opt_disable_next();
                    if (opt_action("Game files folder", ui_tr(src_v.c_str()), src_d.c_str(), true))
                        SDL_ShowOpenFolderDialog(picked, NULL, win, NULL, false);
                    check_left_of_value(src_v);
                    first_row();
                    static const SDL_DialogFileFilter nsz_filter[] = {{"Game archive (NSP / NSZ / XCI / XCZ)", "nsp;nsz;xci;xcz"}};
                    std::string base_hint = std::string(ui_tr("Choose your base game NSP, NSZ, XCI or XCZ. The original archive is kept.")) +
                                            (source_is_archive ? "\n\n" + src : "");
                    if (busy) opt_disable_next();
                    if (opt_action("Base game archive", ui_tr(source_is_archive ? "Selected" : "Choose..."),
                                   base_hint.c_str(), true))
                        SDL_ShowOpenFileDialog(picked_archive, NULL, win, nsz_filter, 1, NULL, false);
                    if (source_is_archive) {
                        if (busy) opt_disable_next();
                        std::string update_hint = std::string(ui_tr("Optional: choose a supported update for the same game.")) + "\n\n" + update_archive;
                        if (opt_action("Update archive (optional)", ui_tr(update_archive.empty() ? "None" : "Selected"),
                                       update_hint.c_str(), true))
                            SDL_ShowOpenFileDialog(picked_archive, (void *)1, win, nsz_filter, 1, NULL, false);
                        if (!update_archive.empty()) {
                            if (busy) opt_disable_next();
                            if (opt_action("Use base game only", "", "Clear the update selection; keep both source archives.", true)) {
                                pthread_mutex_lock(&g_lock);
                                g_update_archive.clear();
                                g_inspected = false;
                                pthread_mutex_unlock(&g_lock);
                            }
                        }
                    }
                    if (source_is_archive) {
                        static const SDL_DialogFileFilter key_filter[] = {{"Local key file", "keys;txt"}};
                        if (busy) opt_disable_next();
                        std::string kd = std::string(ui_tr("Select your own prod.keys for encrypted game content. Used locally for this session only.")) + "\n\n" + keys;
                        if (opt_action("Decryption keys", ui_tr(keys.empty() ? "Choose..." : "Selected"), kd.c_str(), true))
                            SDL_ShowOpenFileDialog(picked_archive, (void *)2, win, key_filter, 1, NULL, false);
                        if (!keys.empty()) {
                            if (busy) opt_disable_next();
                            std::string td = std::string(ui_tr("Optional title.keys when the archive has no usable common ticket.")) + "\n\n" + title_keys;
                            if (opt_action("Title keys (optional)", ui_tr(title_keys.empty() ? "None" : "Selected"), td.c_str(), true))
                                SDL_ShowOpenFileDialog(picked_archive, (void *)3, win, key_filter, 1, NULL, false);
                            if (busy) opt_disable_next();
                            if (opt_action("Clear key selections", "", "Forget selected key paths for this session.", true)) {
                                pthread_mutex_lock(&g_lock);
                                g_keys.clear(); g_title_keys.clear(); g_inspected = false;
                                pthread_mutex_unlock(&g_lock);
                            }
                        }
                        if (busy) opt_disable_next();
                        if (opt_action("Check selection", "", "Read archive type, game identity and content version before importing.", true)) {
                            pthread_mutex_lock(&g_lock); g_busy = true; pthread_mutex_unlock(&g_lock);
                            g_build_frac = -1; g_total = 0;
                            pthread_t t;
                            if (!pthread_create(&t, NULL, inspect_thread, NULL)) pthread_detach(t);
                            else { pthread_mutex_lock(&g_lock); g_busy = false; pthread_mutex_unlock(&g_lock); }
                        }
                        if (inspected) {
                            for (int i = 0; i < 2; ++i) {
                                if (!strcmp(info[i].format, "-")) continue;
                                ImGui::Text("%s: %s", ui_tr(i ? "Update archive (optional)" : "Base game archive"), info[i].format);
                                int identified = game_profile_match(info[i].game_id, info[i].version);
                                if (identified >= 0) ImGui::TextWrapped("%s", ui_tr(game_profile_name(identified)));
                                if (!strcmp(info[i].version, "?")) ImGui::TextWrapped("%s", ui_tr("Version unknown; checked during import."));
                                else ImGui::TextWrapped(ui_tr("Version %s (%s)"), info[i].version,
                                        ui_tr(!strcmp(info[i].metadata, "CNMT") ? "content metadata" :
                                              !strcmp(info[i].metadata, "ExeFS") ? "verified executable modules" : "XML, unverified"));
                                if (!info[i].installable) ImGui::TextWrapped("%s", ui_tr("This game adapter is still in development."));
                            }
                        }
                    }
                    opt_section("Set up");
                    const GameArchiveInfo &selected_release = update_archive.empty() ? info[0] : info[1];
                    bool development_blocked = source_is_archive && inspected && !selected_release.installable && !getenv("LARECOMP_ALLOW_DEVELOPMENT_GAMES");
                    bool pending_build = g_profile >= 0 && game_install_pending(game_profile_install_dir(g_profile));
                    if (busy || gamelib_tables() || (source_is_archive && (!inspected || development_blocked)) ||
                        (src.empty() && !pending_build && !check_copied_files(g_root).empty())) opt_disable_next();
                    bool resume_build = src.empty() && (pending_build || check_copied_files(g_root).empty());
                    if (opt_action(resume_build && resumable_build() ? "Resume game code" : resume_build ? "Build or resume game code" : "Set up game files", ui_tr(busy ? "Working..." : ""), ex_d.c_str(), true)) {
                        files_gb = -1;  // (re-measured after a setup)
                        pthread_mutex_lock(&g_lock);
                        g_busy = true;
                        pthread_mutex_unlock(&g_lock);
                        pthread_t t;
                        if (!pthread_create(&t, NULL, extract_thread, NULL)) pthread_detach(t);
                        else { pthread_mutex_lock(&g_lock); g_busy = false; pthread_mutex_unlock(&g_lock); set_job("Could not start the build worker.", true); }
                    }
                    if (file_exists(build_cache_root() + "/.gamebuild")) {
                        if (busy || game_profile_index(g_root.c_str(), rt_data_root()) < 0) opt_disable_next();
                        if (opt_action("Clear compilation cache", "", "Keeps installed game code and saves. The next rebuild starts from scratch.", true)) {
                            char why[512] = "";
                            bool ok = build_cache_clear(build_cache_root().c_str(), why, sizeof why);
                            set_job(ok ? "Compilation cache cleared. Installed game files and saves are kept." : "Could not clear compilation cache.", !ok, why);
                        }
                    }
                }
                char speedfix_parent[2048];
                bool speedfix_selected=acnh60_base_path(g_root.c_str(),speedfix_parent,sizeof speedfix_parent);
                if (ready && !speedfix_selected) {
                    // DELETE: frees Multi's copy (runtime/paths.c removes only what the setup made, only inside
                    // the data root). Saves, seeds, settings and the user's own files stay.
                    if (files_gb < 0) files_gb = rt_game_files_size(g_root.c_str()) / 1e9;
                    char gb[32];
                    snprintf(gb, sizeof gb, "%.1f GB", files_gb);
                    opt_section("Game files");
                    if (!g_confirm_delete) {
                        if (opt_action("Delete game files", gb, "Frees the space Multi's copy of the game files takes. "
                                       "Saves, seeds and settings are kept, and your own game files aren't touched: you can "
                                       "set the game files up again from them.", true))
                            g_confirm_delete = true;
                        first_row();
                    } else {
                        if (opt_action("Yes, delete them", gb, "Deletes the extracted game files now.", true)) {
                            stop_background_prep();
                            bool ok = rt_remove_game_files(g_root.c_str());
                            game_files_changed();
                            status = check_game_files(g_root);
                            if (ok) set_job("Game files deleted.");
                            else set_job("Couldn't delete the game files", true, "They aren't in Multi's data folder, "
                                         "so they were left alone.");
                            if (!status.empty()) g_bg = 0;
                            g_confirm_delete = false, files_gb = -1;
                        }
                        if (opt_action("Cancel", "", "Keeps the game files.", true)) g_confirm_delete = false;
                    }
                }
                if (!busy && !gamelib_tables() && has_build_cache()) {
                    opt_section("Build cache");
                    if (opt_action("Clear build cache", "", "Removes the saved game-code build files. The next setup compiles "
                                   "everything again.", true)) {
                        char why[512] = "";
                        g_build_cache_changed = true;
                        if (build_cache_clear(build_cache_root().c_str(), why, sizeof why)) set_job("Build cache cleared.");
                        else set_job("Could not clear the build cache.", true, why);
                    }
                }
                if (!job_line.empty()) {  // the current step / result
                    ImGui::Dummy(ImVec2(0, 14 * sc));
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextColored(failed ? ImVec4(1, 0.5f, 0.4f, 1) : busy ? ImVec4(0.93f, 0.76f, 0.33f, 1)
                                              : ImVec4(0.45f, 0.95f, 0.55f, 1), "%s", ui_tr(job_line.c_str()));
                    if (failed && !job_hint.empty())  // (what to do next, in the rows' own text colour)
                        ImGui::TextColored(ImVec4(0.86f, 0.85f, 0.80f, 1), "%s", ui_tr(job_hint.c_str()));
                    ImGui::PopTextWrapPos();
                } else if (!busy && !status.empty() && status != "No game files yet") {
                    // (no setup run yet this session: why the game files on disk can't be played - an incomplete copy
                    // opens this window instead of crashing on Play)
                    ImGui::Dummy(ImVec2(0, 14 * sc));
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", status.c_str());
                    ImGui::TextColored(ImVec4(0.86f, 0.85f, 0.80f, 1), "%s", status_hint(status));
                    ImGui::PopTextWrapPos();
                }
                opt_end();
                if (g_confirm_delete && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                         ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                    g_confirm_delete = false;  // (B / Esc: back out of the question first)
                else if (!busy && !typing && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                              ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                    back = true;
                ImGui::PopFont();
                ImGui::EndChild();
                if (back && !busy) {
                    settings_save();
                    g_confirm_delete = false;
                    page = MAIN, g_refocus = SETUP;
                }
                static bool test_busy = getenv("LARECOMP_LAUNCHER_SHOT") && (strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":extracting") ||
                                                                         strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":preparing") ||
                                                                         strstr(getenv("LARECOMP_LAUNCHER_SHOT"), ":skipask"));
                if (busy || test_busy) draw_busy_overlay(ds, job, test_busy, false, source_is_archive);
            }

            // status (lower left) and version (lower right) - not under the Settings / Game Files windows
            if (page == MAIN) {
            ImGui::PushFont(g_font_small);
            ImGui::SetCursorPos(ImVec2(x0, ds.y - 118 * s));
            if (status.empty()) {
                {  // a drawn check mark (the Switch font has no ✓)
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    float u = ImGui::GetTextLineHeight();
                    ImU32 g = IM_COL32(140, 230, 140, 255);
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + u * 0.1f, p.y + u * 0.55f), ImVec2(p.x + u * 0.35f, p.y + u * 0.8f), g, u * 0.12f);
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + u * 0.35f, p.y + u * 0.8f), ImVec2(p.x + u * 0.85f, p.y + u * 0.2f), g, u * 0.12f);
                    ImGui::SetCursorPosX(x0 + u * 1.3f);
                }
                ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1), "%s", ui_tr("Game files ready."));
                ImGui::SetCursorPosX(x0);
                ImGui::TextColored(ImVec4(0.66f, 0.76f, 0.98f, 1), ui_tr("Nintendo Switch  \xc2\xb7  v%s"),
                                   title_profile_current()->version);
            } else {
                ImGui::TextColored(ImVec4(1, 0.72f, 0.4f, 1), ui_tr("!  %s"), busy ? "Setting up the game files..." : status.c_str());
                ImGui::SetCursorPosX(x0);
                ImGui::TextDisabled("%s", busy ? "Set up the game files to play." : status_hint(status));
            }
            if (const char *f = getenv("LARECOMP_LAUNCHER_FONT")) {  // (test) which typeface this window shows
                const char *b = strrchr(f, '/');
                ImGui::GetWindowDrawList()->AddText(ImVec2(ImGui::GetWindowPos().x + 20 * s, ImGui::GetWindowPos().y + 16 * s),
                                                    IM_COL32(255, 255, 255, 255), b ? b + 1 : f);
            }
            {  // version, and a newer release when the update check found one (lower right). The background is
               // always the title movie's still: bright clouds here, under a shadow banner from the right
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImFont *font = ImGui::GetFont();
                float fs = ImGui::GetFontSize(), right = ImGui::GetWindowPos().x + ds.x - 60 * s;
                auto text = [&](ImVec2 p, float size, ImU32 col, const char *t, bool bold) {
                    for (float d : {2.5f, 1.5f, 0.8f}) dl->AddText(font, size, ImVec2(p.x + d * s, p.y + d * s), IM_COL32(4, 8, 30, 150), t);
                    dl->AddText(font, size, p, col, t);
                    if (bold) dl->AddText(font, size, ImVec2(p.x + 0.7f * s, p.y), col, t);
                };
                std::string ver = tag_label(LARECOMP_VERSION, true);
                float vs = fs * 1.4f, y = ImGui::GetWindowPos().y + ds.y - 134 * s;
                ImVec2 vsz = font->CalcTextSizeA(vs, FLT_MAX, 0, ver.c_str());
                text(ImVec2(right - vsz.x, y), vs, IM_COL32(255, 255, 255, 255), ver.c_str(), false);
                pthread_mutex_lock(&g_update_lock);
                std::string upd = g_update_tag;
                pthread_mutex_unlock(&g_update_lock);
                if (!upd.empty()) {
                    const char *arrow = font->FindGlyphNoFallback(0x2192) ? " \xe2\x86\x92" : " \xe2\x96\xb8";
                    std::string pre = "Update available!  ", link = "DOWNLOAD " + tag_label(upd, false) + arrow;
                    float wp = font->CalcTextSizeA(fs, FLT_MAX, 0, pre.c_str()).x;
                    ImVec2 lsz = font->CalcTextSizeA(fs, FLT_MAX, 0, link.c_str());
                    ImVec2 lp(right - lsz.x, y + vsz.y + 2 * s);
                    text(ImVec2(lp.x - wp, lp.y), fs, IM_COL32(200, 214, 250, 255), pre.c_str(), false);
                    ImGui::SetCursorScreenPos(lp);
                    bool click = ImGui::InvisibleButton("update", lsz);
                    bool hover = ImGui::IsItemHovered();
                    // the menu's hover colours (yellow to orange), lighter while the link itself is hovered
                    ImU32 col = hover ? IM_COL32(255, 238, 150, 255) : IM_COL32(255, 214, 64, 255);
                    ImU32 col_b = hover ? IM_COL32(255, 196, 110, 255) : IM_COL32(255, 160, 50, 255);
                    text_fx(dl, font, fs, lp, col, col_b, link.c_str(), 1.5f * s);
                    text_fx(dl, font, fs, ImVec2(lp.x + 0.7f * s, lp.y), col, col_b, link.c_str(), 0);  // (faux bold)
                    if (hover) {
                        dl->AddLine(ImVec2(lp.x, lp.y + lsz.y), ImVec2(lp.x + lsz.x, lp.y + lsz.y), col, 1.5f * s);
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }
                    if (click) SDL_OpenURL((RELEASE_PAGE + upd).c_str());
                }
            }
            ImGui::PopFont();
            }
            ImGui::EndDisabled();
            ImGui::End();
            ImGui::PopStyleColor();
            if (waiting) draw_busy_overlay(ds, "", false, true);
            osk_draw(g_font_menu, s * 0.7f);  // (a text field activated with the controller)
            typed_before = ui_text_busy();

            ImGui::Render();
#ifdef __APPLE__
            ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
            [enc endEncoding];
            id<MTLTexture> shot = nil;
            if (shot_at && ++frame == shot_at) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:drawable.texture.pixelFormat
                                             width:drawable.texture.width height:drawable.texture.height mipmapped:NO];
                td.storageMode = MTLStorageModeShared;
                shot = [dev newTextureWithDescriptor:td];
                id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
                [bl copyFromTexture:drawable.texture toTexture:shot];
                [bl endEncoding];
            }
            [cb presentDrawable:drawable];
            [cb commit];
            if (shot) {
                [cb waitUntilCompleted];
                save_png(shot, getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/launcher.png");
                plat_exit(0);
            }
#else
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)vk_frame_cmd());
            bool shot = shot_at && ++frame == shot_at;
            vk_frame_end(shot ? getenv("LARECOMP_SHOT_PATH") ? getenv("LARECOMP_SHOT_PATH") : "build/launcher.png" : NULL);
            if (shot) plat_exit(0);
#endif
        }
    }
    // play: remember the choice, hand the window to the game
    snprintf(g_settings.rando_seed, sizeof g_settings.rando_seed, "%s", choice ? choices[choice].c_str() : "");
    workshop_selection(g_root.c_str(),g_settings.rando_seed);
    settings_save();
#ifdef __APPLE__
    ImGui_ImplMetal_Shutdown();
#else
    vk_imgui_backend_shutdown();
#endif
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev_ctx);
    g_bg = 0;
    g_start_screen = 0;
    return g_root.c_str();
}
