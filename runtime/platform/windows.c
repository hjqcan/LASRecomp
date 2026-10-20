// Windows host layer (runtime/platform.h).
#include <windows.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shellapi.h>
#include <timeapi.h>
#include <io.h>

#include "../platform.h"

void rt_crash_explain(char *out, size_t n, const char *log_path);  // runtime/crash_explain.c

// ---- guest address space
// The window is reserved up front and committed per region (plat_commit from the loader / kernel). Anything
// the guest touches outside a committed region is committed on its first access by the exception handler
// below, so the window behaves like macOS's all-RW reservation. The OS itself (ReadFile into a guest buffer)
// does not fault: such buffers must be inside committed regions.
static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull

static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return false;
    // Two threads touching the same uncommitted chunk: the second faulted before the first committed it and now finds
    // it committed - retry the access. (A page protected on purpose, plat_protect / LARECOMP_WATCH, isn't plain
    // read-write and still goes on to the fault watch.) Fix from letsgo-recomp.
    if (mi.State == MEM_COMMIT) return (mi.Protect & 0xFF) == PAGE_READWRITE && !(mi.Protect & PAGE_GUARD);
    if (mi.State != MEM_RESERVE) return false;
    uint64_t base = a & ~(COMMIT_CHUNK - 1);
    return VirtualAlloc((void *)(uintptr_t)base, COMMIT_CHUNK, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static bool g_report_crashes;

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t fa = er->ExceptionInformation[1];
    // (not for an execute fault: on a read-write page, retrying it would fault again forever)
    if (er->ExceptionInformation[0] != EXCEPTION_EXECUTE_FAULT && lazy_commit(fa)) return EXCEPTION_CONTINUE_EXECUTION;
    if (g_report_crashes && rt_fault_watch(fa, ep->ContextRecord->Rip)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

// The window keeps write tracking (MEM_WRITE_WATCH: the Vulkan renderer's vertex / index cache re-copies only pages the
// CPU wrote, runtime/nvn/vulkan/exec.c). Without it (refused): no tracking, the renderer reads guest memory in place.
static bool g_write_watch;

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p == (void *)(uintptr_t)lo) g_write_watch = true;
    else p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)lo) return false;
    g_lo = lo;
    g_hi = hi;
    AddVectoredExceptionHandler(1, vectored_handler);
    return true;
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (!g_write_watch || addr < g_lo || addr + size > g_hi || max <= 0) return -1;
    ULONG_PTR n = (ULONG_PTR)max;
    DWORD gran = 4096;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)addr, (SIZE_T)size, (PVOID *)pages, &n, &gran) != 0) return -1;
    *page = gran;
    if (n >= (ULONG_PTR)max) ResetWriteWatch((void *)(uintptr_t)addr, (SIZE_T)size);  // (more than fit: caller takes all)
    return (int)n;
}

// Decommit everything: the range stays reserved, and pages come back zeroed when committed again (lazily).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return VirtualFree((void *)(uintptr_t)lo, hi - lo, MEM_DECOMMIT) != 0;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    if (!VirtualAlloc((void *)(uintptr_t)a, e - a, MEM_COMMIT, PAGE_READWRITE))
        fprintf(stderr, "[plat] commit %llx+%llx failed (%lu)\n", (unsigned long long)addr,
                (unsigned long long)size, GetLastError());
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    DWORD p = prot == PLAT_PROT_NONE ? PAGE_NOACCESS : prot == PLAT_PROT_READ ? PAGE_READONLY : PAGE_READWRITE, old;
    if (addr >= g_lo && addr < g_hi) plat_commit(addr, size);
    return VirtualProtect((void *)(uintptr_t)addr, size, p, &old) ? 0 : -1;
}

// ---- time
static LARGE_INTEGER qpf;

uint64_t plat_now_ns(void) {
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((unsigned __int128)t.QuadPart * 1000000000u / (uint64_t)qpf.QuadPart);
}

