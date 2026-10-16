// Developer-only observations and final HID-input playback. Never enabled for normal play.
#include "../research.h"
#include "../gameversion.h"
#include "../platform.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <string>
#include <stdexcept>
#include <vector>
namespace fs=std::filesystem;
namespace {
struct Row { ResearchInput input{}; RouteSample state{}; uint64_t frame,ns,timers,vsyncs,digest=0; uint32_t interval; bool observed=false; };
struct Camera { uint64_t frame,ns;std::array<float,48> matrices; };
std::mutex lock;
std::atomic<bool> enabled;
std::atomic<uint64_t> timer_calls,period{16666667};
std::atomic<uint32_t> last_interval;
std::vector<Row> rows;
std::vector<Camera> cameras;
std::vector<ResearchInput> replay;
std::string directory,build_id;
uint64_t start_ns;
size_t replay_at;
bool replaying,truncated,cameras_truncated,replay_exhausted;
thread_local size_t current_row=SIZE_MAX;
uint64_t digest(const void *data,size_t n) {
    uint64_t h=14695981039346656037ull;const unsigned char *p=(const unsigned char *)data;
    for(size_t i=0;i<n;++i) h=(h^p[i])*1099511628211ull;
    return h;
}
}
extern "C" volatile uint64_t g_vsync_count;
extern "C" bool research_active(void) { return enabled.load(std::memory_order_relaxed); }
extern "C" uint64_t research_vsync_period(void) { return period.load(std::memory_order_relaxed); }
extern "C" void research_timer(uint32_t interval) {
    if (!research_active()) return;
    last_interval=interval;timer_calls.fetch_add(1,std::memory_order_relaxed);
}
extern "C" bool research_boot(const char *root,const char *save,char *why,size_t cap) {
    const char *record=getenv("LARECOMP_RESEARCH_CAPTURE"),*tape=getenv("LARECOMP_RESEARCH_REPLAY"),*rate=getenv("LARECOMP_RESEARCH_HZ");
    if (!record&&!tape&&!rate) return true;
    try {
        if ((!record&&!tape)||(record&&tape)||!save||!*save) throw std::runtime_error("Research needs exactly one capture/replay mode and an isolated workshop save.");
        if(record&&strcmp(record,"1"))throw std::runtime_error("Research capture must be explicitly set to 1.");
        char id[41];
        if (!game_build_id((std::string(root)+"/exefs/main").c_str(),id)||game_version_id(id)==GAME_UNSUPPORTED)
            throw std::runtime_error("Research probes support verified LAS executables only.");
        int hz=60;
        if(rate) { if(strcmp(rate,"30")&&strcmp(rate,"60")&&strcmp(rate,"120")) throw std::runtime_error("Research frequency must be 30, 60 or 120 Hz.");hz=atoi(rate); }
        replay.clear();
        if(tape) {
            replay.resize(RESEARCH_MAX_INPUTS);char error[256];
            int count=research_tape_read(tape,id,replay.data(),replay.size(),error,sizeof error);
            if(count<0) throw std::runtime_error(error);replay.resize(count);
        }
        auto path=fs::u8path(save).parent_path()/"research";
        if(!fs::create_directory(path)) throw std::runtime_error("Research output directory already exists or is unavailable.");
        directory=path.u8string();build_id=id;replaying=tape!=nullptr;replay_at=0;
        truncated=cameras_truncated=replay_exhausted=false;rows.clear();cameras.clear();rows.reserve(16000);cameras.reserve(8000);
        timer_calls=0;last_interval=0;start_ns=plat_now_ns();period=(1000000000ull+hz/2)/hz;enabled=true;
        static bool registered;if(!registered){registered=true;atexit(research_finish);}
        fprintf(stderr,"[research] %s, %d Hz emulated vsync, isolated output=%s\n",replaying?"HID replay":"HID capture",hz,directory.c_str());
        return true;
    } catch(const std::exception &e) { if(cap)snprintf(why,cap,"%s",e.what());return false; }
}
extern "C" void research_input(uint64_t frame,uint64_t *buttons,int32_t axes[4]) {
    current_row=SIZE_MAX;if(!research_active())return;
    std::lock_guard<std::mutex> guard(lock);if(!research_active())return;
    if(replaying) {
        ResearchInput input{};
        if(replay_at<replay.size())input=replay[replay_at++];else replay_exhausted=true;
        *buttons=input.buttons;memcpy(axes,input.axes,sizeof input.axes);
    }
    if(rows.size()>=RESEARCH_MAX_INPUTS){truncated=true;return;}
    Row row{};row.input.buttons=*buttons;memcpy(row.input.axes,axes,sizeof row.input.axes);
    row.frame=frame;row.ns=plat_now_ns()-start_ns;row.timers=timer_calls.load();row.interval=last_interval.load();
    row.vsyncs=__atomic_load_n(&g_vsync_count,__ATOMIC_RELAXED);
    current_row=rows.size();rows.push_back(row);
}
extern "C" void research_state(const RouteSample *sample,const void *fields,size_t size) {
    if(!research_active()||current_row==SIZE_MAX)return;
    std::lock_guard<std::mutex> guard(lock);if(!research_active()||current_row>=rows.size())return;
    auto &row=rows[current_row];row.state=*sample;row.observed=true;
    row.digest=fields&&size?digest(fields,size):0;
}
extern "C" void research_camera(uint64_t frame,const float *matrices) {
    if(!research_active())return;
    for(int i=0;i<48;++i)if(!std::isfinite(matrices[i]))return;
    std::lock_guard<std::mutex> guard(lock);if(!research_active())return;
    if(!cameras.empty()&&cameras.back().frame==frame)return;
    if(cameras.size()>=12000){cameras_truncated=true;return;}
    Camera camera{};camera.frame=frame;camera.ns=plat_now_ns()-start_ns;
    memcpy(camera.matrices.data(),matrices,48*sizeof(float));cameras.push_back(camera);
}
extern "C" void research_finish(void) {
    if(!enabled.exchange(false))return;
    auto hz=(1000000000ull+period.load()/2)/period.load();period=16666667;
    std::lock_guard<std::mutex> guard(lock);
    try {
        auto root=fs::u8path(directory);std::vector<ResearchInput> inputs;inputs.reserve(rows.size());for(const auto &r:rows)inputs.push_back(r.input);
        char why[256];if(!research_tape_write((root/"inputs.lastape").u8string().c_str(),build_id.c_str(),inputs.data(),inputs.size(),why,sizeof why))throw std::runtime_error(why);
        std::ofstream trace(root/"observations.csv",std::ios::binary),camera(root/"camera.csv",std::ios::binary),meta(root/"summary.ini",std::ios::binary);
        for(auto *f:{&trace,&camera,&meta})f->imbue(std::locale::classic());trace<<std::setprecision(9);camera<<std::setprecision(9);
        trace<<"poll,frame,elapsed_ns,timer_calls,interval,vsyncs,observed,playing,level,hp,position,x,y,z,save_digest\n";
        for(size_t i=0;i<rows.size();++i){const auto &r=rows[i];const auto &s=r.state;
            trace<<i<<','<<r.frame<<','<<r.ns<<','<<r.timers<<','<<r.interval<<','<<r.vsyncs<<','<<r.observed<<','<<s.playing<<','<<s.level<<','<<s.hp<<','<<s.position<<','<<s.x<<','<<s.y<<','<<s.z<<','<<r.digest<<'\n';}
        camera<<"frame,elapsed_ns";for(const auto *prefix:{"v","p","vp"})for(int i=0;i<16;++i)camera<<','<<prefix<<i;camera<<'\n';
        for(const auto &c:cameras){camera<<c.frame<<','<<c.ns;for(float f:c.matrices)camera<<','<<f;camera<<'\n';}
        meta<<"format=1\nbuild_id="<<build_id<<"\nmode="<<(replaying?"replay":"capture")<<"\nvsync_hz="<<hz
            <<"\ninputs="<<rows.size()<<"\ncameras="<<cameras.size()<<"\nreplay_inputs="<<replay.size()<<"\nreplay_consumed="<<replay_at
            <<"\nreplay_eof="<<replay_exhausted<<"\ntruncated="<<truncated<<"\ncameras_truncated="<<cameras_truncated
            <<"\nstate_boundary=fullkey_hid_poll\ndigest=fnv1a64_live_save_5388_55e0\n";
        for(auto *f:{&trace,&camera,&meta}){f->close();if(!*f)throw std::runtime_error("Research export could not be completed.");}
        fprintf(stderr,"[research] saved %zu HID polls, %zu cameras, replay=%zu/%zu\n",rows.size(),cameras.size(),replay_at,replay.size());
    }catch(const std::exception &e){fprintf(stderr,"[research] export failed: %s\n",e.what());}
}
