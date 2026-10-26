// Linux host layer (runtime/platform.h). x86-64 and aarch64.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/ucontext.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "../paths.h"
#include "../platform.h"
#include "posix_log.h"

void rt_crash_explain(char *out, size_t n, const char *log_path);  // runtime/crash_explain.c

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// ---- page write tracking (plat_written_pages), the counterpart of Windows' MEM_WRITE_WATCH
// The window is registered with a userfaultfd in asynchronous write-protect mode: the kernel itself lifts a page's
// protection on its first write (no fault handler thread, no signal) and remembers it was written. PAGEMAP_SCAN then
// lists the written pages of a range and protects them again in one call, like GetWriteWatch(WRITE_WATCH_FLAG_RESET).
// Pages are only protected once a query has covered them, so memory nobody asks about never faults. Linux 6.7+;
// without it (older kernel, userfaultfd unavailable): no tracking, as before. LARECOMP_WRITE_TRACKING=0: off.
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
#ifndef UFFD_FEATURE_WP_UNPOPULATED
#define UFFD_FEATURE_WP_UNPOPULATED (1 << 13)
#endif
#ifndef UFFD_FEATURE_WP_ASYNC
#define UFFD_FEATURE_WP_ASYNC (1 << 15)
#endif
// linux/fs.h's PAGEMAP_SCAN ABI, spelled out (the AppImage builds against Ubuntu 22.04's headers, which predate it)
typedef struct { uint64_t start, end, categories; } WtRegion;
typedef struct {
    uint64_t size, flags, start, end, walk_end, vec, vec_len, max_pages;
    uint64_t category_inverted, category_mask, category_anyof_mask, return_mask;
} WtScan;
#define WT_PAGEMAP_SCAN _IOWR('f', 16, WtScan)
#define WT_PAGE_IS_WRITTEN (1 << 1)
#define WT_SCAN_WP_MATCHING (1 << 0)    // protect the pages reported again
#define WT_SCAN_CHECK_WPASYNC (1 << 1)  // fail (instead of reporting nothing) where the range isn't registered

static int g_uffd = -1, g_pagemap = -1;
static uint64_t g_wt_lo, g_wt_hi, g_wt_page;

static void wt_off(const char *why) {
    fprintf(stderr, "[plat] no page write tracking: %s (%s)\n", why, strerror(errno));
    if (g_pagemap >= 0) close(g_pagemap);
    if (g_uffd >= 0) close(g_uffd);  // (unregisters the window)
    g_pagemap = g_uffd = -1;
}

static bool wt_register(uint64_t lo, uint64_t hi) {
    struct uffdio_register r = {.range = {.start = lo, .len = hi - lo}, .mode = UFFDIO_REGISTER_MODE_WP};
    return ioctl(g_uffd, UFFDIO_REGISTER, &r) == 0;
}

static void wt_init(uint64_t lo, uint64_t hi) {
    const char *e = getenv("LARECOMP_WRITE_TRACKING");
    if (e && !atoi(e)) return;
    // user-mode faults only: allowed without privileges (vm.unprivileged_userfaultfd=0); the asynchronous protection
    // is lifted by the kernel either way, also for its own writes (read() into guest memory)
    g_uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (g_uffd < 0) { wt_off("userfaultfd"); return; }
    struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED};
    if (ioctl(g_uffd, UFFDIO_API, &api)) { wt_off("asynchronous write protection needs Linux 6.7"); return; }
    if (!wt_register(lo, hi)) { wt_off("register"); return; }
    g_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (g_pagemap < 0) { wt_off("/proc/self/pagemap"); return; }
    WtRegion v;
    uint64_t pg = (uint64_t)sysconf(_SC_PAGESIZE);
    WtScan s = {.size = sizeof s, .flags = WT_SCAN_CHECK_WPASYNC, .start = lo, .end = lo + pg, .vec = (uintptr_t)&v,
                .vec_len = 1, .category_mask = WT_PAGE_IS_WRITTEN, .return_mask = WT_PAGE_IS_WRITTEN};
    if (ioctl(g_pagemap, WT_PAGEMAP_SCAN, &s) < 0) { wt_off("PAGEMAP_SCAN needs Linux 6.7"); return; }
    g_wt_lo = lo;
    g_wt_hi = hi;
    g_wt_page = pg;
}

