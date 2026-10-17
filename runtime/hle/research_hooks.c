// Observe verified LAS callbacks around their original implementation. Disabled for ordinary play.
#include "../research.h"
#include "../game_addresses.h"
#include "../gameversion.h"
#include "../rt.h"
#include <math.h>

static const uint64_t sites[RESEARCH_PHASE_COUNT]={GA_RESEARCH_UPDATE,GA_RESEARCH_INPUT,GA_RESEARCH_TASK,GA_RESEARCH_TIMER};
static GuestFn original[RESEARCH_PHASE_COUNT];
static void observe(int phase,bool before,uint64_t object,uint64_t caller) {
    if(!research_phases_active())return;
    extern uint64_t renderer_present_count(void);
    ResearchPhaseSample s={0};s.object=object;s.caller=caller;
    s.thread=kernel_current_thread_handle();
    s.state.frame=renderer_present_count();s.state.now_ns=plat_now_ns();
    s.observed=workshop_read_sample(&s.state,true);
    s.counter=LD32(GA_RESEARCH_STEP_COUNT);
    s.delta=*(const float *)(uintptr_t)GA_RESEARCH_STEP_DELTA;
    if(!isfinite(s.delta)){s.delta=0;s.observed=false;}
    if(object>=GUEST_LO&&object<GUEST_HI-16) {
        uint64_t context=LD64(object+8);
        if(context>=GUEST_LO&&context<GUEST_HI-0x512)
            s.blocked=LD8(context+0x510)|(LD8(context+0x511)<<8);
    }
    research_phase(phase,before,&s,s.observed&&s.state.playing?(const void *)(uintptr_t)(GA_LIVE_SAVE+0x5388):NULL,
                   s.observed&&s.state.playing?0x259:0);
}
static void invoke(Cpu *c,int phase) {
    uint64_t object=c->x[0],caller=c->x[30];
    observe(phase,true,object,caller);
    original[phase](c);
    observe(phase,false,object,caller);
}
static void update(Cpu *c){invoke(c,RESEARCH_PHASE_UPDATE);}
static void input(Cpu *c){invoke(c,RESEARCH_PHASE_INPUT);}
static void task(Cpu *c){invoke(c,RESEARCH_PHASE_TASK);}
static void timer(Cpu *c){invoke(c,RESEARCH_PHASE_TIMER);}
static const GuestFn wrappers[RESEARCH_PHASE_COUNT]={update,input,task,timer};

bool research_hooks_init(char *why,size_t cap) {
    if(!research_phases_active())return true;
    for(int i=0;i<RESEARCH_PHASE_COUNT;++i) {
        uint64_t site=game_main_address(sites[i]);
        original[i]=dispatch_lookup(site);
        if(!original[i]||!research_site_matches(i,(const void *)(uintptr_t)site,16)) {
            if(cap)snprintf(why,cap,"Research callback instruction/entry guard failed (%d).",i);
            return false;
        }
    }
    return true;
}
GuestFn research_dispatch_override(uint64_t addr) {
    if(!research_phases_active())return NULL;
    uint64_t canonical=game_canonical_address(addr);
    for(int i=0;i<RESEARCH_PHASE_COUNT;++i)if(canonical==sites[i])return wrappers[i];
    return NULL;
}
