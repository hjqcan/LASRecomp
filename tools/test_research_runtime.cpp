#include "research.h"
#include "gameversion.h"
#include "research_actors.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
namespace fs=std::filesystem;
static bool known=true,readable=true;
static const char *id="ae16f71e002af8cb059a9a74c4d90f34ba984892";
extern "C" {
volatile uint64_t g_vsync_count=20;
uint64_t plat_now_ns(void){static uint64_t now=1000000000;return now+=1000000;}
bool game_build_id(const char *,char out[41]){strcpy(out,id);return readable;}
GameVersion game_version_id(const char *){return known?GAME_100:GAME_UNSUPPORTED;}
void research_read_actors(ResearchActorSnapshot *out){*out={};out->complete=true;}
}
static void env(const char *key,const char *value){
#ifdef _WIN32
    _putenv_s(key,value?value:"");
#else
    if(value)setenv(key,value,1);else unsetenv(key);
#endif
}
static std::string read(const fs::path &p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv){
    assert(argc==2);fs::path root=fs::u8path(argv[1]);fs::create_directories(root/"one/save");fs::create_directories(root/"two/save");char why[256];
    for(auto key:{"LARECOMP_RESEARCH_CAPTURE","LARECOMP_RESEARCH_REPLAY","LARECOMP_RESEARCH_HZ"})env(key,nullptr);
    assert(research_boot("game",nullptr,why,sizeof why)&&!research_active());
    env("LARECOMP_RESEARCH_HZ","120");assert(!research_boot("game",nullptr,why,sizeof why));
    env("LARECOMP_RESEARCH_CAPTURE","1");assert(!research_boot("game",nullptr,why,sizeof why));
    auto save=(root/"one/save").u8string();known=false;assert(!research_boot("game",save.c_str(),why,sizeof why));known=true;
    readable=false;assert(!research_boot("game",save.c_str(),why,sizeof why));readable=true;
    env("LARECOMP_RESEARCH_REPLAY","unused.lastape");assert(!research_boot("game",save.c_str(),why,sizeof why));env("LARECOMP_RESEARCH_REPLAY",nullptr);
    env("LARECOMP_RESEARCH_HZ","121");assert(!research_boot("game",save.c_str(),why,sizeof why));
    env("LARECOMP_RESEARCH_HZ","120");assert(research_boot("game",save.c_str(),why,sizeof why));assert(research_active()&&research_vsync_period()==8333333);
    research_timer(2);uint64_t buttons=1;int32_t axes[]={123,-456,0,0};research_input(10,&buttons,axes);
    RouteSample sample{};sample.frame=10;sample.hp=12;sample.playing=sample.position=true;strcpy(sample.level,"Field");
    const char fields[]="state";research_state(&sample,fields,sizeof fields);
    float matrices[48]={};for(int m=0;m<3;++m)for(int i=0;i<4;++i)matrices[m*16+i*5]=1;research_camera(10,matrices);
    research_finish();assert(!research_active()&&research_vsync_period()==16666667);research_finish();
    auto tape=root/"one/research/inputs.lastape";ResearchInput input[2];assert(research_tape_read(tape.u8string().c_str(),id,input,2,why,sizeof why)==1);
    assert(input[0].buttons==1&&input[0].axes[1]==-456);
    assert(read(root/"one/research/summary.ini").find("inputs=1\n")!=std::string::npos);
    env("LARECOMP_RESEARCH_CAPTURE",nullptr);env("LARECOMP_RESEARCH_REPLAY",tape.u8string().c_str());env("LARECOMP_RESEARCH_HZ","30");
    save=(root/"two/save").u8string();assert(research_boot("game",save.c_str(),why,sizeof why)&&research_vsync_period()==33333333);
    buttons=0;memset(axes,0,sizeof axes);research_input(1,&buttons,axes);assert(buttons==1&&axes[1]==-456);
    research_input(2,&buttons,axes);assert(buttons==0&&axes[0]==0&&axes[1]==0);research_finish();
    auto result=read(root/"two/research/summary.ini");assert(result.find("replay_consumed=1\n")!=std::string::npos&&result.find("replay_eof=1\n")!=std::string::npos);
    env("LARECOMP_RESEARCH_REPLAY",nullptr);env("LARECOMP_RESEARCH_CAPTURE","1");
    fs::create_directories(root/"semantics/save");save=(root/"semantics/save").u8string();
    env("LARECOMP_RESEARCH_SEMANTICS","1");assert(!research_boot("game",save.c_str(),why,sizeof why));
    env("LARECOMP_RESEARCH_BOUNDARY","update");env("LARECOMP_RESEARCH_UPDATES","1");
    env("LARECOMP_RESEARCH_WARMUP","0");env("LARECOMP_RESEARCH_STABLE","1");env("LARECOMP_RESEARCH_START_LEVEL","Field");
    env("LARECOMP_RESEARCH_SEMANTICS","2");assert(!research_boot("game",save.c_str(),why,sizeof why));
    env("LARECOMP_RESEARCH_SEMANTICS","1");assert(research_boot("game",save.c_str(),why,sizeof why));
    assert(research_semantics_active());
    research_external(RESEARCH_EXTERNAL_ENTROPY,123,4,2,987);
    ResearchPhaseSample point{};point.state=sample;point.state.now_ns=plat_now_ns();point.observed=true;point.rng_observed=true;
    point.rng[0]=123;point.rng[1]=456;point.rng[2]=789;point.rng[3]=42;
    research_phase(RESEARCH_PHASE_UPDATE,true,&point,fields,sizeof fields);
    research_input(11,&buttons,axes);++point.counter;point.rng[3]=43;
    research_phase(RESEARCH_PHASE_UPDATE,false,&point,fields,sizeof fields);
    research_external(RESEARCH_EXTERNAL_CLOCK,456,4,0,123456);
    for(int i=0;i<4095;++i)research_external(RESEARCH_EXTERNAL_ENTROPY,123,4,2,987);
    research_finish();
    result=read(root/"semantics/research/summary.ini");assert(result.find("external_samples=4096\n")!=std::string::npos);
    assert(result.find("external_truncated=1\n")!=std::string::npos);
    assert(read(root/"semantics/research/start-state.ini").find("rng3=42\n")!=std::string::npos);
    assert(read(root/"semantics/research/updates.csv").find(",1,1,123,456,789,42,123,456,789,43\n")!=std::string::npos);
    assert(read(root/"semantics/research/external.csv").find("0,0,123,4,2,987,")!=std::string::npos);
    assert(!research_semantics_active());
    std::cout<<"Research boot gates, clock reset, capture, replay and neutral EOF passed\n";
}
