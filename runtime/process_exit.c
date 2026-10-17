#include "platform.h"
#include <stdatomic.h>
#include <stdlib.h>

// atexit callbacks do not receive exit()'s status. The Vulkan shutdown callback
// must carry it into immediate OS termination instead of converting failures to 0.
static atomic_int requested_status;
int plat_exit_status(void) { return atomic_load(&requested_status); }
void plat_exit(int status) {
    atomic_store(&requested_status, status);
    exit(status);
}
