// Observe verified LAS callbacks around their original implementation. Disabled for ordinary play.
#include "../research.h"
#include "../game_addresses.h"
#include "../gameversion.h"
#include "../rt.h"
#include "../research_actors.h"
#include "../research_seed.h"
#include <math.h>
#include <string.h>

static const size_t sites[RESEARCH_PHASE_COUNT]={GA_FIELD(RESEARCH_UPDATE),GA_FIELD(RESEARCH_INPUT),GA_FIELD(RESEARCH_TASK),GA_FIELD(RESEARCH_TIMER)};
static GuestFn original[RESEARCH_PHASE_COUNT];
static GuestFn original_time;
static void observe_time(Cpu *c) {
    uint64_t caller=c->x[30],argument=c->x[0];
    original_time(c); // Preserve the actual clock operation and its other effects.
    bool startup=caller==GA(RESEARCH_SEED_CALLSITE)+8&&argument==0;
    c->x[0]=research_seed_time(c->x[0],startup,caller,kernel_current_thread_handle());
}
static bool heap_read(void *context,uint64_t address,void *out,size_t size) {
    uint64_t limit=*(const uint64_t *)context;
    if(address<HEAP_BASE||address>limit||size>limit-address)return false;
    memcpy(out,(const void *)(uintptr_t)address,size);return true;
}
void research_read_actors(ResearchActorSnapshot *out) {
    memset(out,0,sizeof *out);out->issue=RESEARCH_ACTORS_UNREADABLE;
    if(!title_is_las()||!research_semantics_active())return;
    extern uint64_t kernel_heap_size(void);
    uint64_t size=kernel_heap_size();if(!size||size>HEAP_MAX)return;
    uint64_t limit=HEAP_BASE+size;
    research_collect_actors(LD64(GA(ACTOR_REGISTRY)),heap_read,&limit,out);
}
static void observe(int phase,bool before,uint64_t object,uint64_t caller) {
    if(!research_phases_active())return;
    extern uint64_t renderer_present_count(void);
    ResearchPhaseSample s={0};s.object=object;s.caller=caller;
    s.thread=kernel_current_thread_handle();
    s.state.frame=renderer_present_count();s.state.now_ns=plat_now_ns();
    s.observed=workshop_read_sample(&s.state,true);
    s.counter=LD32(GA(RESEARCH_STEP_COUNT));
    s.delta=*(const float *)(uintptr_t)GA(RESEARCH_STEP_DELTA);
    if(phase==RESEARCH_PHASE_UPDATE&&research_semantics_active()) {
        memcpy(s.rng,(const void *)(uintptr_t)GA(RESEARCH_RNG_STATE),sizeof s.rng);
        s.rng_observed=true;
    }
    if(!isfinite(s.delta)){s.delta=0;s.observed=false;}
    if(object>=GUEST_LO&&object<GUEST_HI-16) {
        uint64_t context=LD64(object+8);
        if(context>=GUEST_LO&&context<GUEST_HI-0x512)
            s.blocked=LD8(context+0x510)|(LD8(context+0x511)<<8);
    }
    research_phase(phase,before,&s,s.observed&&s.state.playing?(const void *)(uintptr_t)(GA(LIVE_SAVE)+0x5388):NULL,
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
    if(!title_is_las()||!research_phases_active())return true;
    if(research_semantics_active()) {
        static const uint32_t initial[4]={123456789,362436069,521288629,88675123};
        if(!research_rng_site_matches((const void *)(uintptr_t)GA(RESEARCH_RNG_PROBE),40)||
           memcmp((const void *)(uintptr_t)GA(RESEARCH_RNG_STATE),initial,sizeof initial)) {
            if(cap)snprintf(why,cap,"Research RNG instruction/data guard failed.");
            return false;
        }
        uint64_t site=loader_find_symbol("time");
        original_time=dispatch_lookup(site);
        if(site!=GA(RESEARCH_TIME_ENTRY)||!original_time||
           !research_time_site_matches((const void *)(uintptr_t)site,60)||
           !research_seed_site_matches(game_version_current()==GAME_110,(const void *)(uintptr_t)GA(RESEARCH_SEED_CALLSITE),24)||
           !research_seed_initializer_matches((const void *)(uintptr_t)GA(RESEARCH_SEED_INITIALIZER),168)) {
            if(cap)snprintf(why,cap,"Research startup-seed caller/initializer/SDK guard failed.");
            return false;
        }
    }
    for(int i=0;i<RESEARCH_PHASE_COUNT;++i) {
        uint64_t site=GA_AT(sites[i]);
        original[i]=dispatch_lookup(site);
        if(!original[i]||!research_site_matches(i,(const void *)(uintptr_t)site,16)) {
            if(cap)snprintf(why,cap,"Research callback instruction/entry guard failed (%d).",i);
            return false;
        }
    }
    return true;
}
GuestFn research_dispatch_override(uint64_t addr) {
    if(!title_is_las()||!research_phases_active())return NULL;
    if(research_semantics_active()&&addr==GA(RESEARCH_TIME_ENTRY))return observe_time;
    for(int i=0;i<RESEARCH_PHASE_COUNT;++i)if(addr==GA_AT(sites[i]))return wrappers[i];
    return NULL;
}