// The calling thread's CPU time. GetThreadTimes only advances in whole scheduler ticks (15.6 ms): per frame that reads
// 0 or 15.6. QueryThreadCycleTime counts the thread's cycles exactly, in TSC ticks on x64 (invariant TSC), so
// those are converted with the TSC rate measured once against the performance counter (20 ms, at the first call).
// Elsewhere (or if the cycle count fails): GetThreadTimes, kernel + user.
#if defined(__x86_64__) || defined(_M_X64)
#include <intrin.h>
static double tsc_per_ns;
static INIT_ONCE tsc_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK tsc_calibrate(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o, (void)p, (void)c;
    uint64_t q0 = plat_now_ns(), t0 = __rdtsc();
    Sleep(20);
    uint64_t q1 = plat_now_ns(), t1 = __rdtsc();
    if (q1 > q0 && t1 > t0) tsc_per_ns = (double)(t1 - t0) / (double)(q1 - q0);
    return TRUE;
}
#endif
uint64_t plat_thread_cpu_ns(void) {
#if defined(__x86_64__) || defined(_M_X64)
    InitOnceExecuteOnce(&tsc_once, tsc_calibrate, NULL, NULL);
    ULONG64 cyc;
    if (tsc_per_ns > 0 && QueryThreadCycleTime(GetCurrentThread(), &cyc)) return (uint64_t)((double)cyc / tsc_per_ns);
#endif
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    uint64_t kt = ((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime, ut = ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (kt + ut) * 100;  // (100 ns units)
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

void plat_sleep_until_ns(uint64_t t) {
    static __thread HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { YieldProcessor(); continue; }  // last 0.2 ms: spin (timer slack)
        LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((left - 100000) / 100)};  // relative, 100 ns units
        if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { Sleep(1); continue; }
        WaitForSingleObject(timer, INFINITE);
    }
}

int plat_cpu_count(void) {
    DWORD_PTR proc = 0, sys = 0;
    return GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) ? __builtin_popcountll((unsigned long long)proc) : 0;
}

void plat_thread_guest(int prio) { (void)prio; }  // (the OS's default scheduling serves the game here)

// ---- performance cores (hybrid CPUs: P-cores + E-cores)
// Windows may run the game, render and vsync threads on efficiency cores, where they run far slower (another Switch
// recomp measured 40%). Each of those threads opts out of power throttling (EcoQoS) and, on a hybrid CPU, selects the
// CPU sets of the faster cores (EfficiencyClass above the lowest). Selected CPU sets are a soft preference, never a
// hard pin, and they are only the cores the process affinity allows: when the affinity already excludes the E-cores
// (or the P-cores), nothing is selected, so an affinity-limited run (e.g. 0xF) schedules exactly as before.
// Not hybrid: no CPU sets. LARECOMP_PCORE=0: none of this (the process-wide opt-out in plat_init stays).
static INIT_ONCE pcore_once = INIT_ONCE_STATIC_INIT;
static bool pcore_off;
static ULONG *pcore_ids;  // the selected CPU set ids (NULL: no selection)
static ULONG pcore_n;
static const char *pcore_how = "";

