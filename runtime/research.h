#pragma once
#include "workshop.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RESEARCH_MAX_INPUTS 120000
typedef struct { uint64_t buttons; int32_t axes[4]; } ResearchInput;
bool research_input_valid(const ResearchInput *input);
int research_tape_read(const char *path,const char *build_id,ResearchInput *out,size_t max,char *why,size_t cap);
bool research_tape_write(const char *path,const char *build_id,const ResearchInput *inputs,size_t count,char *why,size_t cap);
bool research_boot(const char *game_root,const char *isolated_save,char *why,size_t cap);
bool research_active(void);
void research_input(uint64_t frame,uint64_t *buttons,int32_t axes[4]);
void research_state(const RouteSample *sample,const void *save_fields,size_t size);
void research_camera(uint64_t frame,const float *matrices);
void research_timer(uint32_t interval);
uint64_t research_vsync_period(void);
void research_finish(void);
#ifdef __cplusplus
}
#endif
