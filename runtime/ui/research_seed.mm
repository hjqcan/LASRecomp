// A scoped experiment: only the verified time(NULL) caller feeding the LAS RNG.
#include "../research_seed.h"
#include "../research.h"
#include "../platform.h"
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>
#include <vector>

namespace {
struct Seed { uint64_t original,effective,caller,ns;uint32_t thread;bool startup; };
std::mutex seed_lock;
std::vector<Seed> events;
bool controlled,truncated;
uint32_t seed;
}
extern "C" bool research_seed_boot(bool semantics,char *why,size_t cap) {
    std::lock_guard<std::mutex> guard(seed_lock);
    events.clear();controlled=truncated=false;seed=0;
    const char *option=getenv("LARECOMP_RESEARCH_STARTUP_SEED");
    if(!option)return true;
    auto result=std::from_chars(option,option+strlen(option),seed);
    if(!semantics||result.ec!=std::errc()||*result.ptr||!*option) {
        if(cap)snprintf(why,cap,"Startup-seed control requires semantic observations and a uint32 value.");
        return false;
    }
    controlled=true;return true;
}
extern "C" uint64_t research_seed_time(uint64_t original,bool startup,uint64_t caller,uint32_t thread) {
    if(!research_semantics_active())return original;
    std::lock_guard<std::mutex> guard(seed_lock);
    uint64_t effective=controlled&&startup?seed:original;
    if(events.size()<128)events.push_back({original,effective,caller,research_elapsed_ns(),thread,startup});
    else truncated=true;
    return effective;
}
extern "C" bool research_seed_export(const char *directory,char *why,size_t cap) {
    std::lock_guard<std::mutex> guard(seed_lock);
    auto root=std::filesystem::u8path(directory);
    std::ofstream meta(root/"seed-summary.ini",std::ios::binary),rows(root/"seed-events.csv",std::ios::binary);
    meta.imbue(std::locale::classic());rows.imbue(std::locale::classic());
    size_t startup=0;for(const auto &e:events)startup+=e.startup;
    meta<<"format=1\ncontrolled="<<controlled<<"\nconfigured_seed="<<seed<<"\nstartup_calls="<<startup
        <<"\ntime_calls="<<events.size()<<"\ntruncated="<<truncated<<'\n';
    rows<<"event,startup,caller,thread,elapsed_ns,original,effective,initial0,initial1,initial2,initial3\n";
    for(size_t i=0;i<events.size();++i){const auto &e=events[i];uint32_t state[4];research_seed_initial((uint32_t)e.effective,state);
        rows<<i<<','<<e.startup<<','<<e.caller<<','<<e.thread<<','<<e.ns<<','<<e.original<<','<<e.effective;
        for(auto v:state)rows<<','<<v;rows<<'\n';}
    meta.close();rows.close();
    if(!meta||!rows){if(cap)snprintf(why,cap,"Seed observation export failed.");return false;}
    return true;
}
