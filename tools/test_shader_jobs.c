#include "../runtime/nvn/vulkan/shader_jobs.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int started, open, completed;
} Gate;
static Gate gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER};

static void gated(void *unused) {
    (void)unused;
    pthread_mutex_lock(&gate.lock);
    gate.started++;
    pthread_cond_broadcast(&gate.changed);
    while (!gate.open) pthread_cond_wait(&gate.changed, &gate.lock);
    gate.completed++;
    pthread_mutex_unlock(&gate.lock);
}
static void await_started(int count) {
    pthread_mutex_lock(&gate.lock);
    while (gate.started < count) pthread_cond_wait(&gate.changed, &gate.lock);
    pthread_mutex_unlock(&gate.lock);
}
static void open_gate(void) {
    pthread_mutex_lock(&gate.lock);
    gate.open = 1;
    pthread_cond_broadcast(&gate.changed);
    pthread_mutex_unlock(&gate.lock);
}
static void count(void *value) { atomic_fetch_add((_Atomic int *)value, 1); }
static void *join_job(void *job) { shader_job_wait(job); return NULL; }

int main(void) {
    shader_jobs_init(2);
    ShaderJob occupied[2] = {0}, required = {0};
    _Atomic int calls = 0;
    for (int i = 0; i < 2; i++) shader_job_submit(&occupied[i], gated, NULL);
    await_started(2);
    // A first-use dependency must not sit behind unrelated prewarming work.
    shader_job_submit(&required, count, &calls);
    shader_job_wait(&required);
    assert(calls == 1 && gate.completed == 0);
    open_gate();
    shader_jobs_drain();
    assert(gate.completed == 2);

    for (int round = 0; round < 100; round++) {
        gate.started = gate.open = gate.completed = 0;
        ShaderJob one = {0};
        shader_job_submit(&one, gated, NULL);
        pthread_t a, b;
        pthread_create(&a, NULL, join_job, &one);
        pthread_create(&b, NULL, join_job, &one);
        await_started(1);
        open_gate();
        pthread_join(a, NULL); pthread_join(b, NULL);
        assert(gate.completed == 1 && atomic_load(&one.state) == SHADER_JOB_DONE);
        shader_jobs_drain();
    }
    // Reset waits for every queued/running task before releasing its inputs.
    ShaderJob *many = calloc(1024, sizeof *many);
    calls = 0;
    for (int i = 0; i < 1024; i++) shader_job_submit(&many[i], count, &calls);
    shader_jobs_drain();
    assert(calls == 1024);
    for (int i = 0; i < 1024; i++) assert(atomic_load(&many[i].state) == SHADER_JOB_DONE);
    free(many);
    puts("PASS: required work bypasses backlog, multiple dependents execute once, all reset jobs finish");
}
