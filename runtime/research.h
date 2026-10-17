#pragma once
#include "workshop.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RESEARCH_MAX_INPUTS 120000
typedef struct { uint64_t buttons; int32_t axes[4]; } ResearchInput;
enum { RESEARCH_PHASE_UPDATE, RESEARCH_PHASE_INPUT, RESEARCH_PHASE_TASK, RESEARCH_PHASE_TIMER, RESEARCH_PHASE_COUNT };
typedef struct {
    RouteSample state;
    uint64_t object,caller;
    uint32_t counter,thread;
    float delta;
    unsigned blocked;
    bool observed;
    bool rng_observed;
    uint32_t rng[4];
} ResearchPhaseSample;
bool research_input_valid(const ResearchInput *input);
int research_tape_read(const char *path,const char *build_id,ResearchInput *out,size_t max,char *why,size_t cap);
bool research_tape_write(const char *path,const char *build_id,const ResearchInput *inputs,size_t count,char *why,size_t cap);
int research_update_tape_read(const char *path,const char *build_id,ResearchInput *out,size_t max,char *why,size_t cap);
bool research_update_tape_write(const char *path,const char *build_id,const ResearchInput *inputs,size_t count,char *why,size_t cap);
bool research_boot(const char *game_root,const char *isolated_save,char *why,size_t cap);
bool research_active(void);
bool research_phases_active(void);
bool research_semantics_active(void);
uint64_t research_elapsed_ns(void);
typedef struct { uint64_t started,completed,bytes,failed; } ResearchIoStats;
bool research_io_begin(void);
void research_io_end(int64_t bytes);
ResearchIoStats research_io_snapshot(void);
enum { RESEARCH_EXTERNAL_ENTROPY, RESEARCH_EXTERNAL_CLOCK };
void research_external(int kind,uint64_t pc,uint32_t thread,uint64_t index,uint64_t value);
bool research_rng_site_matches(const void *bytes,size_t size);
bool research_exit_ready(void);
bool research_hooks_init(char *why,size_t cap);
bool research_site_matches(int phase,const void *bytes,size_t size);
void research_phase(int phase,bool before,const ResearchPhaseSample *sample,const void *fields,size_t size);
void research_input(uint64_t frame,uint64_t *buttons,int32_t axes[4]);
void research_state(const RouteSample *sample,const void *save_fields,size_t size);
void research_camera(uint64_t frame,const float *matrices);
void research_timer(uint32_t interval);
uint64_t research_vsync_period(void);
void research_finish(void);
#ifdef __cplusplus
}
#endif