static BOOL CALLBACK pcore_init(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o, (void)p, (void)c;
    const char *e = getenv("LARECOMP_PCORE");
    if (e && *e && atoi(e) == 0) {
        pcore_off = true;
        fprintf(stderr, "[pcore] off (LARECOMP_PCORE=0): default scheduling, thread power throttling unchanged\n");
        return TRUE;
    }
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    uint8_t *buf = len ? malloc(len) : NULL;
    if (!buf || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        free(buf);
        pcore_how = " (CPU sets unavailable)";
        fprintf(stderr, "[pcore] CPU set information unavailable (error %lu): power throttling off only\n", GetLastError());
        return TRUE;
    }
    // The process affinity mask covers its primary group only: filter by it when the system has a single group.
    DWORD_PTR proc = 0, sys = 0;
    bool filter = GetActiveProcessorGroupCount() == 1 && GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc;
    int lo = 255, hi = -1, all_p = 0, all_e = 0, ok_p = 0, ok_e = 0, n = 0;
    for (ULONG off = 0; off < len;) {  // pass 1: the efficiency classes
        SYSTEM_CPU_SET_INFORMATION *i = (SYSTEM_CPU_SET_INFORMATION *)(buf + off);
        if (!i->Size) break;
        if (i->Type == CpuSetInformation) {
            int ec = i->CpuSet.EfficiencyClass;
            if (ec < lo) lo = ec;
            if (ec > hi) hi = ec;
            n++;
        }
        off += i->Size;
    }
    bool hybrid = n && hi > lo;
    if (hybrid) pcore_ids = calloc((size_t)n, sizeof *pcore_ids);
    for (ULONG off = 0; off < len;) {  // pass 2: count, and the P-cores the affinity allows
        SYSTEM_CPU_SET_INFORMATION *i = (SYSTEM_CPU_SET_INFORMATION *)(buf + off);
        if (!i->Size) break;
        if (i->Type == CpuSetInformation) {
            bool perf = i->CpuSet.EfficiencyClass > lo;
            bool allowed = !filter || (i->CpuSet.Group == 0 && i->CpuSet.LogicalProcessorIndex < 64 &&
                                       (proc >> i->CpuSet.LogicalProcessorIndex) & 1);
            perf ? all_p++ : all_e++;
            if (allowed) perf ? ok_p++ : ok_e++;
            if (allowed && perf && pcore_ids) pcore_ids[pcore_n++] = i->CpuSet.Id;
        }
        off += i->Size;
    }
    free(buf);
    if (!hybrid) {
        pcore_how = " (not hybrid: no CPU sets)";
        fprintf(stderr, "[pcore] CPU not hybrid (%d logical processors, one efficiency class): power throttling off, "
                        "no CPU sets\n", n);
    } else if (!ok_p || !ok_e) {
        free(pcore_ids), pcore_ids = NULL, pcore_n = 0;
        pcore_how = " (affinity decides: no CPU sets)";
        fprintf(stderr, "[pcore] hybrid CPU: %d performance + %d efficiency logical processors; the process affinity "
                        "allows %d + %d: no CPU sets (the affinity decides), power throttling off\n", all_p, all_e, ok_p, ok_e);
    } else {
        pcore_how = " (performance-core CPU sets)";
        fprintf(stderr, "[pcore] hybrid CPU: %d performance + %d efficiency logical processors%s; game/render/vsync "
                        "threads prefer the %d performance ones, power throttling off\n", all_p, all_e,
                filter && (ok_p != all_p || ok_e != all_e) ? " (affinity-limited)" : "", ok_p);
    }
    return TRUE;
}

void plat_thread_perf_core(int role) {
    InitOnceExecuteOnce(&pcore_once, pcore_init, NULL, NULL);
    if (pcore_off) return;
    THREAD_POWER_THROTTLING_STATE tp = {
        .Version = THREAD_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED,
        .StateMask = 0,  // (controlled, and off: never EcoQoS)
    };
    bool thr = SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &tp, sizeof tp) != 0;
    bool sets = !pcore_ids || SetThreadSelectedCpuSets(GetCurrentThread(), pcore_ids, pcore_n) != 0;
    static volatile LONG logged;  // (once per role: a reboot's new game thread applies the same, quietly)
    static const char *const names[] = {"game", "render", "vsync"};
    if (role < 0 || role > 2 || (InterlockedOr(&logged, 1 << role) & (1 << role))) return;
    fprintf(stderr, "[pcore] %s thread: power throttling %s%s%s\n", names[role], thr ? "off" : "unchanged (refused)",
            pcore_how, sets ? "" : " (CPU sets refused)");
}

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

