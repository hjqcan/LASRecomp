#include "shader_jobs.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static ShaderJob *head, *tail;
static unsigned pending;
static int started;

static void execute(ShaderJob *job) {
    job->run(job->arg);
    pthread_mutex_lock(&lock);
    atomic_store(&job->state, SHADER_JOB_DONE);
    --pending;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!head) pthread_cond_wait(&changed, &lock);
        ShaderJob *job = head;
        head = job->next;
        if (!head) tail = NULL;
        atomic_store(&job->state, SHADER_JOB_RUNNING);
        pthread_mutex_unlock(&lock);
        execute(job);
    }
    return NULL;
}

void shader_jobs_init(int workers) {
    pthread_mutex_lock(&lock);
    if (!started) {
        if (workers < 1) workers = 1;
        for (int i = 0; i < workers; i++) {
            pthread_t thread;
            if (pthread_create(&thread, NULL, worker, NULL)) abort();
            pthread_detach(thread);
        }
        started = 1;
    }
    pthread_mutex_unlock(&lock);
}

void shader_job_submit(ShaderJob *job, void (*run)(void *), void *arg) {
    pthread_mutex_lock(&lock);
    assert(started && atomic_load(&job->state) == SHADER_JOB_EMPTY);
    job->run = run; job->arg = arg; job->next = NULL;
    atomic_store(&job->state, SHADER_JOB_QUEUED);
    if (tail) tail->next = job; else head = job;
    tail = job;
    ++pending;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

void shader_job_wait(ShaderJob *job) {
    if (atomic_load(&job->state) == SHADER_JOB_DONE) return;
    pthread_mutex_lock(&lock);
    assert(atomic_load(&job->state) != SHADER_JOB_EMPTY);
    if (atomic_load(&job->state) == SHADER_JOB_QUEUED) {
        ShaderJob **link = &head, *previous = NULL;
        while (*link != job) { previous = *link; link = &(*link)->next; }
        *link = job->next;
        if (tail == job) tail = previous;
        atomic_store(&job->state, SHADER_JOB_RUNNING);
        pthread_mutex_unlock(&lock);
        execute(job);
        return;
    }
    while (atomic_load(&job->state) != SHADER_JOB_DONE) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
}

void shader_jobs_drain(void) {
    pthread_mutex_lock(&lock);
    while (pending) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
}
