#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// One store beside each save mount: save/save_0.backups (seeds therefore stay separate).
// All operations serialize; callers must keep the source quiescent while creating/restoring.
// Automatic snapshots happen on mount and after the game's Commit, never after individual writes.
#define SAVE_BACKUP_KEEP 20
typedef struct {
    char id[64], reason[64], version[32];
    unsigned files;
    uint64_t bytes;
} SaveBackupInfo;
bool save_backup_create(const char *root, const char *reason, const char *version, bool deduplicate,
                        char *why, size_t cap);
int save_backup_list(const char *root, SaveBackupInfo *out, int capacity, char *why, size_t cap);
bool save_backup_restore(const char *root, const char *id, const char *version, char *why, size_t cap);
#ifdef __cplusplus
}
#endif