// ---- process
// GUI launches keep their own append-only log; lasrecomp.log names the latest run, with five recent backups.
// A launcher and the game it starts share one session, so another launch cannot truncate either writer.
static char g_log_path[MAX_PATH], g_log_latest[MAX_PATH];
static char g_log_tag[MAX_PATH + 16];
static HANDLE g_log_mutex;

static bool lock_logs(void) {
    DWORD r = g_log_mutex ? WaitForSingleObject(g_log_mutex, 5000) : WAIT_FAILED;
    return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
}

// A shared lock outside the log's data marks it as live, including across the launcher's child processes.
// Pruning takes the exclusive form, so an older session still running keeps its path until the next cleanup.
static bool log_live_lock(HANDLE file, bool exclusive) {
    OVERLAPPED at = {.Offset = 0xffffffffu, .OffsetHigh = 0x7fffffffu};
    return LockFileEx(file, LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0, 1, 0, &at);
}

typedef struct { char name[MAX_PATH]; FILETIME created; } SessionLog;
static int newest_log(const void *a, const void *b) {
    const SessionLog *x = a, *y = b;
    int time = CompareFileTime(&y->created, &x->created);
    return time ? time : strcmp(y->name, x->name);
}
static bool session_log_name(const char *name) {  // lasrecomp-backup-<filetime hex>-<pid>.log
    if (strncmp(name, "lasrecomp-backup-", 17)) return false;
    const char *p = name + 17;
    while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || *p == '-') p++;
    return p > name + 17 && !strcmp(p, ".log");
}
static void keep_legacy_log(const char *dir) {
    WIN32_FILE_ATTRIBUTE_DATA old;
    if (!GetFileAttributesExA(g_log_latest, GetFileExInfoStandard, &old) ||
        old.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return;
    // The fallback copy has its own creation time, but still names the original session in its first line.
    char old_tag[MAX_PATH + 16] = {0};
    HANDLE old_file = CreateFileA(g_log_latest, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (old_file != INVALID_HANDLE_VALUE) {
        DWORD got;
        ReadFile(old_file, old_tag, sizeof old_tag - 1, &got, NULL); CloseHandle(old_file);
    }
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%slasrecomp-backup-*.log", dir);
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(path, &data);
    bool kept = false;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            char tag[MAX_PATH + 16]; snprintf(tag, sizeof tag, "[session] %s\n", data.cFileName);
            kept |= !strncmp(old_tag, tag, strlen(tag)) || !CompareFileTime(&data.ftCreationTime, &old.ftCreationTime);
        }
        while (!kept && FindNextFileA(find, &data));
        FindClose(find);
    }
    // The first launch after upgrading may only have the old lasrecomp.log; retain that report too.
    if (!kept) {
        unsigned long long stamp = ((unsigned long long)old.ftCreationTime.dwHighDateTime << 32) | old.ftCreationTime.dwLowDateTime;
        if (snprintf(path, sizeof path, "%slasrecomp-backup-%016llx-0.log", dir, stamp) < (int)sizeof path)
            CopyFileA(g_log_latest, path, TRUE);
    }
}
static void prune_logs(const char *dir) {
    char path[MAX_PATH];
    if (snprintf(path, sizeof path, "%slasrecomp-backup-*.log", dir) >= (int)sizeof path) return;
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(path, &data);
    if (find == INVALID_HANDLE_VALUE) return;
    SessionLog *logs = NULL;
    size_t n = 0;
    do {
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) || !session_log_name(data.cFileName)) continue;
        SessionLog *next = realloc(logs, (n + 1) * sizeof *logs);
        if (!next) { free(logs); FindClose(find); return; }
        logs = next;
        snprintf(logs[n].name, sizeof logs[n].name, "%s", data.cFileName);
        logs[n++].created = data.ftCreationTime;
    } while (FindNextFileA(find, &data));
    FindClose(find);
    if (n > 1) qsort(logs, n, sizeof *logs, newest_log);
    for (size_t i = 5; i < n; i++) {
        if (snprintf(path, sizeof path, "%s%s", dir, logs[i].name) >= (int)sizeof path) continue;
        HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        if (log_live_lock(f, true)) DeleteFileA(path);
        CloseHandle(f);
    }
    free(logs);
}

