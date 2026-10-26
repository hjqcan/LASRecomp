// Builds the game code library on the player's machine (runtime/gamebuild.h).
#include "gamebuild.h"
#include "build_cache.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <signal.h>
#endif

#include "gamelib.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"

// From the Makefile: the flags the game code is compiled and linked with (as `make` builds it in a source checkout)
#ifndef GAMELIB_CFLAGS
#define GAMELIB_CFLAGS "-O1 -fno-strict-aliasing -fvisibility=hidden"
#endif
#ifndef GAMELIB_SHARED
#define GAMELIB_SHARED "-shared"
#endif
#ifndef GAMELIB_BASE_ID
#define GAMELIB_BASE_ID "dev"
#endif

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

// ------------------------------------------------------------------ child processes
// Every one running, so that quitting mid-build ends them too (atexit) instead of leaving compilers behind.
#define MAX_PROCS 80
static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
static PlatProc g_procs[MAX_PROCS];
static int g_nprocs;
static bool g_quitting;

static void end_all(void) {
    pthread_mutex_lock(&g_plock);
    g_quitting = true;
    for (int i = 0; i < g_nprocs; i++) plat_proc_kill(g_procs[i]);
    g_nprocs = 0;
    pthread_mutex_unlock(&g_plock);
}

static PlatProc spawn_proc(char **argv, const char *log, bool generator) {
    pthread_mutex_lock(&g_plock);
    PlatProc p = 0;
    if (!g_quitting) {
#ifdef _WIN32
        // The generator runs with Control Flow Guard off even when Windows forces it on for every program (#337)
        if (generator) {
            unsigned long error = 0;
            p = plat_spawn_no_cfg((const char *const *)argv, log, false, &error);
            static bool told;
            if (p && !told) {
                told = true;
                if (!error) LOG("[gamebuild] generator started with Control Flow Guard off\n");
                else LOG("[gamebuild] generator started normally: Control Flow Guard could not be turned off for it (error %lu)\n", error);
            }
        } else
#endif
        p = plat_spawn((const char *const *)argv, log, false);
    }
    (void)generator;
    if (p && g_nprocs < MAX_PROCS) g_procs[g_nprocs++] = p;
    pthread_mutex_unlock(&g_plock);
    return p;
}
static PlatProc spawn(char **argv, const char *log) { return spawn_proc(argv, log, false); }

// plat_proc_wait without blocking, then forgotten once it has ended
static int finished(PlatProc p) {
    pthread_mutex_lock(&g_plock);
    int r = plat_proc_wait(p, false);
    if (r >= 0)
        for (int i = 0; i < g_nprocs; i++)
            if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; break; }
    pthread_mutex_unlock(&g_plock);
    return r;
}

static void stop(PlatProc p) {  // ends one that is still running, then forgotten
    pthread_mutex_lock(&g_plock);
    bool ours = false;
    for (int i = 0; i < g_nprocs; i++)
        if (g_procs[i] == p) { g_procs[i] = g_procs[--g_nprocs]; ours = true; break; }
    pthread_mutex_unlock(&g_plock);
    if (ours) plat_proc_kill(p);
}

static void sleep_ms(int ms) { plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000ull); }

