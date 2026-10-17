#include "research_update.h"
#include "research_gate.h"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
namespace fs=std::filesystem;
static void env(const char *key,const char *value){
#ifdef _WIN32
    _putenv_s(key,value?value:"");
#else
    if(value)setenv(key,value,1);else unsetenv(key);
#endif
}
static std::string read(const fs::path &p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char **argv){
    assert(argc==2);auto root=fs::u8path(argv[1]);fs::create_directories(root);char why[256];
    const char *id="ae16f71e002af8cb059a9a74c4d90f34ba984892";
    ResearchGate gate;RouteSample s{};strcpy(s.level,"House");s.hp=12;s.playing=s.position=true;s.x=1;
    assert(!research_gate_begin(&gate,"../House",3,2));assert(!research_gate_begin(&gate,"House",3601,2));
    assert(!research_gate_begin(&gate,"House",3,0));assert(research_gate_begin(&gate,"House",3,2));
    assert(!research_gate_step(&gate,&s,true,false)&&gate.holding);
    s.x=2;assert(!research_gate_step(&gate,&s,true,false)&&gate.stable==1);
    assert(research_gate_step(&gate,&s,true,false));
    assert(research_gate_begin(&gate,"House",3,2));
    assert(!research_gate_step(&gate,&s,true,false));
    assert(!research_gate_step(&gate,&s,true,true)&&gate.warmup==0);
    s.x=NAN;assert(!research_gate_step(&gate,&s,true,false));s.x=1;
    assert(!research_gate_step(&gate,&s,false,false));
    strcpy(s.level,"Other");assert(!research_gate_step(&gate,&s,true,false));strcpy(s.level,"House");
    unsigned char signature[]={0xf3,0x0f,0x1e,0xf8,0xfd,0x7b,0x01,0xa9,0xfd,0x43,0x00,0x91,0x13,0x04,0x40,0xf9};
    assert(research_site_matches(RESEARCH_PHASE_UPDATE,signature,16));signature[15]^=1;
    assert(!research_site_matches(RESEARCH_PHASE_UPDATE,signature,16));
    assert(!research_site_matches(-1,signature,16)&&!research_site_matches(0,signature,15)&&!research_site_matches(0,nullptr,16));
    ResearchInput script[3]={{1,{123,-456,0,0}},{2,{0,0,0,0}},{0,{0,0,0,0}}},out[3];
    auto tape=(root/"driver.lastape").u8string();
    assert(research_update_tape_write(tape.c_str(),id,script,3,why,sizeof why));
    assert(research_tape_read(tape.c_str(),id,out,3,why,sizeof why)<0);
    assert(research_update_tape_read(tape.c_str(),id,out,3,why,sizeof why)==3);
    auto hid=(root/"hid.lastape").u8string();assert(research_tape_write(hid.c_str(),id,script,3,why,sizeof why));
    assert(research_update_tape_read(hid.c_str(),id,out,3,why,sizeof why)<0);
    env("LARECOMP_RESEARCH_BOUNDARY","update");env("LARECOMP_RESEARCH_START_LEVEL","House");
    env("LARECOMP_RESEARCH_WARMUP","3");env("LARECOMP_RESEARCH_STABLE","2");env("LARECOMP_RESEARCH_UPDATES","3");
    env("LARECOMP_RESEARCH_DRIVER",tape.c_str());assert(!research_update_boot(id,tape.c_str(),why,sizeof why));
    env("LARECOMP_RESEARCH_UPDATES","2");assert(!research_update_boot(id,nullptr,why,sizeof why));env("LARECOMP_RESEARCH_UPDATES","3");
    auto oversized=(root/"oversized.lastape").u8string();std::vector<ResearchInput> long_script(12001);
    assert(research_update_tape_write(oversized.c_str(),id,long_script.data(),long_script.size(),why,sizeof why));
    env("LARECOMP_RESEARCH_UPDATES",nullptr);env("LARECOMP_RESEARCH_DRIVER",oversized.c_str());
    assert(!research_update_boot(id,nullptr,why,sizeof why)&&!research_update_active());
    env("LARECOMP_RESEARCH_UPDATES","3");env("LARECOMP_RESEARCH_DRIVER",tape.c_str());
    assert(research_update_boot(id,nullptr,why,sizeof why));
    ResearchPhaseSample point{};point.thread=1;point.observed=true;point.state=s;point.state.playing=false;point.delta=1.0f/60;
    auto begin=[&](){research_update_phase(true,&point,111,point.counter*1000000ull,1,2);};
    auto end=[&](){++point.counter;research_update_phase(false,&point,222,point.counter*1000000ull,1,2);};
    uint64_t buttons=4;int32_t axes[4]={};begin();research_update_input(&buttons,axes);assert(buttons==4);end();
    point.state.playing=true;point.state.position=false;begin();research_update_input(&buttons,axes);assert(buttons==0);end();
    point.state.position=true;
    for(int i=0;i<2;++i){begin();buttons=4;research_update_input(&buttons,axes);assert(buttons==0);end();}
    assert(!research_update_status().ready);
    for(int i=0;i<3;++i){
        begin();buttons=99;research_update_input(&buttons,axes);assert(buttons==script[i].buttons);
        if(i==0){assert(axes[1]==-456);buttons=9;research_update_input(&buttons,axes);assert(buttons==1&&axes[0]==123);}
        end();
    }
    auto status=research_update_status();assert(status.ready&&status.completed&&!status.integrity_error&&status.inputs==3&&status.source_consumed==3&&research_update_done());
    begin();buttons=3;research_update_input(&buttons,axes);assert(!buttons&&!axes[0]&&!axes[1]);end();
    auto export_path=root/"capture";fs::create_directory(export_path);
    assert(research_update_export(export_path.u8string().c_str(),id,why,sizeof why));
    assert(research_update_tape_read((export_path/"inputs.lastape").u8string().c_str(),id,out,3,why,sizeof why)==3);
    assert(out[0].buttons==1&&out[1].buttons==2);
    assert(read(export_path/"start-state.ini").find("counter=4\n")!=std::string::npos);
    assert(read(export_path/"update-summary.ini").find("completed=1\n")!=std::string::npos);
    env("LARECOMP_RESEARCH_DRIVER",nullptr);assert(research_update_boot(id,tape.c_str(),why,sizeof why));
    begin();point.counter+=2;research_update_phase(false,&point,0,0,0,0);
    assert(research_update_status().integrity_error&&research_update_done());
    env("LARECOMP_RESEARCH_WARMUP","0");env("LARECOMP_RESEARCH_STABLE","1");env("LARECOMP_RESEARCH_UPDATES","1");
    assert(research_update_boot(id,nullptr,why,sizeof why));begin();buttons=4;research_update_input(&buttons,axes);buttons=8;research_update_input(&buttons,axes);assert(buttons==4);end();
    assert(research_update_status().completed);
    assert(research_update_boot(id,nullptr,why,sizeof why));begin();research_update_phase(true,&point,0,0,0,0);assert(research_update_status().integrity_error);
    assert(research_update_boot(id,nullptr,why,sizeof why));begin();buttons=4;research_update_input(&buttons,axes);end();
    env("LARECOMP_RESEARCH_UPDATES","2");assert(research_update_boot(id,nullptr,why,sizeof why));begin();buttons=4;research_update_input(&buttons,axes);end();
    research_update_input(&buttons,axes);assert(research_update_status().integrity_error);
    env("LARECOMP_RESEARCH_WARMUP","oops");assert(!research_update_boot(id,nullptr,why,sizeof why));
    env("LARECOMP_RESEARCH_BOUNDARY","unknown");assert(!research_update_boot(id,nullptr,why,sizeof why));
    env("LARECOMP_RESEARCH_BOUNDARY","hid");assert(!research_update_boot(id,nullptr,why,sizeof why));
    for(auto key:{"LARECOMP_RESEARCH_BOUNDARY","LARECOMP_RESEARCH_START_LEVEL","LARECOMP_RESEARCH_WARMUP","LARECOMP_RESEARCH_STABLE","LARECOMP_RESEARCH_UPDATES"})env(key,nullptr);
    assert(research_update_boot(id,nullptr,why,sizeof why)&&!research_update_active());
    std::cout<<"Update gate, instruction guards, tape boundary, input latching, exact completion and integrity checks passed\n";
}
