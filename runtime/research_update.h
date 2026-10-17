#pragma once
#include "research.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    size_t inputs,source_inputs,source_consumed,neutral_after_complete;
    bool active,playback,driven,ready,completed,integrity_error;
} ResearchUpdateStatus;
bool research_update_configured(void);
bool research_update_boot(const char *build_id,const char *replay_path,char *why,size_t cap);
bool research_update_active(void);
bool research_update_done(void);
void research_update_input(uint64_t *buttons,int32_t axes[4]);
typedef struct { ResearchIoStats io;const void *fields;size_t size; } ResearchDetails;
void research_update_phase(bool before,const ResearchPhaseSample *sample,uint64_t digest,uint64_t ns,uint64_t timers,uint64_t vsyncs,const ResearchDetails *details);
ResearchUpdateStatus research_update_status(void);
bool research_update_export(const char *directory,const char *build_id,char *why,size_t cap);
#ifdef __cplusplus
}
#endif
