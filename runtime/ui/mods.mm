#include "../mods.h"
#include "../gamelib.h"
#include "../paths.h"
#include "imgui.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

static std::string game_root = "extracted", selected;
static std::mutex lock;
static std::string pending, message;
static std::atomic<bool> busy, changed{true};
static ModInfo mods[MODS_MAX];
static int count;

extern "C" void mods_set_game_root(const char *root) {
    game_root = root;
    selected.clear();
    changed = true;
}
static void SDLCALL picked_mod(void *, const char *const *files, int) {
    std::lock_guard<std::mutex> guard(lock);
    if (!files) message = std::string("Could not open the folder picker: ") + SDL_GetError();
    else if (files[0]) pending = files[0];
}
extern "C" void mods_draw(float scale, bool in_game) {
    std::error_code ec;
    bool ready = std::filesystem::is_directory(std::filesystem::u8path(game_root)/"romfs", ec);
    bool locked = in_game || gamelib_tables() || !ready;
    std::string source;
    { std::lock_guard<std::mutex> guard(lock); if (!locked && !busy) source.swap(pending); }
    if (!source.empty()) {
        busy = true;
        std::string data = rt_data_root(), root = game_root;
        std::thread([data,root,source]() {
            char why[512];
            bool ok = mods_import(data.c_str(), root.c_str(), source.c_str(), why, sizeof why);
            { std::lock_guard<std::mutex> guard(lock); message = ok ? "Mod imported. Enable it to use its resources." : why; }
            changed = true;
            busy = false;
        }).detach();
    }
    ImGui::TextWrapped("Mods replace resources without changing the installed game files. Changes apply when you start the game.");
    ImGui::TextWrapped("Randomizer seed resources take priority. Two enabled mods cannot replace the same file.");
    if (locked) ImGui::TextWrapped("Set up your game files and restart LAS Recomp before changing mods.");
    ImGui::BeginDisabled(locked || busy);
    if (ImGui::Button("Import mod folder")) SDL_ShowOpenFolderDialog(picked_mod, NULL, SDL_GetKeyboardFocus(), NULL, false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh mods")) changed = true;
    if (busy) ImGui::TextUnformatted("Importing mod resources...");
    if (!busy && changed.exchange(false)) {
        char why[512] = "";
        count = mods_list(rt_data_root(), game_root.c_str(), mods, MODS_MAX, why, sizeof why);
        if (count < 0) { count = 0; std::lock_guard<std::mutex> guard(lock); message = why; }
    }
    std::string shown;
    { std::lock_guard<std::mutex> guard(lock); shown = message; }
    if (!shown.empty()) ImGui::TextWrapped("%s", shown.c_str());
    if (!count) ImGui::TextDisabled("No mods installed. Choose a folder containing mod.ini and romfs.");
    if (ImGui::BeginTable("##mods", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 110 * scale);
        ImGui::TableSetupColumn("Mod");
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 220 * scale);
        ImGui::TableHeadersRow();
        for (int i = 0; i < count; ++i) {
            ModInfo &m = mods[i];
            ImGui::PushID(m.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool on = m.enabled;
            ImGui::BeginDisabled(locked || busy || (!on && (*m.problem || m.conflicts)));
            if (ImGui::Checkbox("##enabled", &on)) {
                char why[512] = "";
                bool ok = mods_enable(rt_data_root(), game_root.c_str(), m.id, on, why, sizeof why);
                std::lock_guard<std::mutex> guard(lock);
                message = ok ? "Mod selection saved. Changes apply when you start the game." : why;
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImVec2 position = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##select", selected == m.id)) selected = m.id;
            ImGui::GetWindowDrawList()->AddText(position, ImGui::GetColorU32(ImGuiCol_Text), m.name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(*m.problem ? "Invalid mod" : m.conflicts ? "File conflict" : m.enabled ? "Enabled" : "Disabled");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    for (int i = 0; i < count; ++i) if (selected == mods[i].id) {
        const auto &m = mods[i];
        ImGui::Separator();
        ImGui::TextUnformatted(m.name);
        ImGui::TextWrapped("Version: %s   Author: %s", m.version, m.author);
        ImGui::TextWrapped("%s", m.description);
        ImGui::Text("%d replacement files, %.1f MiB", m.files, (double)m.bytes/(1024*1024));
        if (m.conflicts) ImGui::TextWrapped("Another enabled mod replaces these resources. Disable it before enabling this mod.");
        if (*m.problem) ImGui::TextWrapped("%s", m.problem);
    }
}