// Tracking off after all (the Vulkan backend at boot, before any query protected a page: nothing is protected yet), with
// the reason logged; LARECOMP_WRITE_TRACKING=1 keeps it on. Closing the userfaultfd unregisters the window.
void plat_write_tracking_off(const char *why) {
    if (g_uffd < 0 && g_pagemap < 0) return;
    const char *e = getenv("LARECOMP_WRITE_TRACKING");
    if (e && atoi(e)) { fprintf(stderr, "[plat] page write tracking kept on (LARECOMP_WRITE_TRACKING=1): %s\n", why); return; }
    fprintf(stderr, "[plat] page write tracking off: %s\n", why);
    if (g_pagemap >= 0) close(g_pagemap);
    if (g_uffd >= 0) close(g_uffd);
    g_pagemap = g_uffd = -1;
}

// ---- soft page write tracking: mprotect + SIGSEGV, where the kernel's can't run (LARECOMP_SOFT_WRITE_TRACKING=0: off)
// The guest heap as driver memory (exec.c guest_heap_dmabuf: RADV) is no anonymous memory, so userfaultfd can't watch
// it; there a query instead protects the pages it covered read-only, and the CPU's first write to one faults:
// crash_handler (soft_fault) records the page and gives it its write access back. The GPU's own mapping of the memory
// is not the CPU's page table, so the GPU never notices. Only pages a query covered ever fault, and each at most once
// between two queries. The OS's own writes (file reads into the heap) would fail on a protected page instead of
// faulting: plat_os_write unprotects first.
// Page states: 0 never protected (not ours), 1 protected, 2 being given back, 3 writable again (was ours).
enum { SOFT_NONE, SOFT_ARMED, SOFT_RELEASING, SOFT_OPEN };
static _Atomic uint8_t *g_soft_state, *g_soft_written;  // one byte per 4K page of [g_soft_lo, g_soft_hi)
static uint64_t g_soft_lo, g_soft_hi;
static _Atomic uint64_t g_soft_faults, g_soft_queries, g_soft_protects;
#define SOFT_PAGE 4096ull

void plat_soft_write_tracking(uint64_t lo, uint64_t hi) {
    const char *e = getenv("LARECOMP_SOFT_WRITE_TRACKING");
    if ((e && !atoi(e)) || g_soft_state) return;
    uint64_t n = (hi - lo) / SOFT_PAGE;
    g_soft_state = calloc(n, 1), g_soft_written = calloc(n, 1);  // (untouched parts of these cost nothing either)
    if (!g_soft_state || !g_soft_written) { free((void *)g_soft_state), free((void *)g_soft_written); g_soft_state = NULL; return; }
    g_soft_lo = lo, g_soft_hi = hi;
    fprintf(stderr, "[plat] soft page write tracking on (mprotect + SIGSEGV) over %llx-%llx\n", (unsigned long long)lo,
            (unsigned long long)hi);
}

// Give page p its write access back and count it written (the fault handler and plat_os_write). False: not ours.
static bool soft_release(uint64_t p) {
    uint8_t st = SOFT_ARMED;
    if (atomic_compare_exchange_strong(&g_soft_state[p], &st, SOFT_RELEASING)) {
        atomic_store(&g_soft_written[p], 1);  // (before the page opens: a query that sees it open sees it written)
        mprotect((void *)(uintptr_t)(g_soft_lo + p * SOFT_PAGE), SOFT_PAGE, PROT_READ | PROT_WRITE);
        atomic_store(&g_soft_state[p], SOFT_OPEN);
        atomic_fetch_add(&g_soft_faults, 1);
        return true;
    }
    while (st == SOFT_RELEASING) st = atomic_load(&g_soft_state[p]);  // (another thread opens it: the write retries)
    return st == SOFT_OPEN;  // (opened by another thread meanwhile: retry; SOFT_NONE: never ours, a real fault)
}

static bool soft_fault(uint64_t fa) {
    if (!g_soft_state || fa < g_soft_lo || fa >= g_soft_hi) return false;
    return soft_release((fa - g_soft_lo) / SOFT_PAGE);
}

void plat_os_write(uint64_t addr, uint64_t size) {
    if (!g_soft_state || !size || addr >= g_soft_hi || addr + size <= g_soft_lo) return;
    uint64_t a = addr < g_soft_lo ? g_soft_lo : addr, b = addr + size > g_soft_hi ? g_soft_hi : addr + size;
    for (uint64_t p = (a - g_soft_lo) / SOFT_PAGE; p <= (b - 1 - g_soft_lo) / SOFT_PAGE; p++)
        if (atomic_load(&g_soft_state[p]) == SOFT_ARMED) soft_release(p);
}

