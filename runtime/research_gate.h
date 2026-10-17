#pragma once
#include "workshop.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    char level[64];
    unsigned warmup_required,stable_required,warmup,stable;
    bool holding,ready,previous_valid;
    RouteSample previous;
} ResearchGate;
bool research_gate_begin(ResearchGate *gate,const char *level,unsigned warmup,unsigned stable);
bool research_gate_step(ResearchGate *gate,const RouteSample *sample,bool observed,bool blocked);
#ifdef __cplusplus
}
#endif
