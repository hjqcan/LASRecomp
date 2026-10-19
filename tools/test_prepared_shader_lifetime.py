"""Exercise production prepared-job ownership across aliases, failures and reboot."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'runtime/nvn/vulkan/exec.c').read_text(encoding='utf8')


def function(signature):
    start = source.index(signature)
    end = source.index('\n}\n', start) + 3
    return source[start:end]


harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "nvn/vulkan/shader_jobs.h"
typedef uintptr_t VkPipeline;
typedef struct { unsigned marker; } ShaderInfo;
typedef struct {
    uintptr_t layout, set_layout[2]; void *snapshot[2]; ShaderInfo *vi, *fi;
    bool prepared; ShaderJob job;
} Prog;
typedef struct { VkPipeline pipe; bool prepared; ShaderJob job; } Pipe;
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; } CsProg;
static struct { int dev; } g_vk;
static unsigned destroyed[128], freed_shaders, cache_resets;
static _Atomic unsigned next_handle;
static uintptr_t handle(void) { unsigned h = atomic_fetch_add(&next_handle, 1) + 1; assert(h < 128); return h; }
static void destroy(uintptr_t h) { assert(h && h < 128 && destroyed[h]++ == 0); }
static void vkDestroyPipeline(int d, uintptr_t h, void *a) { destroy(h); }
static void vkDestroyPipelineLayout(int d, uintptr_t h, void *a) { destroy(h); }
static void vkDestroyDescriptorSetLayout(int d, uintptr_t h, void *a) { destroy(h); }
static void maxwell_free(ShaderInfo *s) { if (s) { assert(s->marker == 42); freed_shaders++; free(s); } }
static void vk_shader_cache_reset(void) { cache_resets++; }
static bool synchronous;
static bool sync_shaders(void) { return synchronous; }
static bool shader_debug(void) { return false; }
static int SDL_GetNumLogicalCPUCores(void) { return 6; }
static int plat_cpu_count(void) { return 6; }
static struct { Prog *prog; } prog_tab[4];
static size_t prog_cap = 4, prog_count;
static struct { Pipe *pipe; } pipe_tab[4];
static size_t pipe_cap = 4, pipe_count;
static struct { CsProg *cp; } g_cs_tab[4];
static int g_cs_n;
'''
harness += source[source.index('static pthread_mutex_t prep_lock'):source.index('static uint64_t prog_content_key')]
harness += function('static void start_build(')
harness += function('static void destroy_program(')
harness += function('static void reset_compute_programs(void) {')
harness += function('static void reset_prepared_objects(void) {')
harness += source[source.index('typedef struct { void (*fn)(void *); void *arg; } PrepJob;'):
                  source.index('bool renderer_prepare_begin(void)')]
reset = function('void vk_exec_reset(void) {')
harness += reset[:reset.index('    ss_forget();')] + '}\n'
harness += r'''
static void init_program(Prog *p, bool prepared) {
    p->prepared = prepared; p->layout = handle(); p->set_layout[0] = handle();
    p->vi = malloc(sizeof *p->vi); p->vi->marker = 42;
}
static void compile(void *arg) { Prog *p = arg; p->set_layout[1] = handle(); }
typedef struct { Pipe *pipe; Prog *program; bool fail; } PipelineInput;
static void pipeline(void *arg) {
    PipelineInput *in = arg;
    shader_job_wait(&in->program->job);
    if (!in->fail) in->pipe->pipe = handle();
    free(in);
}
static void cycle(bool sync) {
    synchronous = sync;
    unsigned before = freed_shaders;
    Prog *prepared = calloc(1, sizeof *prepared), *ordinary = calloc(1, sizeof *ordinary);
    init_program(prepared, true); init_program(ordinary, false);
    prep_own(prepared, PREP_PROGRAM); PREP_PUT(prep_progs, 1, prepared);
    prog_tab[0].prog = prog_tab[1].prog = prepared; prog_tab[2].prog = ordinary; prog_count = 3;
    CsProg *cs = calloc(1, sizeof *cs), *normal_cs = calloc(1, sizeof *normal_cs);
    init_program(&cs->p, true); init_program(&normal_cs->p, false);
    cs->pipe = handle(); normal_cs->pipe = handle(); prep_own(cs, PREP_COMPUTE);
    PREP_PUT(prep_cs, 2, cs); g_cs_tab[0].cp = g_cs_tab[1].cp = cs; g_cs_tab[2].cp = normal_cs; g_cs_n = 3;
    prep_job(&prepared->job, compile, prepared);
    for (unsigned i = 0; i < 2; i++) {
        Pipe *p = calloc(1, sizeof *p); p->prepared = true; prep_own(p, PREP_PIPELINE);
        PipelineInput *in = malloc(sizeof *in); *in = (PipelineInput){p, prepared, i != 0};
        // The second represents a failed or duplicate prepared variant, absent from lookup tables.
        if (!i) { PREP_PUT(prep_pipes, 3, p); pipe_tab[0].pipe = pipe_tab[1].pipe = p; }
        prep_job(&p->job, pipeline, in);
    }
    Pipe *normal = calloc(1, sizeof *normal); normal->pipe = handle(); pipe_tab[2].pipe = normal; pipe_count = 3;
    // Reset while jobs may still be queued/running: every owner outlives its callback.
    vk_exec_reset();
    assert(freed_shaders == before + 4 && !prep_owned && !prep_pending);
    assert(!prog_count && !pipe_count && !g_cs_n);
    assert(!prep_progs && !prep_pipes && !prep_cs && !PREP_GET(prep_progs, 1));
    for (unsigned i = 1; i <= next_handle; i++) assert(destroyed[i] == 1);
    vk_exec_reset(); // empty reboot is also valid
}
int main(void) { cycle(false); cycle(true); assert(cache_resets == 4); return 0; }
'''
with tempfile.TemporaryDirectory(prefix='prepared-lifetime-') as directory:
    folder = Path(directory)
    code = folder / 'lifetime.c'
    exe = folder / ('lifetime.exe' if os.name == 'nt' else 'lifetime')
    code.write_text(harness, encoding='utf8')
    subprocess.run([*shlex.split(os.environ.get('CC', 'clang')), '-std=gnu11', '-O2', '-pthread',
                    '-I' + str(ROOT / 'runtime'), str(code),
                    str(ROOT / 'runtime/nvn/vulkan/shader_jobs.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
print('PASS: prepared aliases, failed variants, pending jobs, synchronous mode and repeated cache reset')