// plat_written_pages over soft tracking: protect the range's open pages first, then take the written marks, so a write
// between the two is either already marked or faults after the protection.
static int soft_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max) {
    if (addr < g_soft_lo || addr + size > g_soft_hi || max <= 0) return -1;
    uint64_t first = (addr - g_soft_lo) / SOFT_PAGE, last = (addr + size - 1 - g_soft_lo) / SOFT_PAGE;
    for (uint64_t p = first; p <= last;) {  // runs of pages not protected: one mprotect each
        uint8_t st = atomic_load(&g_soft_state[p]);
        if (st == SOFT_ARMED || st == SOFT_RELEASING) { p++; continue; }
        uint64_t q = p;
        while (q <= last && (st = atomic_load(&g_soft_state[q])) != SOFT_ARMED && st != SOFT_RELEASING) q++;
        for (uint64_t i = p; i < q; i++) atomic_store(&g_soft_state[i], SOFT_ARMED);
        if (mprotect((void *)(uintptr_t)(g_soft_lo + p * SOFT_PAGE), (q - p) * SOFT_PAGE, PROT_READ)) {
            static int warned;
            if (warned++ < 5)
                fprintf(stderr, "[plat] soft tracking: mprotect %llx +%llu pages failed: %s\n",
                        (unsigned long long)(g_soft_lo + p * SOFT_PAGE), (unsigned long long)(q - p), strerror(errno));
        }
        atomic_fetch_add(&g_soft_protects, 1);
        p = q;
    }
    int n = 0;
    for (uint64_t p = first; p <= last; p++)
        if (atomic_load(&g_soft_written[p]) && atomic_exchange(&g_soft_written[p], 0)) {
            if (n < max) pages[n] = g_soft_lo + p * SOFT_PAGE;
            n++;
        }
    uint64_t q = atomic_fetch_add(&g_soft_queries, 1);
    static uint64_t last_log;
    uint64_t now = plat_now_ns();
    if (now - last_log > 5000000000ull) {
        fprintf(stderr, "[plat] soft tracking, last 5 s: %llu queries, %llu protects, %llu faults\n",
                    (unsigned long long)q, (unsigned long long)atomic_exchange(&g_soft_protects, 0),
                    (unsigned long long)atomic_exchange(&g_soft_faults, 0));
        atomic_store(&g_soft_queries, 0);
        last_log = now;
    }
    return n < max ? n : max;
}

// ---- guest address space
// Everything readable/writable from the start, like macOS: the guest maps at 4K granularity, and untouched pages
// cost nothing (MAP_NORESERVE: the 12 GB window doesn't count against overcommit).
bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)lo) { munmap(p, hi - lo); return false; }  // (kernels before 4.17: just a hint)
    wt_init(lo, hi);
    return true;
}

// Fresh zero pages over the whole window (the old ones are released at once). The new mapping is registered again.
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    if (mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
             -1, 0) != (void *)(uintptr_t)lo)
        return false;
    if (g_uffd >= 0 && !wt_register(lo, hi)) wt_off("register after reset");
    return true;
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (g_soft_state) { *page = SOFT_PAGE; return soft_written_pages(addr, size, pages, max); }
    if (g_pagemap < 0 || addr < g_wt_lo || addr + size > g_wt_hi || max <= 0) return -1;
    WtRegion v[64];
    uint64_t at = addr, end = addr + size;
    int n = 0;
    while (at < end && n < max) {
        WtScan s = {.size = sizeof s, .flags = WT_SCAN_WP_MATCHING | WT_SCAN_CHECK_WPASYNC, .start = at, .end = end,
                    .vec = (uintptr_t)v, .vec_len = 64, .max_pages = (uint64_t)(max - n),
                    .category_mask = WT_PAGE_IS_WRITTEN, .return_mask = WT_PAGE_IS_WRITTEN};
        int r = ioctl(g_pagemap, WT_PAGEMAP_SCAN, &s);  // (stops early when vec or max_pages fills: walk_end)
        if (r < 0 || s.walk_end <= at) return -1;
        for (int i = 0; i < r; i++)
            for (uint64_t p = v[i].start; p < v[i].end && n < max; p += g_wt_page) pages[n++] = p;
        at = s.walk_end;
    }
    if (at < end) {  // (more than fit: protect the rest too, the caller takes all)
        struct uffdio_writeprotect wp = {.range = {.start = addr, .len = size}, .mode = UFFDIO_WRITEPROTECT_MODE_WP};
        ioctl(g_uffd, UFFDIO_WRITEPROTECT, &wp);
    }
    *page = g_wt_page;
    return n;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

