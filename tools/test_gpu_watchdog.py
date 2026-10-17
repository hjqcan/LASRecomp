"""Exercise the real watchdog against blocked calls without loading a GPU driver."""
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
    raise SystemExit('Vulkan watchdog test requires Windows or Linux')
host = (root / f'runtime/platform/{platform}.c').read_text(encoding='utf-8')
terminate = re.search(r'void plat_terminate\(int status\) \{[^}]+\}', host).group()
device = (root / 'runtime/nvn/vulkan/device.c').read_text(encoding='utf-8')
idle = device[device.index('static VkResult wait_device_idle('):device.index('\nstatic void recreate_swapchain(')]
shutdown = device[device.index('static void quick_exit_handler('):device.index('\nvoid ui_window_init(')]
debug = device[device.index('static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb('):device.index('\n// LARECOMP_VK_VALIDATION=')]
harness = r'''
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include "nvn/vulkan/gpu_watchdog.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
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
        struct timespec delay = { .tv_nsec = 1000000 }; nanosleep(&delay, NULL);
#endif
    }
}
static void delay(unsigned ms) { plat_sleep_until_ns(plat_now_ns() + (uint64_t)ms * 1000000); }
typedef int VkResult;
enum { VK_SUCCESS = 0, VK_ERROR_DEVICE_LOST = -4 };
static struct { int dev; } g_vk;
static int scenario, exit_status;
int plat_exit_status(void) { return exit_status; }
static VkResult vkDeviceWaitIdle(int device) {
    (void)device;
    if (scenario == 6) delay(3000);
    return scenario == 5 ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}
static void vk_pipeline_cache_save(void) { puts("pipeline cache saved"); }
#define LOG(...) fprintf(stderr, __VA_ARGS__)
#define VKAPI_ATTR
#define VKAPI_CALL
typedef unsigned VkBool32, VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT;
enum { VK_FALSE = 0, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT = 0x100,
       VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT = 0x1000 };
typedef struct { const char *pMessage; } VkDebugUtilsMessengerCallbackDataEXT;
''' + terminate + '\n' + idle + '\n' + shutdown + '\n' + debug + r'''
static void cleanup(void) { puts("GPU cleanup entered"); fflush(stdout); }
static void *quick_calls(void *arg) {
    (void)arg;
    for (unsigned i = 0; i < 2000; i++) {
        unsigned guard = vk_watchdog_begin("completed call", i);
        vk_watchdog_end(guard);
    }
    return NULL;
}
static void *blocked_call(void *arg) {
    (void)arg;
    unsigned guard = vk_watchdog_begin("blocked worker", 89);
    delay(3000);
    vk_watchdog_end(guard);
    puts("blocked call returned");
    return NULL;
}
int main(int argc, char **argv) {
    if (argc != 2) return 99;
    scenario = atoi(argv[1]);
    atexit(cleanup);
    if (scenario == 0) {
        // Reusing slots from many different threads must not exhaust the table.
        for (unsigned i = 0; i < 100; i++) {
            pthread_t t; if (pthread_create(&t, NULL, quick_calls, NULL)) return 98;
            pthread_join(t, NULL);
        }
        delay(500);  // all completed guards stay cancelled past the timeout
    } else if (scenario == 1) {
        unsigned guard = vk_watchdog_begin("blocked main", 41);
        delay(3000);
        vk_watchdog_end(guard);
    } else if (scenario == 2) {
        pthread_t threads[9];
        if (pthread_create(&threads[0], NULL, blocked_call, NULL)) return 98;
        for (unsigned i = 1; i < 9; i++) if (pthread_create(&threads[i], NULL, quick_calls, NULL)) return 98;
        for (unsigned i = 0; i < 9; i++) pthread_join(threads[i], NULL);
    } else if (scenario == 3) {
        unsigned outer = vk_watchdog_begin("outer call", 75);
        unsigned inner = vk_watchdog_begin("inner call", 76);
        vk_watchdog_end(inner);
        delay(3000);
        vk_watchdog_end(outer);
    } else if (scenario >= 4 && scenario <= 7) {
        g_vk.dev = 1;
        exit_status = scenario == 7 ? 7 : 0;
        atexit(quick_exit_handler);
        return 0;
    } else if (scenario >= 8 && scenario <= 11) {
        VkDebugUtilsMessengerCallbackDataEXT message = { .pMessage = "validation warning" };
        if (scenario == 11) for (unsigned i = 0; i < 501; i++) debug_cb(0x100, 0, &message, NULL);
        message.pMessage = scenario == 8 ? "validation warning" : "validation error";
        debug_cb(scenario == 8 ? 0x100 : 0x1000, 0, &message, NULL);
    } else return 99;
    puts("completed normally");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='gpu-watchdog-') as temp:
    temp = Path(temp)
    cfile = temp / 'watch.c'
    cfile.write_text(harness, encoding='utf-8')
    exe = temp / ('watch.exe' if os.name == 'nt' else 'watch')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + ['-std=gnu11', '-O2', '-pthread', '-Iruntime',
                   '-DGPU_WATCHDOG_TIMEOUT_NS=200000000ull', '-DGPU_WATCHDOG_POLL_NS=10000000ull',
                   str(cfile), 'runtime/nvn/vulkan/gpu_watchdog.c', '-o', str(exe)], cwd=root, check=True)
    for scenario, expected in enumerate(('', 'blocked main (submission 41)', 'blocked worker (submission 89)', 'outer call (submission 75)')):
        run = subprocess.run([str(exe), str(scenario)], capture_output=True, text=True, timeout=8)
        if scenario == 0:
            assert run.returncode == 0 and 'completed normally' in run.stdout, run
            assert not run.stderr, run
        else:
            assert run.returncode == 1 and expected in run.stderr, run
            assert not run.stdout, run  # no caller continuation or atexit cleanup
    for scenario in (4, 5, 6, 7):
        run = subprocess.run([str(exe), str(scenario)], capture_output=True, text=True, timeout=8)
        assert run.returncode == {4: 0, 5: 1, 6: 1, 7: 7}[scenario], run
        if scenario in (4, 7):
            assert run.stdout.strip() == 'pipeline cache saved', run
        else:
            assert not run.stdout, run
        if scenario == 5:
            assert 'GPU idle wait failed' in run.stderr, run
        if scenario == 6:
            assert 'vkDeviceWaitIdle (exit)' in run.stderr, run
    for scenario in (8, 9, 10, 11):
        env = dict(os.environ)
        env.pop('LARECOMP_VK_VALIDATION_STOP', None)
        if scenario != 9:
            env['LARECOMP_VK_VALIDATION_STOP'] = '1'
        run = subprocess.run([str(exe), str(scenario)], env=env, capture_output=True, text=True, timeout=8)
        if scenario in (8, 9):
            assert run.returncode == 0 and 'completed normally' in run.stdout, run
        else:
            assert run.returncode == 1 and not run.stdout, run
            assert 'validation error' in run.stderr and 'stopping at the first validation error' in run.stderr, run
print('PASS: guard concurrency/cancellation, blocked calls, real shutdown paths, and opt-in validation fail-stop')
