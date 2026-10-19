#include "../performance.h"
#include "../photo.h"
#include "../gameversion.h"
#include "../settings.h"
#include "../paths.h"
#include "../platform.h"
#include "menu.h"
#include "localization.h"
#include "imgui.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
extern "C" int game_in_play(void);
static std::vector<double> samples;
static std::vector<uint64_t> frame_ids;
static bool recording, export_pending;
static int duration=60;
static uint64_t previous, last_stat;
static double elapsed;
static FrameStats stats;
static std::string message;
static void start() {
    samples.clear(); frame_ids.clear(); samples.reserve(18000); frame_ids.reserve(18000);
    previous=last_stat=0; elapsed=0; stats={}; message.clear(); recording=true; export_pending=false;
}
static void export_capture() {
    export_pending=false;
    if (samples.empty()) { message="No gameplay frames were captured."; return; }
    namespace fs=std::filesystem;
    try {
        fs::path root=fs::absolute(fs::u8path(rt_data_root()))/"benchmarks", part;
        for (const auto &c:root) {
            part/=c; if (fs::is_symlink(fs::symlink_status(part))) throw std::runtime_error("Benchmark paths cannot contain links.");
#ifdef _WIN32
            auto a=GetFileAttributesW(part.c_str());
            if (a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("Benchmark paths cannot contain reparse points.");
#endif
        }
        fs::create_directories(root);
        auto dir=root/("capture-"+std::to_string(plat_now_ns()));
        if (!fs::create_directory(dir)) throw std::runtime_error("Cannot create benchmark folder.");
        frame_stats(samples.data(),samples.size(),&stats);
        std::ofstream csv(dir/"frames.csv",std::ios::binary), info(dir/"summary.ini",std::ios::binary);
        csv.imbue(std::locale::classic()); info.imbue(std::locale::classic());
        csv << "frame,interval_ms\n" << std::fixed << std::setprecision(6);
        for (size_t i=0;i<samples.size();++i) csv << frame_ids[i] << ',' << samples[i] << '\n';
        info << "format=1\nmetric=present_interval\ngame_version=" << game_version_name(game_version_current())
             << "\nseed=" << g_settings.rando_seed << "\nframes=" << stats.count << "\naverage_fps=" << stats.fps
             << "\none_percent_low_fps=" << stats.low_1pct_fps << "\nmedian_ms=" << stats.median_ms
             << "\np95_ms=" << stats.p95_ms << "\np99_ms=" << stats.p99_ms << "\nmax_ms=" << stats.max_ms
             << "\nover_16_667_ms=" << stats.over_16ms << "\nover_33_333_ms=" << stats.over_33ms
             << "\nexcluded=launcher,overlay_menu,photo_mode\n";
        csv.close(); info.close();
        if (!csv || !info) throw std::runtime_error("Could not finish benchmark export.");
        message=std::string(ui_tr("Benchmark saved:"))+" "+dir.u8string();
        fprintf(stderr,"[benchmark] %s (%zu frames)\n",dir.u8string().c_str(),samples.size());
    } catch (const std::exception &e) { message=e.what(); }
}
extern "C" void performance_frame(uint64_t frame) {
    static bool test_started;
    if (!test_started) if (const char *at=getenv("LARECOMP_BENCHMARK_AT")) if (frame>=strtoull(at,NULL,10)) {
        test_started=true; duration=10; start();
    }
    uint64_t now=plat_now_ns();
    if (recording) {
        bool eligible=game_in_play() && !menu_is_open() && !photo_mode_active();
        if (eligible && previous && now>previous) {
            double ms=(now-previous)/1e6; samples.push_back(ms); frame_ids.push_back(frame); elapsed+=ms/1000;
            if (elapsed>=duration || samples.size()>=60000) { recording=false; export_pending=true; }
        }
        previous=eligible ? now : 0;
        if (now-last_stat>=1000000000ull || !recording) { frame_stats(samples.data(),samples.size(),&stats); last_stat=now; }
    }
    if (export_pending) export_capture();
}
extern "C" void performance_reset(void) {
    if (recording) { recording=false; export_pending=true; }
    previous=0;
    if (export_pending) export_capture();
}
extern "C" void performance_draw(float scale,bool in_game) {
    ImGui::TextWrapped("%s",ui_tr("Capture presentation intervals during gameplay. Menus and photo mode are excluded. This measures delivered frames, not GPU execution time or game simulation speed."));
    ImGui::BeginDisabled(recording);
    ImGui::SetNextItemWidth(240*scale); ImGui::SliderInt(ui_label("Capture seconds").c_str(),&duration,10,300);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!in_game || recording);
    if (ImGui::Button(ui_label("Start benchmark").c_str())) { start(); menu_close(); }
    ImGui::EndDisabled();
    if (recording) {
        ImGui::SameLine(); if (ImGui::Button(ui_label("Stop and export").c_str())) { recording=false; previous=0; export_pending=true; }
        ImGui::ProgressBar((float)(elapsed/duration));
    }
    if (stats.count) {
        ImGui::Text(ui_tr("Average %.1f FPS   1%% low %.1f FPS"),stats.fps,stats.low_1pct_fps);
        ImGui::Text(ui_tr("Median %.2f ms   p95 %.2f ms   p99 %.2f ms   Max %.2f ms"),stats.median_ms,stats.p95_ms,stats.p99_ms,stats.max_ms);
        std::vector<float> plot; size_t begin=samples.size()>600 ? samples.size()-600 : 0;
        for (size_t i=begin;i<samples.size();++i) plot.push_back((float)samples[i]);
        ImGui::PlotLines("##frametime",plot.data(),(int)plot.size(),0,"ms",0,FLT_MAX,ImVec2(0,150*scale));
    }
    ImGui::TextWrapped("%s",ui_tr("The 1% low uses the mean of the slowest 1% of intervals. CSV and summary files are saved in benchmarks. The game keeps its existing timing; this does not enable 120 FPS simulation."));
    if (!message.empty()) ui_user_text(message.c_str(),true);
}