// ------------------------------------------------------------------ argument lists
typedef struct { char **v; int n, cap; } Args;
static void add(Args *a, const char *s) {
    if (a->n + 2 > a->cap) {
        a->cap = a->cap ? a->cap * 2 : 32;
        a->v = (char **)realloc(a->v, sizeof *a->v * a->cap);
    }
    a->v[a->n++] = strdup(s);
    a->v[a->n] = NULL;
}
static void add_split(Args *a, const char *flags) {  // (space-separated, no quoting)
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", flags);
    for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) add(a, t);
}
static void args_free(Args *a) {
    for (int i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    memset(a, 0, sizeof *a);
}

// ------------------------------------------------------------------ the tools
typedef struct {
    char gen[1100], gen_script[1100];  // generator: lasgen, or (source checkout) the venv's python + tools/gen.py
    char cc[1100];                     // compiler: zig (as `zig cc`), or LARECOMP_CC / clang
    bool zig;
    char inc[1100], entry[1100], hle[1100];  // headers the game code includes, tools/gamelib_entry.c, HLE names
#ifdef _WIN32
    char implib[1100];  // the .exe's import library (the runtime functions the game code calls)
    char zig_rt[1100];  // the release's zig: its runtime built ahead of time (zig/rt, tools/zig_slim.sh), "" = none
#endif
} Tools;

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static bool find_tools(Tools *t) {
    memset(t, 0, sizeof *t);
    const char *e = getenv("LARECOMP_GEN");
    if (e && *e) snprintf(t->gen, sizeof t->gen, "%s", e);
    else if (!rt_resource("lasgen" SEP "lasgen" PLAT_EXE_SUFFIX, NULL, t->gen, sizeof t->gen) &&
             !(rt_resource(NULL, "tools/" PLAT_VENV_PYTHON, t->gen, sizeof t->gen) &&
               rt_resource(NULL, "tools/gen.py", t->gen_script, sizeof t->gen_script)))
        return false;
    e = getenv("LARECOMP_CC");
    if (e && *e) snprintf(t->cc, sizeof t->cc, "%s", e);
    else if (rt_resource("zig" SEP "zig" PLAT_EXE_SUFFIX, NULL, t->cc, sizeof t->cc)) t->zig = true;
#ifndef _WIN32
    else if (rt_dev_mode() && file_exists("/usr/bin/clang")) snprintf(t->cc, sizeof t->cc, "/usr/bin/clang");
#endif
    else return false;
    if (!rt_resource("gamelib-src" SEP "cpu.h", "runtime/cpu.h", t->inc, sizeof t->inc) ||
        !rt_resource("gamelib-src" SEP "gamelib_entry.c", "tools/gamelib_entry.c", t->entry, sizeof t->entry) ||
        !rt_resource("gamelib-src" SEP "hle_symbols.txt", "runtime/hle/hle_symbols.txt", t->hle, sizeof t->hle))
        return false;
    char *s = strrchr(t->inc, '/'), *b = strrchr(t->inc, '\\');
    if (b > s) s = b;
    if (s) *s = 0;
#ifdef _WIN32
    if (!rt_resource("lasrecomp.dll.a", "build/lasrecomp.dll.a", t->implib, sizeof t->implib)) return false;
    t->zig_rt[0] = 0;
    if (t->zig && rt_resource("zig" SEP "rt" SEP "link.rsp", NULL, t->zig_rt, sizeof t->zig_rt)) {
        char *s = strrchr(t->zig_rt, '/'), *b = strrchr(t->zig_rt, '\\');
        if (b > s) s = b;
        if (s) *s = 0;
        else t->zig_rt[0] = 0;
    }
#endif
    return true;
}

bool gamebuild_available(void) {
    Tools t;
    return find_tools(&t);
}

// zig cc on Linux: an explicit target, so it uses its own glibc headers and stubs instead of looking for the system's
// (SteamOS and other systems without development packages have none: "failed to find libc installation", #181)
#if defined(__linux__) && defined(__x86_64__)
#define ZIG_TARGET "x86_64-linux-gnu"
#elif defined(__linux__) && defined(__aarch64__)
#define ZIG_TARGET "aarch64-linux-gnu"
#else
#define ZIG_TARGET ""
#endif

static void compiler(Args *a, const Tools *t) {
    add(a, t->cc);
    if (t->zig) add(a, "cc");
    if (t->zig && ZIG_TARGET[0]) add(a, "-target"), add(a, ZIG_TARGET);
}

// ------------------------------------------------------------------ generator progress (its log)
typedef struct {
    const char *name;
    char log[1100], out[1100];
    PlatProc p;
    int exit;                // -1 while running
    int files;               // chunk files written so far (complete)
    unsigned long long ci, n;  // functions written / all
} Gen;

static void read_gen_log(Gen *g) {
    FILE *f = fopen(g->log, "rb");
    if (!f) return;
    static char buf[1 << 16];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    long from = size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0;
    fseek(f, from, SEEK_SET);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *last = NULL;
    for (const char *p = strstr(buf, ": progress "); p; p = strstr(p + 1, ": progress ")) last = p;
    unsigned long long ci, all;
    int files;
    if (last && sscanf(last, ": progress %llu/%llu files %d", &ci, &all, &files) == 3) {
        g->ci = ci, g->n = all;
        if (files > g->files) g->files = files;
    }
}

static void log_tail(const char *path, char *out, size_t n) {  // the last line of a log (an error message)
    out[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, size > (long)sizeof buf - 1 ? size - (long)sizeof buf + 1 : 0, SEEK_SET);
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = 0;
    while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    char *nl = strrchr(buf, '\n');
    snprintf(out, n, "%s", nl ? nl + 1 : buf);
}

// A helper that failed (plat_proc_wait's r != 0): its log's last line for our log, and how it ended when that says
// nothing or a signal ended it (the system killing it for memory writes nothing). A signal also gives the player why.
static bool g_killed, g_cfg_stopped;
static void helper_failed(const char *what, int r, const char *log, char *why, size_t why_size) {
    char last[300], how[120] = "";
    log_tail(log, last, sizeof last);
#ifndef _WIN32
    if (r > PLAT_PROC_SIGNALED) {
        int sig = r - PLAT_PROC_SIGNALED;
        if (sig == SIGKILL) {
            snprintf(how, sizeof how, "killed by signal %d (SIGKILL: likely out of memory)", sig);
            snprintf(why, why_size, "A build helper was killed by the system, likely out of memory.");
        } else {
            snprintf(how, sizeof how, "killed by signal %d (%s)", sig, strsignal(sig));
            snprintf(why, why_size, "A build helper crashed (signal %d).", sig);
        }
        g_killed = true;
    } else
#endif
#ifdef _WIN32
    if (r == PLAT_PROC_FAST_FAIL) {
        snprintf(how, sizeof how, "exit status 0xC0000409 (a security check stopped it)");
    } else
#endif
    if (!last[0]) {
        snprintf(how, sizeof how, "exit status %d", r);
    }
    LOG("[gamebuild] %s failed: %s%s%s\n", what, how, how[0] && last[0] ? ": " : "", last);
}

bool gamebuild_killed(void) { return g_killed; }
bool gamebuild_cfg_stopped(void) { return g_cfg_stopped; }

// ------------------------------------------------------------------ compiling
typedef struct {
    char src[1100], obj[1100], tmp[1100], log[1100], stem[64], source_hash[65];
    unsigned long long bytes;
} Unit;

typedef struct {
    Unit *u;
    int n, cap;
} Units;

static void add_unit(Units *us, const char *src, const char *work, const char *stem) {
    if (us->n == us->cap) {
        us->cap = us->cap ? us->cap * 2 : 64;
        us->u = (Unit *)realloc(us->u, sizeof *us->u * us->cap);
    }
    Unit *u = &us->u[us->n++];
    snprintf(u->src, sizeof u->src, "%s", src);
    snprintf(u->obj, sizeof u->obj, "%s" SEP "%s.o", work, stem);
    snprintf(u->tmp, sizeof u->tmp, "%s" SEP "%s.o.tmp", work, stem);
    snprintf(u->stem, sizeof u->stem, "%s", stem);
    snprintf(u->log, sizeof u->log, "%s" SEP "%s.log", work, stem);
    struct stat st;
    u->bytes = stat(src, &st) == 0 ? (unsigned long long)st.st_size : 0;
}

// The flags the game code was built with (GAMELIB_CFLAGS, part of its id). zig cc reads -march=<cpu> with its own CPU
// names (x86_64_v3) and takes "-<feature>" off the end, so x86-64-v3 would be CPU "x86": the same CPU as -mcpu there.
static void add_cflags(Args *a, const Tools *t) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", GAMELIB_CFLAGS);
    for (char *f = strtok(buf, " "); f; f = strtok(NULL, " ")) {
        if (t->zig && !strncmp(f, "-march=", 7)) {
            char cpu[128];
            snprintf(cpu, sizeof cpu, "-mcpu=%s", f + 7);
            for (char *c = cpu + 6; *c; c++) if (*c == '-') *c = '_';
            add(a, cpu);
        } else {
            add(a, f);
        }
    }
}