uint64_t plat_thread_cpu_ns(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0 ? (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec : 0;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// Real-time scheduling where the user may have it (rtkit / limits.conf), else just a higher priority: keeps the
// vsync thread on time while the guest threads load every core.
int plat_cpu_count(void) {
    cpu_set_t set;
    return sched_getaffinity(0, sizeof set, &set) == 0 ? CPU_COUNT(&set) : 0;
}

void plat_thread_guest(int prio) { (void)prio; }  // (the OS's default scheduling serves the game here)

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    struct sched_param sp = {.sched_priority = 1};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -10);  // (per thread on Linux; fails quietly without CAP_SYS_NICE)
}
int plat_thread_realtime_check(uint64_t period_ns) { (void)period_ns; return -1; }

// ---- process
// Started from a desktop (the AppImage) there is no terminal: stdout / stderr go to lasrecomp.log in the data folder
// (runtime/paths.h; rt_paths_init runs later, so the same rule is applied here), with five backups (posix_log.h).
static char g_log_path[1024];

static void log_to_file_without_terminal(void) {
    if (isatty(STDERR_FILENO)) return;
    char dir[900];
    if (!rt_data_root_early(dir, sizeof dir)) return;  // dev mode: wherever stderr goes already
    for (char *c = dir + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(dir, 0755); *c = '/'; }
    mkdir(dir, 0755);
    session_log_open(dir, g_log_path, sizeof g_log_path);
}

#if defined(__x86_64__)
// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2): say so instead of crashing
// with an illegal instruction on an older CPU.
static void require_cpu(void) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2")) return;
    fprintf(stderr, "FATAL: LAS Recomp needs a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Excavator / Zen or "
                    "newer).\n");
    exit(1);
}
#else
static void require_cpu(void) {}
#endif

// "key=value" / "key\t: value" lines: the value of the first line starting with key (quotes and spaces trimmed)
static bool kv_line(const char *line, const char *key, char sep, char *out, size_t n) {
    size_t k = strlen(key);
    if (strncmp(line, key, k)) return false;
    const char *p = line + k;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != sep) return false;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    snprintf(out, n, "%s", p);
    for (size_t l = strlen(out); l && (out[l - 1] == '\n' || out[l - 1] == '"' || out[l - 1] == ' '); l--) out[l - 1] = 0;
    return true;
}

void plat_system_info(PlatSysInfo *si) {
    // the OS: /etc/os-release (the host's, also inside an AppImage; SteamOS: NAME="SteamOS", VERSION_ID), the kernel
    char pretty[96] = "", name[64] = "", ver[48] = "", line[512];
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) f = fopen("/usr/lib/os-release", "r");
    for (; f && fgets(line, sizeof line, f);) {
        if (!pretty[0]) kv_line(line, "PRETTY_NAME", '=', pretty, sizeof pretty);
        if (!name[0]) kv_line(line, "NAME", '=', name, sizeof name);
        if (!ver[0]) kv_line(line, "VERSION_ID", '=', ver, sizeof ver);
    }
    if (f) fclose(f);
    char os[160];
    if (pretty[0] && (!ver[0] || strstr(pretty, ver))) snprintf(os, sizeof os, "%s", pretty);
    else if (pretty[0] || name[0]) snprintf(os, sizeof os, "%s %s", pretty[0] ? pretty : name, ver);
    else snprintf(os, sizeof os, "Linux");
    struct utsname u;
    if (uname(&u) == 0) snprintf(si->os, sizeof si->os, "%.80s (%s, kernel %.32s)", os, u.machine, u.release);
    else snprintf(si->os, sizeof si->os, "%.80s", os);
    // the CPU: x86 "model name"; ARM boards often have none - the device tree's model then (e.g. "Raspberry Pi 5")
    // cores: distinct (physical id, core id) pairs; threads: the processors online
    si->threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    enum { MAXC = 1024 };
    static int seen[MAXC];
    int nseen = 0, phys = 0;
    f = fopen("/proc/cpuinfo", "r");
    for (char v[128]; f && fgets(line, sizeof line, f);) {
        if (!si->cpu[0] && kv_line(line, "model name", ':', si->cpu, sizeof si->cpu)) continue;
        if (kv_line(line, "physical id", ':', v, sizeof v)) phys = atoi(v);
        else if (kv_line(line, "core id", ':', v, sizeof v)) {
            int key = phys * 4096 + atoi(v), i = 0;
            while (i < nseen && seen[i] != key) i++;
            if (i == nseen && nseen < MAXC) seen[nseen++] = key;
        }
    }
    if (f) fclose(f);
    si->cores = nseen ? nseen : si->threads;
    if (!si->cpu[0] && (f = fopen("/proc/device-tree/model", "r"))) {
        size_t n = fread(si->cpu, 1, sizeof si->cpu - 1, f);
        si->cpu[n] = 0;  // (NUL-terminated in the file already)
        fclose(f);
    }
    struct sysinfo s;
    if (sysinfo(&s) == 0) si->ram = (uint64_t)s.totalram * s.mem_unit;
    si->package = getenv("APPIMAGE") ? "AppImage" : NULL;  // (set by the AppImage runtime; its value is a path: not logged)
}