// NTFS can give the newest session both names, keeping lasrecomp.log current even after a crash. On filesystems
// without hard links, copy on exit instead. The unique session header identifies who owns the latest report;
// creation times can collide for simultaneous launches. The startup mutex also covers this replacement.
static void publish_log(void) {
    if (!g_log_path[0] || !lock_logs()) return;
    fflush(stdout); fflush(stderr);
    HANDLE latest = CreateFileA(g_log_latest, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BY_HANDLE_FILE_INFORMATION current, session;
    HANDLE source = (HANDLE)_get_osfhandle(_fileno(stderr));
    char tag[sizeof g_log_tag]; DWORD got = 0;
    bool copy = latest != INVALID_HANDLE_VALUE && ReadFile(latest, tag, strlen(g_log_tag), &got, NULL) &&
                got == strlen(g_log_tag) && !memcmp(tag, g_log_tag, got) && GetFileInformationByHandle(latest, &current) &&
                GetFileInformationByHandle(source, &session) &&
                (current.dwVolumeSerialNumber != session.dwVolumeSerialNumber || current.nFileIndexHigh != session.nFileIndexHigh ||
                 current.nFileIndexLow != session.nFileIndexLow);
    if (latest != INVALID_HANDLE_VALUE) CloseHandle(latest);
    if (copy) CopyFileA(g_log_path, g_log_latest, FALSE);
    ReleaseMutex(g_log_mutex);
}

static void log_to_file_without_console(void) {
    // An inherited pipe/file (tools, terminal) is already somewhere useful. Only the GUI launch needs redirecting.
    // A relaunched game must still join its GUI session and publish its fallback copy on exit.
    const char *inherited = getenv("LASRECOMP_LOG_FILE");
    bool fresh = !inherited || !*inherited;
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (fresh && err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN) return;
    char dir[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, dir, sizeof dir);
    if (!n || n >= sizeof dir) return;
    char *slash = strrchr(dir, '\\');
    if (!slash) return;
    slash[1] = 0;
    if (snprintf(g_log_latest, sizeof g_log_latest, "%slasrecomp.log", dir) >= (int)sizeof g_log_latest) return;
    unsigned long long hash = 14695981039346656037ull;
    for (const unsigned char *p = (const unsigned char *)g_log_latest; *p; p++) hash = (hash ^ *p) * 1099511628211ull;
    char mutex[64]; snprintf(mutex, sizeof mutex, "Local\\LASRecomp.logs.%016llx", hash);
    g_log_mutex = CreateMutexA(NULL, FALSE, mutex);
    if (!lock_logs()) return;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    unsigned long long stamp = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
    int used = fresh ? snprintf(g_log_path, sizeof g_log_path, "%slasrecomp-backup-%016llx-%lu.log", dir, stamp,
                               (unsigned long)GetCurrentProcessId())
                     : snprintf(g_log_path, sizeof g_log_path, "%s", inherited);
    HANDLE file = INVALID_HANDLE_VALUE;
    if (used >= 0 && (size_t)used < sizeof g_log_path)
        file = CreateFileA(g_log_path, GENERIC_READ | FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           fresh ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || !log_live_lock(file, false)) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        g_log_path[0] = 0; ReleaseMutex(g_log_mutex); return;
    }
    const char *name = strrchr(g_log_path, '\\');
    snprintf(g_log_tag, sizeof g_log_tag, "[session] %s\n", name ? name + 1 : g_log_path);
    // A GUI-subsystem process with no inherited handles starts with invalid CRT stream descriptors.
    // Give both streams real descriptors before duplicating the shared-delete log handle into them.
    if (!freopen("NUL", "ab", stderr) || !freopen("NUL", "ab", stdout)) {
        CloseHandle(file); g_log_path[0] = 0; ReleaseMutex(g_log_mutex); return;
    }
    int fd = _open_osfhandle((intptr_t)file, _O_WRONLY | _O_APPEND | _O_BINARY);
    if (fd < 0 || _dup2(fd, _fileno(stderr)) != 0) {
        if (fd < 0) CloseHandle(file); else _close(fd);
        g_log_path[0] = 0; ReleaseMutex(g_log_mutex); return;
    }
    _close(fd);
    setvbuf(stderr, NULL, _IONBF, 0);
    _dup2(_fileno(stderr), _fileno(stdout));
    _putenv_s("LASRECOMP_LOG_FILE", g_log_path);
    if (fresh) {
        fputs(g_log_tag, stderr);
        keep_legacy_log(dir);
        char next[MAX_PATH];
        // A shorter name than the session path, which was already checked above.
        snprintf(next, sizeof next, "%slasrecomp-current-%016llx-%lu.tmp", dir, stamp, (unsigned long)GetCurrentProcessId());
        bool staged = CreateHardLinkA(next, g_log_path, NULL) != 0;
        if (!staged) {
            // Keep the session header in the placeholder too; the full copy at exit preserves it.
            HANDLE marker = CreateFileA(next, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (marker != INVALID_HANDLE_VALUE) {
                DWORD written;
                staged = WriteFile(marker, g_log_tag, strlen(g_log_tag), &written, NULL) && written == strlen(g_log_tag);
                CloseHandle(marker);
                if (!staged) DeleteFileA(next);
            }
        }
        if (staged) {
            MoveFileExA(next, g_log_latest, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            DeleteFileA(next);
        }
        prune_logs(dir);
    }
    atexit(publish_log);
    ReleaseMutex(g_log_mutex);
}

// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2): say so instead of crashing
// with an illegal instruction on an older CPU.
static void require_cpu(void) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2")) return;
    MessageBoxA(NULL, "LAS Recomp needs a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Excavator / Zen or newer).",
                "LAS Recomp", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

// Fatal error (rt_fatal_trace): what happened in plain words (runtime/crash_explain.c) in a message box, when there is
// no console; the technical lines stay in the log.
void plat_fatal_notice(void) {
    publish_log();
    char text[1600];
    rt_crash_explain(text, sizeof text, g_log_path);
    if (!g_log_path[0]) return;
    fflush(stderr);
    MessageBoxA(NULL, text, "LAS Recomp", MB_ICONERROR | MB_OK);
}

static DWORD WINAPI notice_thread(LPVOID text) {
    MessageBoxA(NULL, text, "LAS Recomp", MB_ICONWARNING | MB_OK);
    free(text);
    return 0;
}
void plat_notice(const char *text) {
    if (!g_log_path[0]) return;
    char *copy = _strdup(text);
    HANDLE t = copy ? CreateThread(NULL, 0, notice_thread, copy, 0, NULL) : NULL;
    if (t) CloseHandle(t);
    else free(copy);
}

bool plat_module_name(uint64_t pc, char *out, size_t n) {
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)pc, &m))
        return false;
    return GetModuleFileNameA(m, out, (DWORD)n) > 0;
}

