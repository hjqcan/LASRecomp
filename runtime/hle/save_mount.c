// The game's async save worker and startup status query both use the name
// "save". On host threads they can overlap: one MountSaveData returns 0x7802
// (already mounted), its caller still Unmounts, and the first worker then aborts
// with 0x35f202 (not mounted). Hold a lease from a successful mount through its
// matching unmount, so each save transaction retains its own SDK mount.
//
// These wrappers call the original SDK bodies. Do not add their symbols to
// hle_symbols.txt: the generator must retain those bodies in the game library.
#include <pthread.h>
#include <string.h>
#include "../hle.h"

static pthread_mutex_t save_mount_lock = PTHREAD_MUTEX_INITIALIZER;
static Cpu *save_mount_owner;
static unsigned save_mount_depth;

static void save_mount_acquire(Cpu *c) {
    for (;;) {
        // A guest reboot must also be able to stop a thread waiting here. Never
        // unwind while holding a host mutex, or keep that mutex across SDK code.
        rt_unwind_point();
        pthread_mutex_lock(&save_mount_lock);
        if (!save_mount_owner || save_mount_owner == c) {
            save_mount_owner = c;
            ++save_mount_depth;
            pthread_mutex_unlock(&save_mount_lock);
            return;
        }
        pthread_mutex_unlock(&save_mount_lock);
        plat_sleep_until_ns(plat_now_ns() + 1000000);
    }
}

static void save_mount_release(Cpu *c) {
    pthread_mutex_lock(&save_mount_lock);
    if (save_mount_owner == c && save_mount_depth && !--save_mount_depth)
        save_mount_owner = NULL;
    pthread_mutex_unlock(&save_mount_lock);
}

HLE_FUNC("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", mount_save_serialized) {
    bool guarded = c->x[0] && !strcmp((const char *)(uintptr_t)c->x[0], "save");
    if (guarded) save_mount_acquire(c);
    dispatch_lookup(guest_sym("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE"))(c);
    if (guarded && c->x[0]) save_mount_release(c);  // failed mount owns no lease
}

HLE_FUNC("_ZN2nn2fs7UnmountEPKc", unmount_save_serialized) {
    bool guarded = c->x[0] && !strcmp((const char *)(uintptr_t)c->x[0], "save");
    dispatch_lookup(guest_sym("_ZN2nn2fs7UnmountEPKc"))(c);
    if (guarded) save_mount_release(c);
}

RT_ON_RESET(save_mount_reset) {
    // All guest threads have stopped before reset hooks run.
    pthread_mutex_lock(&save_mount_lock);
    save_mount_owner = NULL;
    save_mount_depth = 0;
    pthread_mutex_unlock(&save_mount_lock);
}