void plat_init(void) {
    log_to_file_without_terminal();
    raise_open_file_limit();
    require_cpu();
}

// (inside an AppImage: the binary in its mount, which stays mounted while this process lives - re-exec included)
bool plat_exe_path(char *buf, size_t size) {
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) return false;
    buf[n] = 0;
    return true;
}

// Our own command line, from /proc/self/cmdline (NUL-separated).
static char **self_argv(void) {
    static char buf[32768];
    static char *argv[256];
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;
    int k = 0;
    for (char *p = buf; p < buf + n && k < 255; p += strlen(p) + 1) argv[k++] = p;
    argv[k] = NULL;
    return argv;
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    char **av = self_argv();
    if (av && plat_exe_path(path, sizeof path)) execv(path, av);
}

// ---- diagnostics
// Exported symbols only (the Makefile links with -rdynamic); frames in the generated code are left to the caller,
// which maps them to guest functions (runtime/dispatch.c). Static runtime functions come out as the nearest guest
// function, so backtrace lines also give the offset in the executable (addr2line -fe lasrecomp <offset>).
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

// Fatal error without a terminal: say so in a message box rather than vanishing (the details are in the log).
// (what happened in plain words: runtime/crash_explain.c)
void plat_fatal_notice(void) {
    char text[1600];
    rt_crash_explain(text, sizeof text, g_log_path);
    if (!g_log_path[0]) return;
    fflush(stderr);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "LAS Recomp", text, NULL);
}

static void *notice_thread(void *text) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "LAS Recomp", text, NULL);
    free(text);
    return NULL;
}
void plat_notice(const char *text) {
    if (!g_log_path[0]) return;
    pthread_t t;
    char *copy = strdup(text);
    if (copy && !pthread_create(&t, NULL, notice_thread, copy)) pthread_detach(t);
    else free(copy);
}

bool plat_module_name(uint64_t pc, char *out, size_t n) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_fname) return false;
    snprintf(out, n, "%s", di.dli_fname);
    return true;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s && ours) fprintf(stderr, "  #%-2d %016llx %s+%llx (lasrecomp+%llx)\n", i, (unsigned long long)pc, s,
                               (unsigned long long)off, (unsigned long long)moff);
        else if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s)\n", i, (unsigned long long)pc, s, (unsigned long long)off, mod);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
#if defined(__aarch64__)
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
#else
    // x86-64 has no link register: report the return address on top of the stack (valid at function entry)
    uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP], sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP], lr = 0;
    if (sig != SIGSEGV || (fa & ~0xFFFull) != (sp & ~0xFFFull)) lr = *(uint64_t *)(uintptr_t)sp;
#endif
    if (sig == SIGSEGV && soft_fault(fa)) return;  // (soft page write tracking: the write retries on an open page)
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. a compiler's own child)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? PLAT_PROC_SIGNALED + WTERMSIG(st) : 255;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path) == 0 || errno == ENOENT ? 0 : -1;  // (gone already: e.g. a ._ file macOS removed with its folder on exFAT)
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }
bool plat_onedrive_synced(const char *dir) { (void)dir; return false; }

// End the process at once, skipping the library teardown: as on Windows, the GPU driver's threads crashed in exit()
// while the game's threads still submitted work.
void plat_terminate(void) { _exit(0); }