void plat_init(void) {
    // abort() (rt_fatal_trace, after the crash dialog) without the C runtime's own "Microsoft Visual C++ Runtime
    // Library - abort() has been called" box and the Windows error report: the player sees only LAS Recomp's dialog
#ifdef _WRITE_ABORT_MSG  // (MinGW's stdlib.h declares it with these flags)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    log_to_file_without_console();
    require_cpu();
    _fmode = _O_BINARY;  // fopen/open default to binary (the runtime's "r"/"w" files are binary or LF text)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    timeBeginPeriod(1);  // 1 ms Sleep / condition-variable timeouts (default 15.6 ms)
    // Opt out of EcoQoS and of Windows 11 ignoring the timer resolution while the window is occluded.
    PROCESS_POWER_THROTTLING_STATE pt = {
        .Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
        .StateMask = 0,
    };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
}

bool plat_exe_path(char *buf, size_t size) {
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
    return n > 0 && n < size;
}

// Quote one argument the way CommandLineToArgvW / the CRT parse it back.
static void append_arg(char *out, size_t cap, const char *a) {
    size_t n = strlen(out);
    bool q = !*a || strpbrk(a, " \t\"") != NULL;
    if (n && n + 1 < cap) out[n++] = ' ';
    if (q && n + 1 < cap) out[n++] = '"';
    for (const char *p = a; *p; p++) {
        int bs = 0;
        while (*p == '\\') { bs++; p++; }
        int reps = !*p ? (q ? 2 * bs : bs) : *p == '"' ? 2 * bs + 1 : bs;
        for (int i = 0; i < reps && n + 1 < cap; i++) out[n++] = '\\';
        if (!*p) break;
        if (n + 1 < cap) out[n++] = *p;
    }
    if (q && n + 1 < cap) out[n++] = '"';
    out[n] = 0;
}

