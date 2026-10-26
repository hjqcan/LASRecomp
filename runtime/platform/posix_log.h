// macOS / Linux session log (included by macos.c and linux.c): lasrecomp.log in the data folder is always the latest
// run; a new start moves the previous one aside as lasrecomp-backup-<YYYYMMDD-HHMMSS>-<pid>.log (its last write, the
// pid from its first line, 0 if unknown) and keeps the five newest of those, as on Windows. Moving instead of
// truncating also means a copy still running or quitting keeps writing to its own (moved) file. Processes started by
// this one (the start screen re-executing itself for the game) get LASRECOMP_LOG_APPEND and append to the same log.
#pragma once
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SESSION_LOG_BACKUPS 5

static void session_log_backup(const char *dir, const char *name) {
    char from[1100], to[1200];
    snprintf(from, sizeof from, "%s/%s", dir, name);
    struct stat st;
    if (lstat(from, &st) != 0 || !S_ISREG(st.st_mode)) return;
    int pid = 0;
    FILE *f = fopen(from, "r");
    if (f) {
        char line[128];
        if (!fgets(line, sizeof line, f) || sscanf(line, "[log] %*[^,], pid %d", &pid) != 1 || pid < 0) pid = 0;
        fclose(f);
    }
    char when[32];
    strftime(when, sizeof when, "%Y%m%d-%H%M%S", localtime(&st.st_mtime));
    snprintf(to, sizeof to, "%s/lasrecomp-backup-%s-%d.log", dir, when, pid);
    for (int i = 1; access(to, F_OK) == 0 && i < 100; i++)  // (never replace another backup)
        snprintf(to, sizeof to, "%s/lasrecomp-backup-%s-%d-%d.log", dir, when, pid, i);
    rename(from, to);
}

typedef struct { char name[256]; time_t mtime; } SessionLogBackup;
static int session_log_newest(const void *a, const void *b) {
    const SessionLogBackup *x = a, *y = b;
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    return strcmp(y->name, x->name);
}

static void session_log_prune(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    SessionLogBackup *logs = NULL;
    size_t n = 0, cap = 0;
    char path[1400];
    for (struct dirent *e; (e = readdir(d));) {
        size_t len = strlen(e->d_name);
        if (strncmp(e->d_name, "lasrecomp-backup-", 17) || len < 22 || len >= sizeof logs->name ||
            strcmp(e->d_name + len - 4, ".log")) continue;
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (n == cap) {
            SessionLogBackup *more = realloc(logs, (cap = cap ? cap * 2 : 16) * sizeof *logs);
            if (!more) break;
            logs = more;
        }
        snprintf(logs[n].name, sizeof logs[n].name, "%s", e->d_name);
        logs[n++].mtime = st.st_mtime;
    }
    closedir(d);
    if (n > 1) qsort(logs, n, sizeof *logs, session_log_newest);
    for (size_t i = SESSION_LOG_BACKUPS; i < n; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, logs[i].name);
        unlink(path);
    }
    free(logs);
}

// stdout / stderr -> <dir>/lasrecomp.log (path_out: its path, emptied on failure).
static void session_log_open(const char *dir, char *path_out, size_t n) {
    snprintf(path_out, n, "%s/lasrecomp.log", dir);
    bool append = getenv("LASRECOMP_LOG_APPEND") != NULL;
    if (!append) {
        session_log_backup(dir, "lasrecomp.prev.log");  // (the earlier macOS scheme's one backup)
        session_log_backup(dir, "lasrecomp.log");
        session_log_prune(dir);
    }
    if (!freopen(path_out, "a", stderr)) { path_out[0] = 0; return; }
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    setenv("LASRECOMP_LOG_APPEND", "1", 1);
    // (a first line: an empty log then means writing failed - e.g. a full disk - not another start; its pid names
    // the backup later)
    time_t now = time(NULL);
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(stderr, "[log] %s, pid %d%s\n", when, (int)getpid(), append ? " (restarted)" : "");
}

// The open-file limit: a macOS app starts with 256 (Linux usually 1024). Raised toward the hard limit (at most
// 10240, macOS's OPEN_MAX) so a long session never runs out - running out fails saves ("could not be saved") and
// game file reads. Called first thing (plat_init), after the log is set up.
#include <errno.h>
#include <sys/resource.h>
static void raise_open_file_limit(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl)) {
        fprintf(stderr, "[fd] open-file limit unknown (%s)\n", strerror(errno));
        return;
    }
    rlim_t before = rl.rlim_cur, want = 10240;
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < want) want = rl.rlim_max;
    if (before != RLIM_INFINITY && before < want) {
        rl.rlim_cur = want;
        if (setrlimit(RLIMIT_NOFILE, &rl)) {
            fprintf(stderr, "[fd] open-file limit %llu, could not raise it to %llu (%s)\n", (unsigned long long)before,
                    (unsigned long long)want, strerror(errno));
            return;
        }
    }
    getrlimit(RLIMIT_NOFILE, &rl);
    char hard[32] = "unlimited";
    if (rl.rlim_max != RLIM_INFINITY) snprintf(hard, sizeof hard, "%llu", (unsigned long long)rl.rlim_max);
    fprintf(stderr, "[fd] open-file limit %llu (was %llu, hard limit %s)\n", (unsigned long long)rl.rlim_cur,
            (unsigned long long)before, hard);
}
