"""Exercise production NVN fence waits with a mocked driver and the real watchdog."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
platform = 'windows' if os.name == 'nt' else 'linux' if sys.platform.startswith('linux') else None
if platform is None:
    raise SystemExit('Vulkan fence test requires Windows or Linux')
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
counter = source[source.index('uint64_t vk_completed_seq(void) {'):source.index('\n// (LARECOMP_VK_PROF)')]
wait = source[source.index('int renderer_wait_seq_timeout('):source.index('\nVkDescriptorSet vk_alloc_set(')]
host = (root / f'runtime/platform/{platform}.c').read_text()
terminate = re.search(r'void plat_terminate\(int status\) \{[^}]+\}', host).group()
harness = r'''
#include "platform.h"
#include "nvn/vulkan/gpu_watchdog.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
uint64_t plat_now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency, now;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&now);
    return (uint64_t)((unsigned __int128)now.QuadPart * 1000000000ull / frequency.QuadPart);
#else
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
#endif
}
void plat_sleep_until_ns(uint64_t until) {
    while (plat_now_ns() < until) {
#ifdef _WIN32
        Sleep(1);
#else
        struct timespec delay = {.tv_nsec=1000000}; nanosleep(&delay,NULL);
#endif
    }
}
typedef int VkResult;
enum { VK_SUCCESS=0, VK_TIMEOUT=2, VK_ERROR_DEVICE_LOST=-4, VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO=1 };
typedef struct { int sType; unsigned semaphoreCount; uint64_t *pSemaphores; const uint64_t *pValues; } VkSemaphoreWaitInfo;
static struct { int dev; uint64_t timeline; } g_vk;
static int scenario, queries, waits;
static uint64_t resolved, waited_timeout;
static void rt_fatal_msg(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap);
}
#define FATAL(...) do { rt_fatal_msg(__VA_ARGS__); exit(7); } while (0)
#define VK_CHECK(r) do { if ((r)!=VK_SUCCESS) FATAL("counter query failed\n"); } while (0)
static void cq_resolve_done(uint64_t seq) { resolved=seq; }
static VkResult vkGetSemaphoreCounterValue(int dev, uint64_t semaphore, uint64_t *value) {
    (void)dev; (void)semaphore; ++queries;
    if (scenario==6) plat_sleep_until_ns(plat_now_ns()+3000000000ull);
    *value=scenario==1 ? 7 : 0;
    return scenario==7 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static VkResult vkWaitSemaphores(int dev, const VkSemaphoreWaitInfo *wi, uint64_t timeout) {
    (void)dev; assert(wi->semaphoreCount==1 && *wi->pValues==7);
    ++waits; waited_timeout=timeout;
    if (scenario==5) plat_sleep_until_ns(plat_now_ns()+3000000000ull);
    return scenario==2 ? VK_SUCCESS : VK_TIMEOUT;
}
''' + terminate + '\n' + counter + '\n' + wait + r'''
static void cleanup(void) { puts("cleanup"); fflush(stdout); }
int main(int argc,char **argv) {
    assert(argc==2); scenario=atoi(argv[1]); atexit(cleanup);
    uint64_t timeout=scenario==4 || scenario==5 ? UINT64_MAX : scenario==8 ? 10000000000ull : 123;
    int result=renderer_wait_seq_timeout(scenario==0 ? 0 : 7,timeout);
    if (scenario==0) assert(result==0 && !queries && !waits && !resolved);
    else if (scenario==1) assert(result==0 && queries==1 && !waits && !resolved);
    else if (scenario==2) assert(result==1 && queries==1 && waits==1 && resolved==7 && waited_timeout==123);
    else if (scenario==3 || scenario==8) assert(result==2 && !resolved && waited_timeout==timeout);
    else abort();
    puts("returned"); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='gpu-fence-') as temp:
    temp = Path(temp)
    cfile = temp / 'fence.c'
    cfile.write_text(harness)
    exe = temp / ('fence.exe' if os.name == 'nt' else 'fence')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + ['-std=gnu11', '-O2', '-pthread', '-Iruntime',
                   '-DGPU_WATCHDOG_TIMEOUT_NS=200000000ull', '-DGPU_WATCHDOG_POLL_NS=10000000ull',
                   str(cfile), 'runtime/nvn/vulkan/gpu_watchdog.c', '-o', str(exe)], cwd=root, check=True)
    for scenario in range(9):
        run = subprocess.run([str(exe), str(scenario)], capture_output=True, text=True, timeout=8)
        if scenario in (4, 5, 6):
            assert run.returncode == 1 and not run.stdout, run
            expected = {4: 'SyncWait submission 7 within 10 s',
                        5: 'vkWaitSemaphores (SyncWait) (submission 7)',
                        6: 'vkGetSemaphoreCounterValue'}[scenario]
            assert expected in run.stderr, run
        elif scenario == 7:
            assert run.returncode == 7 and 'returned' not in run.stdout, run
        else:
            assert run.returncode == 0 and run.stdout.splitlines() == ['returned', 'cleanup'], run
print('PASS: empty/completed fences, finite timeouts, infinite waits, blocked fence/query calls and query errors')