// No exec() on Windows: start the new process with the same console / standard handles, then exit.
void plat_exec(char *const argv[]) {
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    BOOL ok;
    fflush(stdout);
    fflush(stderr);
    if (!argv) {
        ok = CreateProcessW(NULL, GetCommandLineW(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    } else {
        static char cmd[32768];
        cmd[0] = 0;
        for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
        static wchar_t wcmd[32768];
        MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
        ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    }
    if (!ok) return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
}

// ---- diagnostics
// DLL frames are named module+offset; frames in our own executable are left to the caller, which maps
// them to guest functions (runtime/dispatch.c) - mingw builds carry DWARF, not PDB symbols.
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    static __thread char name[MAX_PATH];
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)pc, &m))
        return NULL;
    if (m == GetModuleHandleA(NULL)) return NULL;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, sizeof path)) return NULL;
    const char *b = strrchr(path, '\\');
    snprintf(name, sizeof name, "%s", b ? b + 1 : path);
    if (offset) *offset = pc - (uint64_t)(uintptr_t)m;
    return name;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    USHORT n = RtlCaptureStackBackTrace(0, 64, bt, NULL);
    uint64_t exe = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = plat_symbol(pc, &off);
        if (!s) s = rt_host_fn_name(pc, &off);
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, s, (unsigned long long)off);
        else fprintf(stderr, "  #%-2d %016llx lasrecomp+%llx\n", i, (unsigned long long)pc,
                     (unsigned long long)(pc - exe));
    }
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uint64_t fa = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    char what[48];
    snprintf(what, sizeof what, "exception %08lx", er->ExceptionCode);
    // x64 has no link register: report the return address on top of the stack (valid at function entry).
    uint64_t lr = 0;
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((void *)(uintptr_t)ep->ContextRecord->Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT)
        lr = *(uint64_t *)(uintptr_t)ep->ContextRecord->Rsp;
    rt_fault_report(what, fa, ep->ContextRecord->Rip, lr);
}

void plat_crash_handler_install(void) {
    ULONG guarantee = 64 << 10;  // room for the report after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    if (g_report_crashes) return;
    g_report_crashes = true;
    SetUnhandledExceptionFilter(unhandled_filter);
}

// ---- POSIX shims (win_compat.h)
static ssize_t prw(int fd, void *buf, size_t n, off_t off, bool write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov = {0};
        uint64_t o = (uint64_t)off + done;
        ov.Offset = (DWORD)o;
        ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD chunk = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done), got = 0;
        BOOL ok = write ? WriteFile(h, (const char *)buf + done, chunk, &got, &ov)
                        : ReadFile(h, (char *)buf + done, chunk, &got, &ov);
        if (!ok) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return done ? (ssize_t)done : -1;
        }
        if (!got) break;
        done += got;
    }
    return (ssize_t)done;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) { return prw(fd, buf, n, off, false); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return prw(fd, (void *)buf, n, off, true); }

