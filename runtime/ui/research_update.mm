// Input epochs at the verified LAS update-dispatch callback. This is not a process snapshot.
#include "../research_update.h"
#include "../research_gate.h"
#include "../research_actors.h"
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
namespace {
struct Point { ResearchPhaseSample sample{};uint64_t digest=0,ns=0,timers=0,vsyncs=0; };
struct Update { Point before,after;ResearchInput input{};size_t polls=0; };
struct ActorRow { size_t update;char level[64];ResearchActorSnapshot snapshot; };
std::mutex update_lock;
std::atomic<bool> active,done;
ResearchGate gate{};
ResearchUpdateStatus status{};
std::vector<ResearchInput> source;
std::vector<Update> updates;
std::vector<ActorRow> actor_rows;
Point entry,origin;
ResearchInput latched{};
size_t limit,cycle_polls,total_callbacks;
uint32_t owner;
bool owner_set,cycle_recording,input_latched;
std::string error;
thread_local unsigned depth;
const char *keys[]={"LARECOMP_RESEARCH_BOUNDARY","LARECOMP_RESEARCH_START_LEVEL","LARECOMP_RESEARCH_WARMUP",
                    "LARECOMP_RESEARCH_STABLE","LARECOMP_RESEARCH_UPDATES","LARECOMP_RESEARCH_DRIVER"};
void require(bool value,const char *why){if(!value)throw std::runtime_error(why);}
size_t number(const char *key,size_t fallback,size_t maximum,size_t minimum=0) {
    const char *s=getenv(key);
    if(!s){require(fallback>=minimum&&fallback<=maximum,"Default update count exceeds its limit.");return fallback;}
    size_t n=0;auto result=std::from_chars(s,s+strlen(s),n);
    require(result.ec==std::errc()&&*result.ptr=='\0'&&n>=minimum&&n<=maximum,"Invalid update-research numeric limit.");
    return n;
}
void fail(const char *why){if(error.empty())error=why;status.integrity_error=true;done=true;cycle_recording=false;}
void stream_setup(std::ofstream &stream){stream.imbue(std::locale::classic());stream<<std::setprecision(9);}
void stream_close(std::ofstream &stream){stream.close();require(bool(stream),"Update research export failed.");}
}
extern "C" bool research_update_configured(void){for(auto key:keys)if(getenv(key))return true;return false;}
extern "C" bool research_update_active(void){return active.load(std::memory_order_relaxed);}
extern "C" bool research_update_done(void){return done.load(std::memory_order_relaxed);}
extern "C" bool research_update_boot(const char *id,const char *replay,char *why,size_t cap) {
    std::lock_guard<std::mutex> guard(update_lock);
    active=false;done=false;status={};source.clear();updates.clear();actor_rows.clear();error.clear();gate={};depth=0;
    owner_set=cycle_recording=input_latched=false;cycle_polls=total_callbacks=0;entry={};origin={};latched={};
    try {
        const char *boundary=getenv(keys[0]);
        if(!boundary||!strcmp(boundary,"hid")) {
            for(size_t i=1;i<sizeof keys/sizeof *keys;++i)require(!getenv(keys[i]),"Update controls require the update boundary.");
            return true;
        }
        require(!strcmp(boundary,"update"),"Research boundary must be hid or update.");
        const char *level=getenv(keys[1]);if(!level)level="MarinTarinHouse";
        require(research_gate_begin(&gate,level,(unsigned)number(keys[2],240,3600),(unsigned)number(keys[3],60,3600,1)),"Invalid update start gate.");
        const char *driver=getenv(keys[5]);
        require(!(replay&&driver),"Replay and a scripted capture driver cannot be combined.");
        const char *path=replay?replay:driver;
        if(path) {
            source.resize(RESEARCH_MAX_INPUTS);char message[256];
            int n=research_update_tape_read(path,id,source.data(),source.size(),message,sizeof message);
            require(n>0,message);source.resize(n);
        }
        limit=number(keys[4],source.empty()?1200:source.size(),12000,1);
        require(source.empty()||source.size()==limit,"Update count must equal the complete source tape length.");
        updates.reserve(limit);status.active=true;status.playback=replay!=nullptr;status.driven=driver!=nullptr;
        status.source_inputs=source.size();active=true;return true;
    }catch(const std::exception &e){if(cap)snprintf(why,cap,"%s",e.what());return false;}
}
extern "C" void research_update_input(uint64_t *buttons,int32_t axes[4]) {
    if(!research_update_active())return;
    std::lock_guard<std::mutex> guard(update_lock);
    if(status.completed||status.integrity_error){*buttons=0;memset(axes,0,4*sizeof *axes);++status.neutral_after_complete;return;}
    if(!gate.holding)return; // The existing bootstrap script can reach the selected level.
    if(gate.ready&&(depth!=1||!cycle_recording))fail("Primary-controller poll outside the captured update");
    if(!gate.ready||status.integrity_error){*buttons=0;memset(axes,0,4*sizeof *axes);return;}
    if(!input_latched) {
        if(!source.empty()){latched=source[updates.size()];++status.source_consumed;}
        else{latched.buttons=*buttons;memcpy(latched.axes,axes,sizeof latched.axes);}
        input_latched=true;
    }
    *buttons=latched.buttons;memcpy(axes,latched.axes,sizeof latched.axes);++cycle_polls;
}
extern "C" void research_update_phase(bool before,const ResearchPhaseSample *sample,uint64_t digest,uint64_t ns,uint64_t timers,uint64_t vsyncs) {
    if(!research_update_active())return;
    std::lock_guard<std::mutex> guard(update_lock);
    Point point{*sample,digest,ns,timers,vsyncs};
    if(before) {
        if(++depth!=1){fail("Nested update callbacks");return;}
        if(owner_set&&owner!=sample->thread){fail("Update callback moved between guest threads");return;}
        owner=sample->thread;owner_set=true;++total_callbacks;
        entry=point;cycle_polls=0;input_latched=false;cycle_recording=false;
        if(done)return;
        bool was_ready=gate.ready;
        if(research_gate_step(&gate,&sample->state,sample->observed,sample->blocked!=0)) {
            if(!was_ready){origin=point;status.ready=true;fprintf(stderr,"[research] update start gate ready at counter %u (%s)\n",sample->counter,sample->state.level);}
            cycle_recording=true;
        }
        return;
    }
    if(!depth){fail("Unpaired update exit");return;}
    if(--depth)return;
    if(status.completed||status.integrity_error)return;
    if((uint32_t)(sample->counter-entry.sample.counter)!=1){fail("Update counter did not advance exactly once");return;}
    if(cycle_recording) {
        if(!input_latched){fail("Captured update had no primary-controller poll");return;}
        if(sample->rng_observed&&updates.size()%10==0) {
            ActorRow row{};row.update=updates.size();memcpy(row.level,sample->state.level,sizeof row.level);
            row.snapshot.issue=RESEARCH_ACTORS_UNREADABLE;
            if(sample->observed&&sample->state.playing)research_read_actors(&row.snapshot);
            actor_rows.push_back(row);
        }
        updates.push_back({entry,point,latched,cycle_polls});status.inputs=updates.size();
        if(updates.size()==limit){status.completed=true;done=true;}
    }
    cycle_recording=false;
}
extern "C" ResearchUpdateStatus research_update_status(void){std::lock_guard<std::mutex> guard(update_lock);return status;}
extern "C" bool research_update_export(const char *directory,const char *id,char *why,size_t cap) {
    std::lock_guard<std::mutex> guard(update_lock);
    try {
        fs::path root=fs::u8path(directory);
        std::ofstream meta(root/"update-summary.ini",std::ios::binary);stream_setup(meta);
        meta<<"format=1\nboundary=las_update_dispatch_v1\nrequested_updates="<<limit<<"\ncaptured_updates="<<updates.size()
            <<"\nsource_inputs="<<source.size()<<"\nsource_consumed="<<status.source_consumed<<"\nsource_mode="<<(status.playback?"replay":status.driven?"driver":"host")
            <<"\ngate_ready="<<status.ready<<"\ngate_level="<<gate.level<<"\nwarmup_required="<<gate.warmup_required<<"\nstable_required="<<gate.stable_required
            <<"\nwarmup_observed="<<gate.warmup<<"\nstable_observed="<<gate.stable<<"\ncompleted="<<status.completed<<"\nintegrity_error="<<status.integrity_error
            <<"\nerror="<<error<<"\ntotal_callbacks="<<total_callbacks<<"\nneutral_after_complete="<<status.neutral_after_complete<<'\n';
        stream_close(meta);
        if(status.ready) {
            std::ofstream start(root/"start-state.ini",std::ios::binary);stream_setup(start);const auto &s=origin.sample.state;
            start<<"format=1\nlevel="<<s.level<<"\nhp="<<s.hp<<"\nx="<<s.x<<"\ny="<<s.y<<"\nz="<<s.z
                 <<"\nsave_digest="<<origin.digest<<"\ncounter="<<origin.sample.counter<<"\nframe="<<s.frame<<"\nelapsed_ns="<<origin.ns
                 <<"\nrng_observed="<<origin.sample.rng_observed<<'\n';
            for(int i=0;i<4;++i)start<<"rng"<<i<<'='<<origin.sample.rng[i]<<'\n';
            stream_close(start);
        }
        std::ofstream output(root/"updates.csv",std::ios::binary);stream_setup(output);
        output<<"update,frame,elapsed_ns,timer_calls,vsyncs,observed,playing,level,hp,position,x,y,z,save_digest,counter_before,counter_after,delta,blocked,hid_polls,rng_before_observed,rng_observed";
        for(const char *prefix:{"rng_before","rng"})for(int i=0;i<4;++i)output<<','<<prefix<<i;
        output<<'\n';
        std::vector<ResearchInput> inputs;inputs.reserve(updates.size());
        for(size_t i=0;i<updates.size();++i){const auto &u=updates[i];const auto &a=u.after.sample;const auto &s=a.state;
            output<<i<<','<<s.frame<<','<<u.after.ns<<','<<u.after.timers<<','<<u.after.vsyncs<<','<<a.observed<<','<<s.playing<<','<<s.level<<','<<s.hp
                  <<','<<s.position<<','<<s.x<<','<<s.y<<','<<s.z<<','<<u.after.digest<<','<<u.before.sample.counter<<','<<a.counter<<','<<a.delta<<','<<a.blocked<<','<<u.polls
                  <<','<<u.before.sample.rng_observed<<','<<a.rng_observed;
            for(auto v:u.before.sample.rng)output<<','<<v;
            for(auto v:a.rng)output<<','<<v;
            output<<'\n';
            inputs.push_back(u.input);
        }
        stream_close(output);
        if(!actor_rows.empty()) {
            std::ofstream index(root/"actor-samples.csv",std::ios::binary),actors(root/"actors.csv",std::ios::binary);
            stream_setup(index);stream_setup(actors);
            index<<"update,level,complete,issue,registry_count,selected_count\n";
            actors<<"update,key,vtable,name,component,position,x,y,z\n";
            for(const auto &row:actor_rows){const auto &snapshot=row.snapshot;
                index<<row.update<<','<<row.level<<','<<snapshot.complete<<','<<snapshot.issue<<','<<snapshot.registry_count<<','<<snapshot.count<<'\n';
                if(!snapshot.complete)continue; // Partial walks are diagnostics, never compared as complete rosters.
                for(unsigned i=0;i<snapshot.count;++i){const auto &a=snapshot.actors[i];
                    actors<<row.update<<','<<a.key<<','<<a.vtable<<','<<a.name<<','<<a.component<<','<<a.position
                          <<','<<a.xyz[0]<<','<<a.xyz[1]<<','<<a.xyz[2]<<'\n';}
            }
            stream_close(index);stream_close(actors);
        }
        if(!inputs.empty()) {
            char message[256];require(research_update_tape_write((root/"inputs.lastape").u8string().c_str(),id,inputs.data(),inputs.size(),message,sizeof message),message);
        }
        return true;
    }catch(const std::exception &e){if(cap)snprintf(why,cap,"%s",e.what());return false;}
}
