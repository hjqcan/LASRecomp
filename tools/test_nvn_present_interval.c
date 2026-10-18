#include <assert.h>
#include <limits.h>
#include "../runtime/hle.h"
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#include "../runtime/nvn/objects.c"

static bool las, speedfix;
static uint64_t now = 1000000000, render_cost, oversleep, presented[1024];
static unsigned presents, sleeps;
static NvnTexture texture;
bool title_is_las(void) { return las; }
bool acnh60_active(void) { return speedfix; }
uint64_t plat_now_ns(void) { return now; }
void plat_sleep_until_ns(uint64_t deadline) {
    assert(deadline >= now && deadline - now <= 66666667);
    ++sleeps;
    now = deadline + oversleep;
}
void backend_present(NvnTexture *t) {
    assert(t == &texture && presents < 1024);
    presented[presents++] = now;
    now += render_cost;
}
uint64_t renderer_present_count(void) { return presents; }
void photo_mode_presented(uint64_t frame) { assert(frame == presents); }
bool photo_mode_hold(void) { return false; }
int rt_reboot_requested(void) { return 0; }
void backend_represent(void) { abort(); }
void rt_fatal_msg(const char *fmt, ...) { (void)fmt; abort(); }
void rt_fatal_trace(void) { abort(); }

static void present(NvnWindow *w, int index) {
    uint64_t handle = (uintptr_t)w;
    Cpu c = {0}; c.x[1] = (uintptr_t)&handle; c.x[2] = index;
    hle_nvnQueuePresentTexture(&c);
}

int main(void) {
    uint64_t texture_handle = (uintptr_t)&texture;
    NvnWindow w = {.textures={(uintptr_t)&texture_handle}, .num=1, .active=1, .interval=2};
    render_cost = 9000000;
    present(&w, 0); assert(sleeps == 0);
    for (unsigned i=1; i<301; ++i) {
        now += 6000000; // CPU work plus the previous render must fit inside the period.
        present(&w, 0);
        assert(presented[i] - presented[i-1] == 33333334);
    }
    assert(presented[300] - presented[0] == 10000000200ull);

    speedfix=true;
    for (unsigned i=0; i<60; ++i) {
        uint64_t prior=presented[presents-1];
        present(&w,0);
        assert(presented[presents-1]-prior==16666667);
    }
    speedfix=false;
    uint64_t restored=presented[presents-1];present(&w,0);
    assert(presented[presents-1]-restored==33333334);

    unsigned before = sleeps;
    now += 250000000; present(&w, 0); assert(sleeps == before);
    uint64_t late = presented[presents-1];
    present(&w, 0); assert(presented[presents-1] - late == 33333334);
    oversleep = 2000000; present(&w, 0);
    uint64_t actual = presented[presents-1]; oversleep = 0;
    present(&w, 0); assert(presented[presents-1] - actual == 33333334);

    uint64_t handle = (uintptr_t)&w;
    Cpu c = {0}; c.x[0]=(uintptr_t)&handle; c.x[1]=1;
    hle_nvnWindowSetPresentInterval(&c);
    actual=presented[presents-1]; present(&w,0);
    assert(presented[presents-1]-actual==16666667);
    actual=presented[presents-1]; present(&w,0);
    assert(presented[presents-1]-actual==16666667);

    NvnWindow second = {.textures={(uintptr_t)&texture_handle}, .num=1, .active=1, .interval=4};
    before=sleeps; present(&second,0); assert(sleeps==before);
    actual=presented[presents-1]; present(&second,0);
    assert(presented[presents-1]-actual==66666667);

    for (int i=0; i<3; ++i) {
        w.interval = (int[]){0,-1,INT_MAX}[i]; before=sleeps;
        present(&w,0); present(&w,0); assert(sleeps==before);
    }
    w.interval=2; las=true; before=sleeps;
    present(&w,0); present(&w,0); assert(sleeps==before && !w.present_time_ns);
    las=false; present(&w,0);
    now=1; before=sleeps; present(&w,0); assert(sleeps==before);
    now=UINT64_MAX-10; render_cost=0; w.present_time_ns=now;
    present(&w,0); assert(sleeps==before);
    before=presents; present(&w,-1); present(&w,1); assert(presents==before);
    puts("PASS: NVN presentation intervals, render time, stalls, wake jitter, changes, separate windows and legacy pacing");
}
