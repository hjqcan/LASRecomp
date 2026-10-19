#include "workshop.h"
#include "performance.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
namespace fs=std::filesystem;
static void write(const fs::path &p,const std::string &s) { fs::create_directories(p.parent_path()); std::ofstream(p,std::ios::binary)<<s; }
static std::string read(const fs::path &p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
int main(int argc,char **argv) {
    assert(argc==2); fs::path root=fs::u8path(argv[1]); char why[512],run[1024];
    ChallengeConfig c={"test-route","1.1.0","Field",60,2,true,false}; assert(challenge_config_valid(&c));
    ChallengeState s; challenge_begin(&s); RouteSample o{}; o.now_ns=1000000000; o.hp=12;
    challenge_step(&s,&c,&o); assert(s.status==CHALLENGE_WAITING);
    o.playing=true; strcpy(o.level,"MarinTarinHouse"); challenge_step(&s,&c,&o); assert(s.status==CHALLENGE_RUNNING);
    o.now_ns+=2000000000; strcpy(o.level,"Field"); challenge_step(&s,&c,&o); assert(s.status==CHALLENGE_WON&&s.elapsed==2);
    o.hp=0; challenge_step(&s,&c,&o); assert(s.status==CHALLENGE_WON); // terminal result cannot change
    challenge_begin(&s); o.hp=12; strcpy(o.level,"House"); challenge_step(&s,&c,&o);
    o.hp=11; challenge_step(&s,&c,&o); assert(s.status==CHALLENGE_LOST&&s.reason==CHALLENGE_HIT);
    c.no_hit=false; challenge_begin(&s); challenge_step(&s,&c,&o); o.hp=0; challenge_step(&s,&c,&o); assert(s.reason==CHALLENGE_HEALTH);
    challenge_begin(&s); o.hp=12; challenge_step(&s,&c,&o); challenge_clock(&s,&c,o.now_ns+60000000000ull); assert(s.reason==CHALLENGE_TIME);
    auto path=root/"one.laschallenge";
    assert(workshop_config_write(path.u8string().c_str(),&c,why,sizeof why));
    assert(!workshop_config_write(path.u8string().c_str(),&c,why,sizeof why));
    ChallengeConfig restored{}; assert(workshop_config_read(path.u8string().c_str(),&restored,why,sizeof why)); assert(restored.seconds==60&&restored.damage==2&&!restored.no_hit);
    auto original=read(path);
    for (auto extra : {"seconds=30\n","unknown=1\n", "\nformat=2\n"}) { write(path,original+extra); assert(!workshop_config_read(path.u8string().c_str(),&restored,why,sizeof why)); }
    write(path,original); c.seconds=0; assert(!challenge_config_valid(&c)); c.seconds=60;
    strcpy(c.name,"../escape"); assert(!workshop_config_write((root/"escape.laschallenge").u8string().c_str(),&c,why,sizeof why)); strcpy(c.name,"test-route");
    auto source=root/"source",data=root/"data";
    write(source/"save_0/slot.bin","unchanged save");write(source/"save_1/state.bin","system save");write(source/"save_0.backups/private.bin","backup");
    assert(workshop_prepare_run(data.u8string().c_str(),source.u8string().c_str(),&c,run,sizeof run,why,sizeof why));
    auto output=fs::u8path(run); assert(read(output/"save/save_0/slot.bin")=="unchanged save"); assert(!fs::exists(output/"save/save_0.backups"));
    write(output/"save/save_0/slot.bin","changed run"); assert(read(source/"save_0/slot.bin")=="unchanged save");
    assert(!workshop_prepare_run(data.u8string().c_str(),source.u8string().c_str(),&c,run,4,why,sizeof why));
    RouteSample samples[3]{}; for (int i=0;i<3;++i) { samples[i].now_ns=1000000000ull+i*100000000ull; samples[i].frame=i; samples[i].x=(float)i;samples[i].hp=12;samples[i].position=true;strcpy(samples[i].level,"Field"); }
    s={};s.start_ns=1000000000;s.status=CHALLENGE_WON;s.elapsed=.2;
    assert(workshop_route_write(output.u8string().c_str(),samples,3,&s,"test-seed",false,why,sizeof why));
    RouteSample loaded[8]; auto csv=output/"route.csv";
    assert(workshop_route_read(csv.u8string().c_str(),loaded,8,why,sizeof why)==3);
    assert(loaded[2].now_ns==200000000&&loaded[2].x==2&&loaded[2].position);
    assert(workshop_route_read(csv.u8string().c_str(),loaded,2,why,sizeof why)<0);
    auto good=read(csv);
    for (const char *row : {"-1,0,Field,0,0,0,12,0,0,0,0,0,1\n", "0,0,Field,nan,0,0,12,0,0,0,0,0,1\n", "0,0,../bad,0,0,0,12,0,0,0,0,0,1\n", "0,0,Field,0,0,0,12,0,-2147483648,0,0,0,1\n"}) {
        write(csv,good.substr(0,good.find('\n')+1)+row); assert(workshop_route_read(csv.u8string().c_str(),loaded,8,why,sizeof why)<0);
    }
    write(csv,good+"1,4,Field,0,0,0,12,0,0,0,0,0,1\n");assert(workshop_route_read(csv.u8string().c_str(),loaded,8,why,sizeof why)<0);
    std::error_code ec; fs::create_directory_symlink(source,root/"linked",ec);
    if (!ec) assert(!workshop_prepare_run(data.u8string().c_str(),(root/"linked").u8string().c_str(),&c,run,sizeof run,why,sizeof why));
    double frames[100]; for (auto &f:frames) f=10;frames[99]=100;
    FrameStats stats;assert(frame_stats(frames,100,&stats));assert(stats.median_ms==10&&stats.p95_ms==10&&stats.p99_ms==10&&stats.max_ms==100);
    assert(fabs(stats.mean_ms-10.9)<1e-9&&stats.low_1pct_fps==10&&stats.over_33ms==1);
    assert(!frame_stats(frames,0,&stats));frames[0]=std::numeric_limits<double>::quiet_NaN();assert(!frame_stats(frames,100,&stats));
    double two[]={10,30};assert(frame_stats(two,2,&stats)&&stats.median_ms==20&&fabs(stats.low_1pct_fps-1000.0/30)<1e-9);
    std::cout<<"Challenge rules, preset validation, isolated saves, route round-trip, malformed routes and frame statistics passed\n";
}
