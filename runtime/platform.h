// Host OS layer: everything the runtime needs that differs between macOS and Windows.
// Implementations: runtime/platform/macos.c, runtime/platform/windows.c (the Makefile picks one).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include "platform/win_compat.h"
#elif defined(__linux__)
#include "platform/linux_compat.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- guest address space
// Claim [lo, hi) for the guest at exactly that address, readable/writable (pages may be backed lazily).
bool plat_reserve_guest(uint64_t lo, uint64_t hi);
// Make sure [addr, addr+size) inside the guest window is backed (before the OS itself writes there, e.g.
// file reads); a no-op where the whole window is backed from the start.
void plat_commit(uint64_t addr, uint64_t size);
// In-place reboot: every page of [lo, hi) back to zero (and unbacked), the window still claimed.
bool plat_reset_guest(uint64_t lo, uint64_t hi);
// Pages of [addr, addr+size) in the guest window the CPU wrote since the last call over them (write tracking: Windows
// MEM_WRITE_WATCH, Linux 6.7+ userfaultfd + PAGEMAP_SCAN), their addresses into pages[] (at most max), and the tracking
// reset for the range. Returns the count (max: maybe more), or -1 where there is no tracking (macOS, older Linux).
// Page size: *page.
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page);
#ifdef __linux__
void plat_write_tracking_off(const char *why);  // tracking off from now on (unless LARECOMP_WRITE_TRACKING=1)
// Tracking of [lo, hi) by mprotect + SIGSEGV instead (memory the kernel's can't watch: the heap as driver memory);
// LARECOMP_SOFT_WRITE_TRACKING=0: off
void plat_soft_write_tracking(uint64_t lo, uint64_t hi);
// The OS is about to write [addr, addr+size) (a file read into guest memory): pages soft tracking protected are opened
// and count as written first (a write by the OS on a protected page would fail instead of faulting)
void plat_os_write(uint64_t addr, uint64_t size);
#else
static inline void plat_os_write(uint64_t addr, uint64_t size) { (void)addr, (void)size; }
#endif
enum { PLAT_PROT_NONE = 0, PLAT_PROT_READ = 1, PLAT_PROT_RW = 3 };
int plat_protect(uint64_t addr, uint64_t size, int prot);  // 0 on success
#define PLAT_PAGE 0x4000ull  // largest page size we deal with (macOS 16K; multiple of Windows' 4K)

// ---- time
uint64_t plat_now_ns(void);                // monotonic
void plat_sleep_until_ns(uint64_t t);      // precise wait for plat_now_ns() >= t
void plat_thread_realtime(uint64_t period_ns);  // periodic timing thread (vsync): don't coalesce its timers
int plat_thread_realtime_check(uint64_t period_ns);  // (the calling thread, after a late wait) 0 the policy holds, 1 it was
                                                     // gone and is set again, -1 can't tell; only macOS looks
int plat_cpu_count(void);  // logical cores this process may run on (its affinity mask); 0 if unknown
void plat_thread_guest(int prio);  // a guest thread's host scheduling class from its Switch priority (lower = more urgent)
uint64_t plat_thread_cpu_ns(void);  // the calling thread's CPU time so far (user + system; diagnostics)
// The calling thread is one of the frame's critical threads: on Windows, no power throttling (EcoQoS) for it and, on a
// hybrid CPU, a preference for the performance cores (selected CPU sets; LARECOMP_PCORE=0: off). Elsewhere a no-op.
enum { PLAT_PERF_GAME, PLAT_PERF_RENDER, PLAT_PERF_VSYNC };
#ifdef _WIN32
void plat_thread_perf_core(int role);
#else
static inline void plat_thread_perf_core(int role) { (void)role; }
#endif

// ---- process
void plat_init(void);                                // first thing in main()
// The machine, for the log's first "[info]" line (runtime/main.c): nothing that identifies the player (no user name,
// no paths). Fields left empty / 0 when unknown.
typedef struct {
    char os[128];          // "macOS 15.4 (arm64)", "SteamOS 3.6.19 (x86_64, kernel 6.5.0-valve22)"
    char cpu[128];         // "Apple M3 Max"
    char cores_detail[48]; // optional, after the core count: "12P + 4E"
    int cores, threads;    // physical cores, hardware threads
    uint64_t ram;          // bytes
    const char *package;   // ".app", "AppImage", "zip"; NULL: a build run from its folder
} PlatSysInfo;
void plat_system_info(PlatSysInfo *si);
bool plat_exe_path(char *buf, size_t size);
void plat_exec(char *const argv[]);  // replace the process (NULL = same command line); returns only on failure
#if defined(_WIN32) || defined(__linux__)
void plat_terminate(void);  // end the process now, skipping DLL teardown (exit status 0)
#endif

// Child processes (seed generator, game-file extraction). stdout + stderr go to log_path (NULL = /dev/null).
typedef intptr_t PlatProc;  // 0 = failed to start
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log);
int plat_proc_wait(PlatProc p, bool block);  // exit code, or -1 while still running (block = false)
#define PLAT_PROC_SIGNALED 256               // + N: ended by signal N instead of exiting (not on Windows)
#define PLAT_PROC_FAST_FAIL 512              // Windows: ended by a security check (0xC0000409, e.g. Control Flow Guard)
#ifdef _WIN32
// plat_spawn with Control Flow Guard off for the child (the generator: unicorn's longjmp fails CFG's check, #337).
// *error: 0 = started that way; else why not (then it was started normally, or not at all if that failed too)
PlatProc plat_spawn_no_cfg(const char *const argv[], const char *log_path, bool append_log, unsigned long *error);
#endif
void plat_proc_kill(PlatProc p);             // end it and what it started (then it's gone: no wait)
bool plat_remove_tree(const char *dir);      // rm -rf
// dir is synced by OneDrive (Windows: under a OneDrive folder, or a cloud file); false on other systems
bool plat_onedrive_synced(const char *dir);
#ifdef _WIN32
#define PLAT_EXE_SUFFIX ".exe"
#define PLAT_VENV_PYTHON "venv/Scripts/python.exe"  // relative to the venv's parent
#else
#define PLAT_EXE_SUFFIX ""
#define PLAT_VENV_PYTHON "venv/bin/python"
#endif

// ---- diagnostics
// Name of the host function containing pc (guest functions are f_<addr>), NULL if unknown.
const char *plat_symbol(uint64_t pc, uint64_t *offset);
bool plat_module_name(uint64_t pc, char *out, size_t n);  // the file of the module (exe / library / driver) holding pc
void plat_print_backtrace(void);
void plat_fatal_notice(void);  // a fatal error is about to end the process: tell the user (Windows release: message box)
// Something the player should know that doesn't stop the game (a save that failed): the same dialog as
// plat_fatal_notice, as a warning, from its own thread so the game keeps running. Like it, only shown when started as
// the app (a terminal / test run has the log).
void plat_notice(const char *text);
void plat_crash_handler_install(void);  // per thread where the OS needs it (alternate signal stack)
// Called by the crash handler (runtime/dispatch.c): true = fault handled (watchpoint), resume.
bool rt_fault_watch(uint64_t fault_addr, uint64_t host_pc);
void rt_fault_report(const char *what, uint64_t fault_addr, uint64_t host_pc, uint64_t host_lr) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
