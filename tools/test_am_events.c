// The service and HIPC codec are production code; kernel waits are inserted by the runner.
#include <assert.h>
#include "../runtime/hle.h"
#include "../runtime/ipc.h"
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/ipc.c"
#undef SERVICE
#define SERVICE(name, fn) static void fn(Service *s, IpcReq *req, IpcResp *resp)
#include "../runtime/services/am.c"

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static KObject *test_handles[64];
static KEvent events[16];
static unsigned nevents, next_handle = 1;
static KThread current, *t_cur = &current, *g_syncwaiters;
volatile int g_rt_unwind;
uint32_t kernel_new_handle(KObject *object) {
    assert(next_handle < 64); test_handles[next_handle] = object; return 0x100 + next_handle++;
}
KObject *handle_get(uint32_t handle) {
    return handle > 0x100 && handle < 0x140 ? test_handles[handle - 0x100] : NULL;
}
KEvent *kernel_create_event_pair(uint32_t *write, uint32_t *read) {
    assert(nevents < 16); KEvent *event = &events[nevents++];
    *event = (KEvent){.type = KOBJ_EVENT};
    *write = kernel_new_handle(event); *read = kernel_new_handle(event); return event;
}
void kernel_signal(KObject *event) { event->signaled = true; }
const uint8_t g_user_id[16];
const char *settings_language(void) { return "en-US"; }
void plat_exit(int code) { (void)code; abort(); }
void rt_fatal_msg(const char *fmt, ...) { (void)fmt; abort(); }
void rt_fatal_trace(void) { abort(); }
void plat_print_backtrace(void) { abort(); }
uint32_t vi_vsync_handle(void) { return 0; }
static void deadline(struct timespec *ts, int64_t ns) { (void)ts; (void)ns; abort(); }
static bool kwait(int64_t ns, const struct timespec *ts) { (void)ns; (void)ts; abort(); }
/* production wait */

static _Alignas(16) uint32_t message[64];
static Cpu cpu;
static uint32_t event_request(uint32_t session, uint32_t command, bool domain) {
    memset(message, 0, sizeof message); message[0] = 4; message[1] = domain ? 12 : 8;
    unsigned cmif = domain ? 8 : 4;
    if (domain) { message[4] = 1 | (16u << 16); message[5] = 1; }
    memcpy(message + cmif, "SFCI", 4); message[cmif + 2] = command;
    assert(!ipc_handle_request(&cpu, session));
    assert((message[1] & 0x80000000u) && message[2] == 2); // one copy handle
    assert(!memcmp(message + cmif, "SFCO", 4) && !message[cmif + 2]);
    uint32_t handle = message[3]; KObject *event = handle_get(handle);
    assert(event && event->type == KOBJ_EVENT && !event->signaled);
    int32_t index = -1;
    assert(svc_wait_synchronization((uintptr_t)&handle, 1, 0, &index) == R_TIMEOUT && index == -1);
    event->signaled = true;
    assert(svc_wait_synchronization((uintptr_t)&handle, 1, 0, &index) == R_OK && index == 0);
    event->signaled = false;
    return handle;
}
int main(void) {
    cpu.tpidrro = (uintptr_t)message;
    uint32_t app = session_new(service_new("IApplicationFunctions", application_functions, NULL));
    uint32_t first = event_request(app, 210, false), second = event_request(app, 210, false);
    assert(first != second);
    test_handles[first - 0x100] = NULL; // one client closing its handle does not invalidate another
    int32_t index = -1;
    assert(svc_wait_synchronization((uintptr_t)&second, 1, 0, &index) == R_TIMEOUT);
    event_request(app, 130, false); event_request(app, 140, false); // existing notification events
    memset(message, 0, sizeof message); message[0] = 5; message[1] = 8; memcpy(message + 4, "SFCI", 4);
    assert(!ipc_handle_request(&cpu, app) && message[8] == 1);
    event_request(app, 210, true);
    uint32_t self = session_new(service_new("ISelfController", self_controller, NULL));
    event_request(self, 91, false);
    IpcReq request = {.cmd = 90}; IpcResp response = {0}; self_controller(NULL, &request, &response);
    assert(!response.result && response.data_size == 8 && !*(uint64_t *)response.data);
    am_reset(); event_request(app, 210, true); event_request(self, 91, false);
    puts("PASS: applet event handles, nonblocking kernel waits, CMIF/domain replies and reset");
}
