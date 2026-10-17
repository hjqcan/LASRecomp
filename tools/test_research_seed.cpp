#include "research_seed.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
namespace fs=std::filesystem;
static bool active=true;
extern "C" bool research_semantics_active(void){return active;}
extern "C" uint64_t research_elapsed_ns(void){return 123;}
static void env(const char *value){
#ifdef _WIN32
    _putenv_s("LARECOMP_RESEARCH_STARTUP_SEED",value?value:"");
#else
    if(value)setenv("LARECOMP_RESEARCH_STARTUP_SEED",value,1);else unsetenv("LARECOMP_RESEARCH_STARTUP_SEED");
#endif
}
static std::string read(const fs::path &p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv){
    assert(argc==2);fs::path root=fs::u8path(argv[1]);fs::create_directories(root);char why[256];
    uint32_t state[4];research_seed_initial(0,state);
    assert(state[0]==308695721u&&state[1]==3216456777u&&state[2]==3964979509u&&state[3]==1318340232u);
    research_seed_initial(0xffffffffu,state);assert(state[0]==2890487081u&&state[3]==2073701384u);
    env(nullptr);assert(research_seed_boot(false,why,sizeof why));
    assert(research_seed_time(1700000000,true,1,2)==1700000000);
    for(auto invalid:{"-1","4294967296","1x","+1"}){env(invalid);assert(!research_seed_boot(true,why,sizeof why));}
    env("0");assert(!research_seed_boot(false,why,sizeof why));assert(research_seed_boot(true,why,sizeof why));
    assert(research_seed_time(1700000000,true,1,2)==0);
    assert(research_seed_time(1700000000,false,3,2)==1700000000);
    active=false;assert(research_seed_time(1700000000,true,1,2)==1700000000);active=true;
    assert(research_seed_export(root.u8string().c_str(),why,sizeof why));
    auto summary=read(root/"seed-summary.ini");
    assert(summary.find("controlled=1\n")!=std::string::npos&&summary.find("startup_calls=1\n")!=std::string::npos);
    assert(summary.find("time_calls=2\n")!=std::string::npos);
    assert(read(root/"seed-events.csv").find(",1700000000,0,308695721,")!=std::string::npos);
    for(int i=0;i<128;++i)research_seed_time(1,false,3,2);
    assert(research_seed_export(root.u8string().c_str(),why,sizeof why));
    assert(read(root/"seed-summary.ini").find("truncated=1\n")!=std::string::npos);
    unsigned char caller[]={0xe0,0x03,0x1f,0xaa,0xdd,0x9f,0x50,0x94,0xe1,0x03,0x00,0xaa,
        0x40,0xe1,0x00,0xf0,0x00,0xe0,0x07,0x91,0xd5,0x03,0x00,0x94};
    assert(research_seed_site_matches(false,caller,sizeof caller)&&!research_seed_site_matches(true,caller,sizeof caller));
    caller[4]=0x11;caller[5]=0xa0;assert(research_seed_site_matches(true,caller,sizeof caller));
    assert(!research_seed_site_matches(true,caller,23)&&!research_time_site_matches(nullptr,60));
    unsigned char invalid[168]={};assert(!research_seed_initializer_matches(invalid,sizeof invalid));
    std::cout<<"Seed scope, disabled behavior, uint32 bounds, initialization, export and version guards passed\n";
}
