#pragma once
#include <stdatomic.h>

enum { SHADER_JOB_EMPTY, SHADER_JOB_QUEUED, SHADER_JOB_RUNNING, SHADER_JOB_DONE };
typedef struct ShaderJob {
    _Atomic int state;
    void (*run)(void *);
    void *arg;
    struct ShaderJob *next;
} ShaderJob;

void shader_jobs_init(int workers);
void shader_job_submit(ShaderJob *job, void (*run)(void *), void *arg);
// A required queued job runs immediately on the caller; running jobs are joined.
// The job and its inputs remain owned by the caller until completion.
void shader_job_wait(ShaderJob *job);
void shader_jobs_drain(void);