static char g_full_id[GAMELIB_FULL_ID_SIZE];  // this build's library id (gamelib_full_id): GAMELIB_ID-<build ID>

static PlatProc start_compile(const Tools *t, const Unit *u) {
    Args a = {0};
    compiler(&a, t);
    add_cflags(&a, t);
    char inc[1200];
    snprintf(inc, sizeof inc, "-I%s", t->inc);
    add(&a, inc);
    char id[GAMELIB_FULL_ID_SIZE + 32];
    snprintf(id, sizeof id, "-DGAMELIB_ID=\"%s\"", g_full_id);
    add(&a, id);
    add(&a, "-c");
    add(&a, u->src);
    add(&a, "-o");
    add(&a, u->tmp);
    PlatProc p = spawn(a.v, u->log);
    args_free(&a);
    return p;
}

// ------------------------------------------------------------------ the build
bool gamebuild_run(const char *game_dir, const GameBuildOpts *o, char *why, size_t why_size) {
    uint64_t t0 = plat_now_ns();
    static bool registered;
    if (!registered) registered = true, atexit(end_all);
    g_killed = false, g_cfg_stopped = false;
    if (!gamelib_full_id(game_dir, g_full_id, sizeof g_full_id)) {
        snprintf(why, why_size, "the game files' exefs/main is missing or unreadable");
        return false;
    }
    Tools t;
    if (!find_tools(&t)) {
        snprintf(why, why_size, "the game code builder is missing from this copy of LAS Recomp");
        return false;
    }
    char work[1100], tmp[1100];
    snprintf(work, sizeof work, "%s" SEP ".gamebuild", game_dir);
    snprintf(tmp, sizeof tmp, "%s" SEP "%s.tmp", work, GAMELIB_NAME);
    char input_paths[6][1100], spec[4096];
    snprintf(input_paths[0], sizeof input_paths[0], "%s" SEP "exefs" SEP "main", game_dir);
    snprintf(input_paths[1], sizeof input_paths[1], "%s" SEP "exefs" SEP "sdk", game_dir);
    const char *headers[] = {"cpu.h", "simd.h", "simd_fp.h", "gamelib.h"};
    for (int i = 0; i < 4; ++i) snprintf(input_paths[i+2], sizeof input_paths[i+2], "%s" SEP "%s", t.inc, headers[i]);
    // Generator changes should rebuild only chunks whose C changed, as in the existing incremental builder.
    // Compiler/header/flag changes invalidate every object; entry always rebuilds because it embeds the library id.
    const char *inputs[] = {input_paths[2], input_paths[3], input_paths[4], input_paths[5], t.cc};
    // The compiler's contents identify it, so relocating the app does not discard reusable objects.
    snprintf(spec, sizeof spec, "LASBUILD3\n%s\n%s\n%s\nzig=%d pointer=%d%s%s\n", GAMELIB_BASE_ID,
             GAMELIB_CFLAGS, GAMELIB_SHARED, t.zig, (int)sizeof(void *),  // (+ the Linux zig target: objects from
             t.zig && ZIG_TARGET[0] ? " target=" : "", t.zig ? ZIG_TARGET : "");  // before it are rebuilt once)
    if (o->progress) o->progress(0, "Checking completed build files...");
    bool resuming = build_cache_resumable(game_dir);  // (a cancelled or interrupted build; read before open marks this one)
    BuildCache *cache = build_cache_open(game_dir, spec, inputs, 5, why, why_size);
    if (!cache) return false;
    // an update keeps the last good build's objects (open drops them when compiler, headers or flags changed)
    bool update = false;
    if (!resuming) {
        char rp[1200];
        struct stat st;
        snprintf(rp, sizeof rp, "%s" SEP "gamelib_entry.receipt", work);
        update = stat(rp, &st) == 0;
    }
    const char *gen_inputs[5] = {input_paths[0], input_paths[1], t.gen, t.hle};
    int ngen = 4;
    if (t.gen_script[0]) gen_inputs[ngen++] = t.gen_script;
    if (!build_cache_prepare_generation(cache, GAMELIB_ID, gen_inputs, ngen, why, why_size)) {
        build_cache_failed(cache);
        build_cache_close(cache);
        return false;
    }
    if (t.zig) {  // keep its caches with this installation; the explicit Clear action removes them too
        char zc[1200];
        snprintf(zc, sizeof zc, "%s" SEP "zig-cache", work);
#ifdef _WIN32
        _putenv_s("ZIG_GLOBAL_CACHE_DIR", zc);
        _putenv_s("ZIG_LOCAL_CACHE_DIR", zc);
#else
        setenv("ZIG_GLOBAL_CACHE_DIR", zc, 1);
        setenv("ZIG_LOCAL_CACHE_DIR", zc, 1);
#endif
    }
    LOG("[gamebuild] generator %s%s%s, compiler %s%s, %d jobs\n", t.gen, t.gen_script[0] ? " " : "", t.gen_script,
        t.cc, t.zig ? " cc" : "", o->jobs);

    // 1. the generator, both modules at once; their finished files are compiled while it goes on
    Gen gens[2] = {{.name = "main"}, {.name = "sdk"}};
    static const char *const base[2] = {"0x7000000000", "0x7004000000"};
    bool ok = true;
    for (int i = 0; i < 2 && ok; i++) {
        Gen *g = &gens[i];
        g->exit = -1;
        snprintf(g->log, sizeof g->log, "%s" SEP "gen_%s.log", work, g->name);
        snprintf(g->out, sizeof g->out, "%s" SEP "%s", work, g->name);
        if (build_cache_generation(cache, g->name, &g->files)) {
            g->exit = 0;
            LOG("[gamebuild] reused generated %s: %d chunks\n", g->name, g->files);
            continue;
        }
        char nso[1100];
        snprintf(nso, sizeof nso, "%s" SEP "exefs" SEP "%s", game_dir, g->name);
        Args a = {0};
        add(&a, t.gen);
        if (t.gen_script[0]) add(&a, t.gen_script);
        add(&a, g->name);
        add(&a, nso);
        add(&a, base[i]);
        add(&a, g->out);
        if (i == 1) { add(&a, "--hle"); add(&a, t.hle); }
        g->p = spawn_proc(a.v, g->log, true);
        args_free(&a);
        if (!g->p) { snprintf(why, why_size, "the game code builder didn't start"); ok = false; }
    }

    Units units = {0};
    int queued[2] = {0, 0};  // chunk files of each module put in units so far
    bool tables[2] = {false, false};
    int next = 0, jobs = o->jobs < 1 ? 1 : o->jobs > 64 ? 64 : o->jobs;
    struct { PlatProc p; int unit; } run[64];
    int nrun = 0;
    int reused = 0, compiled = 0;
    unsigned long long done_bytes = 0;
    double shown = 0;
    {
        add_unit(&units, t.entry, work, "gamelib_entry");
    }
    while (ok) {
        if (o->cancelled && o->cancelled()) {
            snprintf(why, why_size, "Compilation cancelled. Completed files are kept for Resume."); ok = false; break;
        }
        // generator progress, newly finished files
        bool gens_done = true;
        double gen_frac = 0;
        unsigned long long written = 0;
        for (int i = 0; i < 2; i++) {
            Gen *g = &gens[i];
            if (g->exit < 0) {
                read_gen_log(g);
                int r = finished(g->p);
                if (r >= 0) {
                    g->exit = r;
                    read_gen_log(g);
                    if (r != 0) {
                        char what[64];
                        snprintf(what, sizeof what, "generator (%s)", g->name);
                        snprintf(why, why_size, "the game code couldn't be prepared");
                        helper_failed(what, r, g->log, why, why_size);
#ifdef _WIN32
                        if (r == PLAT_PROC_FAST_FAIL) g_cfg_stopped = true;  // (#337)
#endif
                        ok = false;
                    } else if (!build_cache_save_generation(cache, g->name, g->files, why, why_size)) {
                        ok = false;
                    }
                }
            }
            if (g->exit != 0) gens_done = false;
            for (; queued[i] < g->files; queued[i]++) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_%03d", g->name, queued[i]);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
            }
            if (g->exit == 0 && !tables[i]) {
                char src[1100], stem[64];
                snprintf(stem, sizeof stem, "%s_table", g->name);
                snprintf(src, sizeof src, "%s" SEP "%s.c", g->out, stem);
                add_unit(&units, src, work, stem);
                tables[i] = true;
            }
            gen_frac += g->exit == 0 ? 0.5 : g->n ? 0.5 * (double)g->ci / (double)g->n : 0;
        }
        if (!ok) break;
        for (int k = 0; k < units.n; k++) written += units.u[k].bytes;

        // compiles
        while (nrun < jobs && next < units.n) {
            Unit *u = &units.u[next];
            int cached = build_cache_object(cache, u->stem, u->src, u->source_hash);
            if (cached < 0) { snprintf(why, why_size, "A generated source file is missing or unreadable."); ok = false; break; }
            if (cached && next != 0) { done_bytes += u->bytes; ++reused; ++next; continue; }
            PlatProc p = start_compile(&t, &units.u[next]);
            if (!p) { snprintf(why, why_size, "the compiler didn't start"); ok = false; break; }
            run[nrun].p = p, run[nrun].unit = next++;
            nrun++;
        }
        for (int k = 0; k < nrun; k++) {
            int r = finished(run[k].p);
            if (r < 0) continue;
            Unit *u = &units.u[run[k].unit];
            if (r != 0) {
                char what[1200];
                snprintf(what, sizeof what, "compiling %s", u->src);
                snprintf(why, why_size, "the game code didn't compile");
                helper_failed(what, r, u->log, why, why_size);
                ok = false;
            } else if (!build_cache_save_object(cache, u->stem, u->src, u->source_hash, why, why_size)) {
                ok = false;
            } else {
                ++compiled;
            }
            done_bytes += u->bytes;
            run[k--] = run[--nrun];
        }
        if (!ok) break;
        if (gens_done && next == units.n && nrun == 0) break;

        // progress: the generator's share (its functions written), the compiler's (bytes of C compiled, of an
        // estimate of all of it while the generator still writes)
        double est_total = gens_done ? (double)written : gen_frac > 0.02 ? (double)written / gen_frac : 0;
        double comp_frac = est_total > 0 ? (double)done_bytes / est_total : 0;
        if (comp_frac > 1) comp_frac = 1;
        // (an update that keeps the last build's pieces compiles little: generating the C is most of its time)
        double f = update ? 0.85 * gen_frac + 0.1 * comp_frac : 0.2 * gen_frac + 0.75 * comp_frac;
        double secs = (double)(plat_now_ns() - t0) / 1e9;
        if (gen_frac == 0) f = 0.05 * (secs / (secs + 20));  // (the generator's first pass: no count yet)
        if (f > shown) shown = f;
        if (o->progress) o->progress(shown, gen_frac < 1 && !gens_done ? "Reading the game code..." : resuming ? "Resuming compilation..." : "Recompiling...");
        sleep_ms(50);
    }
    for (int k = 0; k < nrun; k++) stop(run[k].p);  // (a failure: the rest is of no use - the failure shows at once)
    for (int i = 0; i < 2; i++)
        if (gens[i].p && gens[i].exit < 0) stop(gens[i].p);

    // 2. link
    if (ok) {
        if (o->progress) o->progress(shown > 0.95 ? shown : 0.95, "Finishing...");
        char rsp[1100], log[1100];
        snprintf(rsp, sizeof rsp, "%s" SEP "link.rsp", work);
        snprintf(log, sizeof log, "%s" SEP "link.log", work);
        FILE *f = fopen(rsp, "wb");
        if (f) {
            for (int k = 0; k < units.n; k++) {  // (forward slashes: the response file's backslashes are escapes)
                fputc('"', f);
                for (const char *c = units.u[k].obj; *c; c++) fputc(*c == '\\' ? '/' : *c, f);
                fputs("\"\n", f);
            }
            fclose(f);
        }
        Args a = {0};
        char at[1200];
        snprintf(at, sizeof at, "@%s", rsp);
#ifdef _WIN32
        if (t.zig_rt[0]) {  // the linker itself, with the runtime built ahead of time (its flags and libraries: link.rsp)
            char opt[1200];
            add(&a, t.cc);
            add(&a, "lld-link");
            snprintf(opt, sizeof opt, "@%s" SEP "link.rsp", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-LIBPATH:%s", t.zig_rt);
            add(&a, opt);
            snprintf(opt, sizeof opt, "-OUT:%s", tmp);
            add(&a, opt);
            add(&a, at);
            add(&a, t.implib);
        } else
#endif
        {
            compiler(&a, &t);
            add_split(&a, GAMELIB_SHARED);
            add(&a, "-o");
            add(&a, tmp);
            add(&a, at);
#ifdef _WIN32
            add(&a, t.implib);
#endif
        }
        PlatProc p = f ? spawn(a.v, log) : 0;
        args_free(&a);
        int r = p ? -1 : 255;
        while (p && (r = finished(p)) < 0) {
            if (o->cancelled && o->cancelled()) { stop(p); r = 130; break; }
            sleep_ms(50);
        }
        if (r == 130) {
            LOG("[gamebuild] linking cancelled\n");
            snprintf(why, why_size, "Compilation cancelled. Completed files are kept for Resume.");
            ok = false;
        } else if (!p) {
            LOG("[gamebuild] linking failed: the linker didn't start\n");
            snprintf(why, why_size, "the game code didn't link");
            ok = false;
        } else if (r != 0) {
            snprintf(why, why_size, "the game code didn't link");
            helper_failed("linking", r, log, why, why_size);
            ok = false;
        }
    }
    if (ok) ok = build_cache_publish(cache, GAMELIB_NAME, g_full_id, why, why_size);
    bool cancelled = !ok && o->cancelled && o->cancelled();
    if (!ok && !cancelled) build_cache_failed(cache);
    free(units.u);
    build_cache_close(cache);
    LOG("[gamebuild] reused %d objects, compiled %d objects\n", reused, compiled);
    LOG("[gamebuild] %s in %.0fs\n", ok ? "done" : cancelled ? "cancelled" : "failed", (double)(plat_now_ns() - t0) / 1e9);
    if (ok && o->progress) o->progress(1, "Finishing...");
    return ok;
}