// ---- child processes
typedef struct { HANDLE process, job; } WinProcess;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    static __thread char cmd[32768];
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
    static __thread wchar_t wcmd[32768], wlog[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
    MultiByteToWideChar(CP_UTF8, 0, log_path ? log_path : "NUL", -1, wlog, MAX_PATH);
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
    HANDLE log = CreateFileW(wlog, append_log ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, append_log ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) return 0;
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = NULL, .hStdOutput = log,
                       .hStdError = log};
    PROCESS_INFORMATION pi;
    // CreateProcess doesn't resolve relative paths written with '/' (e.g. a helper under tools/): pass the program
    // as a full path, like posix_spawn takes it as given
    static __thread wchar_t wprog[MAX_PATH], wfull[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[0], -1, wprog, MAX_PATH);
    for (wchar_t *p = wprog; *p; p++) if (*p == L'/') *p = L'\\';
    const wchar_t *app = GetFullPathNameW(wprog, MAX_PATH, wfull, NULL) ? wfull : NULL;
    BOOL ok = CreateProcessW(app, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    if (!ok) fprintf(stderr, "[plat] could not start %s (error %lu)\n", argv[0], GetLastError());
    CloseHandle(log);
    if (!ok) return 0;
    // A separate job per helper: cancelling one compiler also stops its children immediately,
    // without killing the other compilation jobs. Handles close on app exit as well.
    WinProcess *child = calloc(1, sizeof *child);
    if (!child) {
        TerminateProcess(pi.hProcess, 255);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 0;
    }
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li) ||
        !AssignProcessToJobObject(job, pi.hProcess)) {
        // A restricted parent job can prevent assignment. Still run the helper, as before jobs were added;
        // cancellation can stop this process only in that case, not its descendants.
        fprintf(stderr, "[plat] could not supervise %s (error %lu); starting without a job\n", argv[0], GetLastError());
        if (job) CloseHandle(job);
        job = NULL;
    }
    child->process = pi.hProcess, child->job = job;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return (PlatProc)child;
}

void plat_proc_kill(PlatProc p) {
    WinProcess *child = (WinProcess *)p;
    if (child->job) TerminateJobObject(child->job, 255);
    else TerminateProcess(child->process, 255);
    WaitForSingleObject(child->process, INFINITE);
    CloseHandle(child->process);
    if (child->job) CloseHandle(child->job);
    free(child);
}

int plat_proc_wait(PlatProc p, bool block) {
    WinProcess *child = (WinProcess *)p;
    DWORD w = WaitForSingleObject(child->process, block ? INFINITE : 0);
    if (w == WAIT_TIMEOUT) return -1;
    DWORD code = 255;
    if (w == WAIT_OBJECT_0) GetExitCodeProcess(child->process, &code);
    CloseHandle(child->process);
    if (child->job) CloseHandle(child->job);
    free(child);
    return code > 255 ? 255 : (int)code;  // (a crash's NTSTATUS, e.g. 0xC0000409, is not "still running" (-1))
}

bool plat_remove_tree(const char *dir) {
    // SHFileOperation wants a double-NUL-terminated path with backslashes
    static __thread wchar_t w[MAX_PATH + 2];
    int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, MAX_PATH);
    if (n <= 1) return false;
    w[n] = 0;
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    SHFILEOPSTRUCTW op = {.wFunc = FO_DELETE, .pFrom = w, .fFlags = FOF_NO_UI};
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

// End the process at once, skipping DLL teardown (from an atexit handler, after flushing). Exit status 0.
void plat_terminate(void) { publish_log(); TerminateProcess(GetCurrentProcess(), 0); }
