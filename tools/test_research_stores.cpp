#include "research_stores.h"
#include "game_addresses.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
namespace fs=std::filesystem;
static bool library=true;
extern "C" const char *gamelib_research_stores(void){return library?"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa":nullptr;}
extern "C" uint64_t research_elapsed_ns(void){static uint64_t n;return ++n;}
extern "C" uint32_t kernel_current_thread_handle(void){return 7;}
static void env(const char *key,const char *value){
#ifdef _WIN32
    _putenv_s(key,value?value:"");
#else
    if(value)setenv(key,value,1);else unsetenv(key);
#endif
}
static std::string read(const fs::path &p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv){
    assert(argc==2);fs::path root=fs::u8path(argv[1]);char why[256];uint32_t a[4]={1,2,3,4},b[4]={9,2,3,4};
    for(auto key:{"LARECOMP_RESEARCH_STORE_FROM","LARECOMP_RESEARCH_STORE_TO","LARECOMP_RESEARCH_STORE_MAX"})env(key,nullptr);
    assert(research_stores_boot(false,why,sizeof why));
    assert(!research_store_begin(1,2,3,GA_RESEARCH_RNG_STATE,4,a));
    env("LARECOMP_RESEARCH_STORE_FROM","2");env("LARECOMP_RESEARCH_STORE_TO","4");
    assert(!research_stores_boot(false,why,sizeof why));library=false;assert(!research_stores_boot(true,why,sizeof why));library=true;
    env("LARECOMP_RESEARCH_STORE_MAX","0");assert(!research_stores_boot(true,why,sizeof why));
    env("LARECOMP_RESEARCH_STORE_MAX","2x");assert(!research_stores_boot(true,why,sizeof why));
    env("LARECOMP_RESEARCH_STORE_MAX","2");assert(research_stores_boot(true,why,sizeof why));
    research_stores_boundary(true,1,7,a);assert(!g_research_stores);
    research_stores_boundary(true,2,7,a);assert(g_research_stores);
    auto token=research_store_begin(1,2,3,GA_RESEARCH_RNG_STATE,4,a);assert(token);research_store_end(token,b);
    research_stores_boundary(false,2,7,b);research_stores_boundary(true,3,7,b);research_stores_boundary(false,3,7,b);assert(!g_research_stores);
    fs::create_directories(root/"complete");assert(research_stores_export((root/"complete").u8string().c_str(),why,sizeof why));
    auto complete=read(root/"complete/stores-summary.ini");assert(complete.find("recording_complete=1\n")!=std::string::npos);
    assert(read(root/"complete/stores.csv").find(",1,2,3,4,9,2,3,4\n")!=std::string::npos);
    for(const char *mode:{"overflow","overlap","pending","outside","window"}) {
        env("LARECOMP_RESEARCH_STORE_MAX",std::string(mode)=="overflow"?"1":"2");assert(research_stores_boot(true,why,sizeof why));
        research_stores_boundary(true,2,7,a);
        auto first=research_store_begin(1,2,3,GA_RESEARCH_RNG_STATE,4,a);assert(first);
        if(std::string(mode)=="overlap") {auto second=research_store_begin(2,2,3,GA_RESEARCH_RNG_STATE,4,a);assert(second);research_store_end(second,b);}
        if(std::string(mode)!="pending")research_store_end(first,b);
        if(std::string(mode)=="overflow")assert(!research_store_begin(2,2,3,GA_RESEARCH_RNG_STATE,4,b));
        research_stores_boundary(false,2,7,b);
        if(std::string(mode)=="outside"){auto second=research_store_begin(2,2,3,GA_RESEARCH_RNG_STATE,4,b);assert(second);research_store_end(second,b);}
        if(std::string(mode)!="window"){research_stores_boundary(true,3,7,b);research_stores_boundary(false,3,7,b);}
        fs::create_directories(root/mode);assert(research_stores_export((root/mode).u8string().c_str(),why,sizeof why));
        assert(read(root/mode/"stores-summary.ini").find("recording_complete=0\n")!=std::string::npos);
    }
    std::cout<<"Store trace gates, limits, pairing, overlap, ownership, complete windows and exports passed\n";
}
