// Bounded observations of generated memory stores; does not serialize guest writes.
#include "../research_stores.h"
#include "../research.h"
#include "../game_addresses.h"
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" { unsigned g_research_stores=0;uint32_t kernel_current_thread_handle(void); }
namespace {
struct Write {
    uint64_t begin=0,end=0,ns=0,site=0,function=0,caller=0,address=0;
    size_t window=0;int64_t update=-1;uint32_t thread=0,size=0,before[4]{},after[4]{};
};
struct Boundary { uint64_t sequence,ns;size_t update;uint32_t thread,state[4];bool before; };
std::mutex mutex;
std::vector<Write> writes;
std::vector<Boundary> boundaries;
size_t first,last,capacity,count,seen,dropped,pending,overlaps,outside,invalid,current;
uint64_t sequence;
bool configured,opened,closed;
std::string manifest;
thread_local int64_t owned_update=-1;
size_t number(const char *key,size_t fallback,size_t maximum) {
    const char *s=getenv(key);if(!s)return fallback;
    size_t value=0;auto result=std::from_chars(s,s+strlen(s),value);
    if(!*s||result.ec!=std::errc()||*result.ptr||value>maximum)throw std::runtime_error("Invalid store-trace limit.");
    return value;
}
void state(std::ofstream &out,const uint32_t words[4]) { for(int i=0;i<4;++i)out<<','<<words[i]; }
}
extern "C" bool research_stores_boot(bool semantics,char *why,size_t cap) {
    std::lock_guard<std::mutex> guard(mutex);
    __atomic_store_n(&g_research_stores,0,__ATOMIC_RELAXED);
    writes.clear();boundaries.clear();configured=opened=closed=false;manifest.clear();owned_update=-1;
    first=last=capacity=count=seen=dropped=pending=overlaps=outside=invalid=current=0;sequence=0;
    try {
        const char *start=getenv("LARECOMP_RESEARCH_STORE_FROM"),*end=getenv("LARECOMP_RESEARCH_STORE_TO"),*max=getenv("LARECOMP_RESEARCH_STORE_MAX");
        if(!start&&!end&&!max)return true;
        if(!semantics||!start||!end)throw std::runtime_error("Store tracing requires semantic update capture and both window limits.");
        first=number("LARECOMP_RESEARCH_STORE_FROM",0,12000);last=number("LARECOMP_RESEARCH_STORE_TO",0,12000);
        capacity=number("LARECOMP_RESEARCH_STORE_MAX",65536,262144);
        if(first>=last||!capacity)throw std::runtime_error("Store trace needs a nonempty update window and buffer.");
        const char *id=gamelib_research_stores();
        if(!id||strlen(id)!=64||strspn(id,"0123456789abcdef")!=64)
            throw std::runtime_error("Store tracing needs an explicitly instrumented diagnostic game library.");
        manifest=id;writes.resize(capacity);boundaries.reserve(2*(last-first));configured=true;return true;
    }catch(const std::exception &e){if(cap)snprintf(why,cap,"%s",e.what());return false;}
}
extern "C" void research_stores_boundary(bool before,size_t update,uint32_t thread,const uint32_t words[4]) {
    if(!configured)return;
    std::lock_guard<std::mutex> guard(mutex);
    if(update<first||update>=last)return;
    if(boundaries.size()>=2*(last-first)){++invalid;return;}
    if(before) {
        if(owned_update!=-1||closed)++invalid;
        owned_update=(int64_t)update;current=update;opened=true;
    }
    Boundary row{++sequence,research_elapsed_ns(),update,thread,{},before};memcpy(row.state,words,16);boundaries.push_back(row);
    if(before)__atomic_store_n(&g_research_stores,1,__ATOMIC_RELAXED);
    else {
        if(owned_update!=(int64_t)update)++invalid;
        owned_update=-1;
        if(update+1==last){closed=true;__atomic_store_n(&g_research_stores,0,__ATOMIC_RELAXED);}
    }
}
extern "C" uint64_t research_store_begin(uint64_t site,uint64_t function,uint64_t caller,uint64_t address,unsigned size,const uint32_t before[4]) {
    if(!__atomic_load_n(&g_research_stores,__ATOMIC_RELAXED))return 0;
    std::lock_guard<std::mutex> guard(mutex);
    if(!__atomic_load_n(&g_research_stores,__ATOMIC_RELAXED))return 0;
    ++seen;
    if(!site||!before||(size!=1&&size!=2&&size!=4&&size!=8&&size!=16)||
       address>=GA_RESEARCH_RNG_STATE+16||address<=GA_RESEARCH_RNG_STATE-size){++invalid;return 0;}
    if(count==capacity){++dropped;return 0;}
    if(pending)++overlaps;
    if(owned_update<0)++outside;
    auto &w=writes[count++];w.begin=++sequence;w.ns=research_elapsed_ns();w.site=site;w.function=function;w.caller=caller;w.address=address;
    w.window=current;w.update=owned_update;w.thread=kernel_current_thread_handle();w.size=size;memcpy(w.before,before,16);++pending;
    return count;
}
extern "C" void research_store_end(uint64_t token,const uint32_t after[4]) {
    std::lock_guard<std::mutex> guard(mutex);
    if(!token||token>count||!after||writes[token-1].end){++invalid;return;}
    auto &w=writes[token-1];memcpy(w.after,after,16);w.end=++sequence;--pending;
}
extern "C" bool research_stores_export(const char *directory,char *why,size_t cap) {
    std::lock_guard<std::mutex> guard(mutex);
    __atomic_store_n(&g_research_stores,0,__ATOMIC_RELAXED);
    if(!configured)return true;
    auto root=std::filesystem::u8path(directory);
    std::ofstream meta(root/"stores-summary.ini",std::ios::binary),rows(root/"stores.csv",std::ios::binary),bounds(root/"store-boundaries.csv",std::ios::binary);
    for(auto *s:{&meta,&rows,&bounds})s->imbue(std::locale::classic());
    bool complete=opened&&closed&&!dropped&&!pending&&!overlaps&&!outside&&!invalid&&boundaries.size()==2*(last-first);
    meta<<"format=1\nmanifest="<<manifest<<"\ncoverage=static_page_reference_functions\nconsumption_reconstruction=not_established\nfirst_update="<<first<<"\nend_update="<<last
        <<"\ncapacity="<<capacity<<"\nseen="<<seen<<"\nrecorded="<<count<<"\ndropped="<<dropped<<"\npending="<<pending<<"\noverlaps="<<overlaps
        <<"\noutside_update="<<outside<<"\ninvalid="<<invalid<<"\nwindow_opened="<<opened<<"\nwindow_closed="<<closed<<"\nrecording_complete="<<complete<<'\n';
    rows<<"write,begin,end,elapsed_ns,site,function,caller,address,size,window_update,update,thread,before0,before1,before2,before3,after0,after1,after2,after3\n";
    for(size_t i=0;i<count;++i){const auto &w=writes[i];rows<<i<<','<<w.begin<<','<<w.end<<','<<w.ns<<','<<w.site<<','<<w.function<<','<<w.caller<<','<<w.address<<','<<w.size<<','<<w.window<<','<<w.update<<','<<w.thread;state(rows,w.before);state(rows,w.after);rows<<'\n';}
    bounds<<"sequence,elapsed_ns,update,thread,before,rng0,rng1,rng2,rng3\n";
    for(const auto &b:boundaries){bounds<<b.sequence<<','<<b.ns<<','<<b.update<<','<<b.thread<<','<<b.before;state(bounds,b.state);bounds<<'\n';}
    meta.close();rows.close();bounds.close();
    if(!meta||!rows||!bounds){if(cap)snprintf(why,cap,"Store trace export failed.");return false;}return true;
}
