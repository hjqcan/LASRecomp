"""A stalled GPU must not let callers recycle resources or enter unbounded cleanup."""
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
    raise SystemExit('Vulkan termination test requires Windows or Linux')
source = (root / 'runtime/nvn/vulkan/exec.c').read_text(encoding='utf-8')
begin = source.index('void vk_wait_seq(uint64_t seq) {')
end = source.index('\nuint64_t vk_pending_seq', begin)
wait = source[begin:end]
host = (root / f'runtime/platform/{platform}.c').read_text(encoding='utf-8')
terminate = re.search(r'void plat_terminate\(int status\) \{[^}]+\}', host).group()
harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
typedef int VkResult;
enum { VK_SUCCESS = 0, VK_TIMEOUT = 2, VK_ERROR_DEVICE_LOST = -4, VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO = 1 };
typedef struct { int sType; unsigned semaphoreCount; uint64_t *pSemaphores; const uint64_t *pValues; } VkSemaphoreWaitInfo;
static struct { int dev; uint64_t timeline; } g_vk;
static uint64_t stat_wait_ns, cum_wait, resolved, waited;
static const char *wait_why;
static VkResult result;
static uint64_t SDL_GetTicksNS(void) { static uint64_t now; return ++now; }
static void wait_account(uint64_t ns) { (void)ns; }
static void cq_resolve_done(uint64_t seq) { resolved = seq; }
static VkResult vkWaitSemaphores(int dev, const VkSemaphoreWaitInfo *wi, uint64_t timeout) {
    (void)dev;
    if (wi->semaphoreCount != 1 || timeout != 10000000000ull) abort();
    waited = *wi->pValues;
    return result;
}
static void rt_fatal_msg(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
void vk_devlost_dump(void) { puts("device dump"); }
static void vk_device_fault_report(void) { puts("fault report"); }
#define FATAL(...) do { rt_fatal_msg(__VA_ARGS__); exit(7); } while (0)
#define VK_WATCHDOG(operation, sequence, statement) do { statement; } while (0)
''' + terminate + '\n' + wait + r'''
static void cleanup(void) { puts("GPU cleanup entered"); fflush(stdout); }
int main(int argc, char **argv) {
    if (argc != 2) return 99;
    int scenario = atoi(argv[1]);
    result = scenario == 1 || scenario == 2 ? VK_TIMEOUT : scenario == 3 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
    atexit(cleanup);
    vk_wait_seq(scenario == 2 ? 0 : 7);
    if (scenario == 2 ? (waited || resolved) : (waited != 7 || resolved != 7)) return 98;
    puts("safe to recycle");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='gpu-wait-timeout-') as temp:
    temp = Path(temp)
    cfile = temp / 'wait.c'
    cfile.write_text(harness, encoding='utf-8')
    exe = temp / ('wait.exe' if os.name == 'nt' else 'wait')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + ['-std=c11', str(cfile), '-o', str(exe)], check=True)
    for scenario in (0, 1, 2, 3):
        run = subprocess.run([str(exe), str(scenario)], capture_output=True, text=True, timeout=5)
        if scenario == 1:
            assert run.returncode == 1, run
            assert 'submission 7 within 10 s' in run.stderr, run
            assert not run.stdout, run  # no caller continuation, cleanup, or further driver queries
        elif scenario == 3:
            assert run.returncode == 7 and 'safe to recycle' not in run.stdout, run
            assert 'device dump' in run.stdout and 'fault report' in run.stdout, run
        else:
            assert run.returncode == 0 and 'safe to recycle' in run.stdout, run
print('PASS: completed/empty waits return; timeouts terminate without recycling or GPU cleanup; device loss stays fatal')
