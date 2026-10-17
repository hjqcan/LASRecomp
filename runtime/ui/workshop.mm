#include "../workshop.h"
#include "../coop.h"
#include "../game_profiles.h"
#include "../gameversion.h"
#include "../gamelib.h"
#include "../paths.h"
#include "../settings.h"
#include "../rando_profile.h"
#include "../platform.h"
#include "menu.h"
#include "localization.h"
#include "imgui.h"
#include "warp_data.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <vector>
extern "C" { void rt_request_reboot(int kind); }
namespace {
std::mutex lock, dialog_lock;
std::atomic<bool> observing, overlay;
std::atomic<bool> tool_used;
std::atomic<int> damage_override;
std::string selected_root, selected_seed, armed_root, armed_seed, run_dir, save_dir, message, pending_config, pending_route;
ChallengeConfig draft={"leave-house","1.0.0","Field",300,1,false,false}, config;
ChallengeState state;
bool armed,booted,dirty,assisted,draft_coop;
std::vector<RouteSample> route, replay;
uint64_t last_sample;
float replay_time;
bool replay_playing;
const char *status_name(int status) {
    const char *names[]={"Ready","Challenge armed","Waiting for gameplay","Challenge complete","Challenge failed","Challenge cancelled"};
    if (status==CHALLENGE_RUNNING) return "Challenge running";
    if (status==CHALLENGE_WAITING) return booted ? "Waiting for gameplay" : "Challenge armed";
    return names[std::clamp(status,0,5)];
}
void flush_run() {
    if (!dirty || run_dir.empty()) return;
    char why[512];
    if (workshop_route_write(run_dir.c_str(),route.data(),route.size(),&state,armed_seed.c_str(),assisted,why,sizeof why)) {
        dirty=false; message=std::string(ui_tr("Run saved:"))+" "+run_dir;
        fprintf(stderr,"[workshop] status=%d elapsed=%.3f samples=%zu saved=%s\n",state.status,state.elapsed,route.size(),run_dir.c_str());
    } else { dirty=false; message=why; fprintf(stderr,"[workshop] export failed: %s\n",why); }
}
void disarm() {
    if (armed) { state.status=CHALLENGE_CANCELLED; dirty=true; flush_run(); }
    armed=false; save_dir.clear();coop_request(false);
}
bool arm(char *why,size_t cap) {
    auto version=game_version_file((selected_root+"/exefs/main").c_str());
    if (strcmp(config.game_version,game_version_name(version))) { snprintf(why,cap,"Challenge belongs to a different game version."); return false; }
    if (!selected_seed.empty()&&!rando_seed_name_valid(selected_seed.c_str())) { snprintf(why,cap,"Invalid randomizer seed name."); return false; }
    int profile=game_profile_index(selected_root.c_str(),rt_data_root());
    std::string source=getenv("LARECOMP_SAVE_DIR") ? getenv("LARECOMP_SAVE_DIR") : game_profile_save_dir(profile);
    if (!getenv("LARECOMP_SAVE_DIR") && !selected_seed.empty()) source="rando/"+selected_seed+"/save";
    char directory[1024];
    if (!workshop_prepare_run(rt_data_root(),source.c_str(),&config,directory,sizeof directory,why,cap)) return false;
    run_dir=directory; save_dir=run_dir+"/save"; armed_root=selected_root; armed_seed=selected_seed;
    armed=true; route.clear(); route.reserve(12000); last_sample=0; assisted=false; tool_used=false; dirty=false; draft=config; challenge_begin(&state);
    coop_request(draft_coop&&!strcmp(config.game_version,"1.0.0")&&selected_seed.empty());
    return true;
}
void load_replay(const std::string &path) {
    std::vector<RouteSample> next(40000); char why[512];
    int count=workshop_route_read(path.c_str(),next.data(),next.size(),why,sizeof why);
    if (count<0) { message=why; return; }
    next.resize((size_t)count); replay=std::move(next); replay_time=0; replay_playing=false;
}
void SDLCALL picked_config(void *,const char *const *files,int) {
    if (files&&files[0]) { std::lock_guard<std::mutex> guard(dialog_lock); pending_config=files[0]; }
}
void SDLCALL picked_route(void *,const char *const *files,int) {
    if (files&&files[0]) { std::lock_guard<std::mutex> guard(dialog_lock); pending_route=files[0]; }
}
void replay_draw(float scale) {
    if (replay.empty()) return;
    float end=(float)(replay.back().now_ns/1e9);
    if (ImGui::Button(ui_label(replay_playing?"Pause route":"Play route").c_str())) replay_playing=!replay_playing;
    if (replay_playing) { replay_time=std::min(end,replay_time+ImGui::GetIO().DeltaTime); if (replay_time>=end) replay_playing=false; }
    ImGui::SliderFloat(ui_label("Route time").c_str(),&replay_time,0,end,"%.1f s");
    auto it=std::upper_bound(replay.begin(),replay.end(),(uint64_t)(replay_time*1e9),[](uint64_t ns,const RouteSample &s){return ns<s.now_ns;});
    size_t index=it==replay.begin()?0:(size_t)(it-replay.begin()-1); const auto &current=replay[index];
    ImGui::Text("%s   HP: %d",current.level,current.hp);
    if (current.position) ImGui::Text("X: %.2f   Z: %.2f",current.x,current.z);
    else ImGui::TextDisabled("%s",ui_tr("Position unavailable for this sample."));
    ImGui::Text(ui_tr("Buttons: 0x%llX   L: %d, %d   R: %d, %d"),(unsigned long long)current.buttons,current.lx,current.ly,current.rx,current.ry);
    ImVec2 size(std::max(100.f,ImGui::GetContentRegionAvail().x),280*scale),origin=ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##route-map",size); auto *draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,ImVec2(origin.x+size.x,origin.y+size.y),IM_COL32(16,25,30,255),8*scale);
    float x0=FLT_MAX,z0=FLT_MAX,x1=-FLT_MAX,z1=-FLT_MAX;
    for (const auto &s:replay) if (s.position&&!strcmp(s.level,current.level)) { x0=std::min(x0,s.x);x1=std::max(x1,s.x);z0=std::min(z0,s.z);z1=std::max(z1,s.z); }
    if (x0==FLT_MAX) return;
    float factor=std::min((size.x-40*scale)/std::max(2.f,x1-x0),(size.y-40*scale)/std::max(2.f,z1-z0));
    auto point=[&](const RouteSample &s){return ImVec2(origin.x+size.x/2+(s.x-(x0+x1)/2)*factor,origin.y+size.y/2+(s.z-(z0+z1)/2)*factor);};
    draw->PushClipRect(origin,ImVec2(origin.x+size.x,origin.y+size.y),true);
    for (size_t i=1;i<replay.size();++i) if (replay[i-1].position&&replay[i].position&&!strcmp(replay[i].level,current.level)&&!strcmp(replay[i-1].level,current.level))
        draw->AddLine(point(replay[i-1]),point(replay[i]),i<=index?IM_COL32(235,192,82,255):IM_COL32(100,120,130,100),2*scale);
    if (current.position) draw->AddCircleFilled(point(current),5*scale,IM_COL32(120,230,220,255));
    draw->PopClipRect();
}
}
extern "C" void workshop_selection(const char *root,const char *seed) {
    std::lock_guard<std::mutex> guard(lock);
    selected_root=root; selected_seed=getenv("LARECOMP_RANDO_SEED")?getenv("LARECOMP_RANDO_SEED"):seed;
    if (armed && (armed_root!=selected_root||armed_seed!=selected_seed)) {
        disarm(); message="Selection changed. Arm a new challenge snapshot.";
    }
}
extern "C" bool workshop_boot(const char *root,char *why,size_t cap) {
    std::lock_guard<std::mutex> guard(lock);
    const char *actual_seed=getenv("LARECOMP_RANDO_SEED")?getenv("LARECOMP_RANDO_SEED"):g_settings.rando_seed;
    static bool test_loaded;
    if (!test_loaded) if (const char *path=getenv("LARECOMP_CHALLENGE_PRESET")) {
        selected_root=root; selected_seed=actual_seed;
        if (!workshop_config_read(path,&config,why,cap)||!arm(why,cap)) return false;
        test_loaded=true;
    }
    if (!armed) return true;
    if (armed_root!=root||armed_seed!=actual_seed||strcmp(config.game_version,game_version_name(game_version_current()))) {
        snprintf(why,cap,"Game or seed changed after arming the challenge. Return to the launcher and arm it again."); return false;
    }
    armed=false; booted=true; observing=true; overlay=true; damage_override=config.ohko?999:config.damage;
    static bool exit_registered;
    if (!exit_registered) {
        exit_registered=true;
        atexit([]() {
            std::lock_guard<std::mutex> guard(lock); observing=false;
            if (booted && (state.status==CHALLENGE_WAITING||state.status==CHALLENGE_RUNNING)) { state.status=CHALLENGE_CANCELLED; dirty=true; }
            flush_run();
        });
    }
    fprintf(stderr,"[workshop] isolated save=%s goal=%s\n",save_dir.c_str(),config.goal_level); return true;
}
extern "C" const char *workshop_save_dir(void) { return booted?save_dir.c_str():NULL; }
extern "C" bool workshop_recording(void) { return observing.load(); }
extern "C" void workshop_mark_assisted(void) { if (observing.load()) tool_used=true; }
extern "C" int workshop_damage(int base) {
    int mult=damage_override.load(); if (!mult) return -1;
    if (mult==999) return 999;
    return (int)std::min((int64_t)base*mult,(int64_t)INT32_MAX);
}
extern "C" void workshop_observe(const RouteSample *sample) {
    if (!observing.load()) return;
    std::lock_guard<std::mutex> guard(lock); if (!observing.load()) return;
    int before=state.status; challenge_step(&state,&config,sample); assisted|=sample->assisted||tool_used.load();
    bool terminal=state.status==CHALLENGE_WON||state.status==CHALLENGE_LOST;
    if (sample->playing && route.size()<40000 && (sample->position||terminal||route.empty()||
        strcmp(sample->level,route.back().level)||sample->now_ns-last_sample>=200000000ull)) {
        route.push_back(*sample); last_sample=sample->now_ns;
    }
    if (before!=state.status) fprintf(stderr,"[workshop] status=%d level=%s elapsed=%.3f\n",state.status,sample->level,state.elapsed);
    if (terminal) { observing=false; dirty=true; }
}
extern "C" void workshop_frame(uint64_t) {
    if (!overlay.load()) return;
    std::lock_guard<std::mutex> guard(lock);
    challenge_clock(&state,&config,plat_now_ns());
    if (observing && state.status==CHALLENGE_LOST) { observing=false;dirty=true; }
    flush_run();
}
extern "C" void workshop_reset(void) {
    std::lock_guard<std::mutex> guard(lock);
    if (booted && (state.status==CHALLENGE_WAITING||state.status==CHALLENGE_RUNNING)) { state.status=CHALLENGE_CANCELLED; dirty=true; }
    flush_run(); observing=false;overlay=false;damage_override=0;booted=false;save_dir.clear();coop_request(false);
}
extern "C" bool workshop_overlay_wanted(void) { return overlay.load(); }
extern "C" void workshop_overlay(float scale) {
    std::lock_guard<std::mutex> guard(lock);
    ImVec2 display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x-20*scale,display.y-20*scale),ImGuiCond_Always,ImVec2(1,1));
    ImGui::SetNextWindowBgAlpha(.75f);
    ImGui::Begin("##challenge-status",NULL,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoFocusOnAppearing);
    if(coop_active())ImGui::TextUnformatted(ui_tr("Local co-op: BowWow is controlled by player 2."));
    ImGui::Text("%s",ui_tr(status_name(state.status))); ImGui::Text("%s: %.1f / %d s",config.name,state.elapsed,config.seconds);
    ImGui::Text("%s: %s",ui_tr("Goal level"),config.goal_level); ImGui::End();
}
extern "C" void workshop_draw(float scale,bool in_game) {
    std::lock_guard<std::mutex> guard(lock);
    std::string incoming,route_file;
    { std::lock_guard<std::mutex> dialog_guard(dialog_lock); incoming.swap(pending_config);route_file.swap(pending_route); }
    static bool test_route_loaded;
    const char *test_route=getenv("LARECOMP_WORKSHOP_ROUTE");
    if (test_route&&!test_route_loaded) {
        route_file=!strcmp(test_route,"@latest") ? run_dir+"/route.csv" : test_route;
        test_route_loaded=true;
    }
    bool locked=in_game||gamelib_tables()||armed;
    if (!incoming.empty()&&!locked) { char why[512]; if (!workshop_config_read(incoming.c_str(),&draft,why,sizeof why)) message=why; }
    if (!route_file.empty()) load_replay(route_file);
    ImGui::TextWrapped("%s",ui_tr("Create a timed journey to a level. Arm it before starting the game: your selected saves are copied into an isolated run. Reach the goal before time expires; death ends the attempt."));
    ImGui::TextWrapped("%s",ui_tr("The timer starts when a gameplay level loads and includes dialogue, loading, menus and photo mode. Attempts are for practice; saves, settings and tools are not competitively verified."));
    bool show_rules=ImGui::CollapsingHeader(ui_label("Challenge rules").c_str(),in_game||test_route?0:ImGuiTreeNodeFlags_DefaultOpen);
    if (show_rules) {
    ImGui::BeginDisabled(locked);
    ImGui::SetNextItemWidth(300*scale); ImGui::InputText(ui_label("Challenge name").c_str(),draft.name,sizeof draft.name);
    const char *versions[]={"1.0.0","1.1.0"};
    if (ImGui::BeginCombo(ui_label("Game version").c_str(),draft.game_version)) {
        for (auto v:versions) if (ImGui::Selectable(v,!strcmp(v,draft.game_version))) snprintf(draft.game_version,sizeof draft.game_version,"%s",v);
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo(ui_label("Goal level").c_str(),draft.goal_level)) {
        std::set<std::string> levels; for (const auto &spot:kWarpSpots) levels.insert(spot.level);
        for (const auto &level:levels) if (ImGui::Selectable(level.c_str(),level==draft.goal_level)) snprintf(draft.goal_level,sizeof draft.goal_level,"%s",level.c_str());
        ImGui::EndCombo();
    }
    ImGui::SliderInt(ui_label("Time limit (seconds)").c_str(),&draft.seconds,10,3600);
    ImGui::SliderInt(ui_label("Challenge damage multiplier").c_str(),&draft.damage,1,8);
    ImGui::Checkbox(ui_label("Fail on health loss").c_str(),&draft.no_hit); ImGui::SameLine(); ImGui::Checkbox(ui_label("Challenge one-hit KO").c_str(),&draft.ohko);
    ImGui::BeginDisabled(strcmp(draft.game_version,"1.0.0")||!selected_seed.empty());
    ImGui::Checkbox(ui_label("Local companion co-op (1.0.0 prototype)").c_str(),&draft_coop);
    ImGui::EndDisabled();
    if(draft_coop)ImGui::TextWrapped("%s",ui_tr("Player 1: normal controls. Player 2: second gamepad (left stick / south button), or numpad 8/4/5/6 and 0. BowWow uses the game's companion combat rules. Shared camera and the host's isolated save."));
    if (ImGui::Button(ui_label("Save challenge").c_str())) {
        char why[512];
        auto path=std::filesystem::u8path(rt_data_root())/"workshop"/"presets"/(std::string(draft.name)+".laschallenge");
        message=workshop_config_write(path.u8string().c_str(),&draft,why,sizeof why)?"Challenge saved.":why;
    }
    ImGui::SameLine();
    if (ImGui::Button(ui_label("Load challenge").c_str())) {
        static const SDL_DialogFileFilter filter[]={{"Challenge","laschallenge"}};
        SDL_ShowOpenFileDialog(picked_config,NULL,SDL_GetKeyboardFocus(),filter,1,NULL,false);
    }
    if (ImGui::Button(ui_label("Arm isolated attempt").c_str())) {
        config=draft; char why[512]; message=arm(why,sizeof why)?"Challenge armed. Return to the launcher and press Play.":why;
    }
    ImGui::EndDisabled();
    }
    if (armed && ImGui::Button(ui_label("Disarm challenge").c_str())) disarm();
    if (booted) {
        if(coop_active())ImGui::TextUnformatted(ui_tr("Local co-op: BowWow is controlled by player 2."));
        ImGui::Text("%s  %.1f s",ui_tr(status_name(state.status)),state.elapsed);
        ImGui::Text("%s: %s",ui_tr("Goal level"),config.goal_level);
        if (state.reason) {
            const char *reasons[]={"", "Time limit reached", "No health remaining", "Health was lost"};
            ImGui::TextUnformatted(ui_tr(reasons[state.reason]));
        }
        if (ImGui::Button(ui_label("Finish and return to launcher").c_str())) rt_request_reboot(2);
        ImGui::TextWrapped("%s",ui_tr("This run keeps writing only to its isolated save until you return to the launcher."));
    } else if (gamelib_tables()) ImGui::TextWrapped("%s",ui_tr("Restart LAS Recomp before arming another challenge."));
    if (!message.empty()) ui_user_text(ui_tr(message.c_str()),true);
    ImGui::Separator(); ImGui::TextUnformatted(ui_tr("Route replay"));
    ImGui::TextWrapped("%s",ui_tr("Replay the observed route, health and controller samples on a 2D map. This viewer does not drive the game or reproduce its simulation. Share route.csv separately from the run's save folder."));
    if (ImGui::Button(ui_label("Open route CSV").c_str())) {
        static const SDL_DialogFileFilter filter[]={{"Route CSV","csv"}};
        SDL_ShowOpenFileDialog(picked_route,NULL,SDL_GetKeyboardFocus(),filter,1,NULL,false);
    }
    if (!run_dir.empty()&&!observing) {
        ImGui::SameLine(); if (ImGui::Button(ui_label("View latest route").c_str())) load_replay(run_dir+"/route.csv");
    }
    replay_draw(scale);
}
