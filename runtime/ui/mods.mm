#include "../mods.h"
#include "../game_profiles.h"
#include "../gameversion.h"
#include "../gamelib.h"
#include "../paths.h"
#include "../save_backup.h"
#include "../settings.h"
#include "imgui.h"
#include "localization.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

static std::string game_root = "extracted", selected;
static std::mutex lock;
static std::string pending, pending_preset, message;
static std::atomic<bool> busy, changed{true};
static ModInfo mods[MODS_MAX];
static int count;
static ModPreset presets[64];
static int preset_count;
static std::string preset_selected;
static char preset_name[64];
extern "C" void mods_set_game_root(const char *root) {
    game_root = root; selected.clear(); changed = true;
}
static void SDLCALL picked_mod(void *, const char *const *files, int) {
    if (files && files[0]) { std::lock_guard<std::mutex> guard(lock); pending = files[0]; }
}
static void SDLCALL picked_preset(void *, const char *const *files, int) {
    if (files && files[0]) { std::lock_guard<std::mutex> guard(lock); pending_preset = files[0]; }
}
static bool backup_before_change(int profile, const char *version, std::string &error) {
    namespace fs = std::filesystem;
    std::string base = getenv("LARECOMP_SAVE_DIR") ? getenv("LARECOMP_SAVE_DIR") : game_profile_save_dir(profile);
    if (!getenv("LARECOMP_SAVE_DIR") && *g_settings.rando_seed) base = std::string("rando/") + g_settings.rando_seed + "/save";
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(base + "/save_0"), ec) || fs::is_empty(fs::u8path(base + "/save_0"), ec)) return true;
    char why[512];
    if (save_backup_create((base + "/save_0").c_str(), "Before mod change", version, true, why, sizeof why)) return true;
    error = why; return false;
}
extern "C" void mods_draw(float scale, bool in_game) {
    int profile = game_profile_index(game_root.c_str(), rt_data_root());
    GameVersion game = game_version_file((game_root + "/exefs/main").c_str());
    const char *version = game_version_name(game);
    bool locked = in_game || gamelib_tables() || profile < 0 || game == GAME_UNSUPPORTED;
    std::string source, preset_source;
    { std::lock_guard<std::mutex> guard(lock); if (!locked && !busy) { source.swap(pending); preset_source.swap(pending_preset); } }
    if (!preset_source.empty()) {
        char why[512] = ""; bool ok = mods_preset_import(rt_data_root(),preset_source.c_str(),why,sizeof why);
        std::lock_guard<std::mutex> guard(lock); message = ok ? "Preset imported." : why; changed = true;
    }
    if (!source.empty()) {
        busy = true;
        std::string data = rt_data_root(), root = game_root, v = version;
        std::thread([data,root,v,source]() {
            char why[512]; bool ok = mods_import(data.c_str(), root.c_str(), v.c_str(), source.c_str(), why, sizeof why);
            { std::lock_guard<std::mutex> guard(lock); message = ok ? "Mod imported. Enable it to use its resources." : why; }
            changed = true; busy = false;
        }).detach();
    }
    ImGui::TextWrapped("%s", ui_tr("Mods replace resources without changing the installed game files. Enabled mods are saved separately for each installation."));
    ImGui::TextWrapped("%s", ui_tr("Randomizer seed resources take priority. Two enabled mods cannot replace the same file."));
    if (locked) ImGui::TextWrapped("%s", ui_tr("Choose an installed version and restart Multi before changing mods."));
    ImGui::BeginDisabled(locked || busy);
    if (ImGui::Button(ui_label("Import mod folder").c_str())) SDL_ShowOpenFolderDialog(picked_mod, NULL, SDL_GetKeyboardFocus(), NULL, false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ui_label("Refresh mods").c_str())) changed = true;
    if (busy) ImGui::TextWrapped("%s", ui_tr("Importing mod resources..."));
    if (!busy && changed.exchange(false)) {
        char why[512] = "";
        count = profile < 0 ? 0 : mods_list(rt_data_root(), game_root.c_str(), profile, version, mods, MODS_MAX, why, sizeof why);
        if (count < 0) { count = 0; std::lock_guard<std::mutex> guard(lock); message = why; }
        preset_count = mods_preset_list(rt_data_root(),presets,64,why,sizeof why);
        if (preset_count < 0) { preset_count = 0; std::lock_guard<std::mutex> guard(lock); message = why; }
    }
    std::string shown;
    { std::lock_guard<std::mutex> guard(lock); shown = message; }
    if (!shown.empty()) ImGui::TextWrapped("%s", ui_tr(shown.c_str()));
    if (ImGui::CollapsingHeader(ui_label("Mod presets").c_str())) {
        ImGui::TextWrapped("%s",ui_tr("Save and share a .lasmods file from mod-presets. It contains mod IDs and versions, without resource files. Applying checks every mod before replacing the current selection."));
        ImGui::BeginDisabled(locked || busy);
        ImGui::SetNextItemWidth(240*scale);
        ImGui::InputText(ui_label("Preset name").c_str(),preset_name,sizeof preset_name);
        if (ImGui::Button(ui_label("Save current selection").c_str())) {
            char why[512] = "";
            bool ok = mods_preset_save(rt_data_root(),game_root.c_str(),profile,version,preset_name,why,sizeof why);
            std::lock_guard<std::mutex> guard(lock); message = ok ? "Preset saved." : why; changed = true;
            if (ok) preset_selected = preset_name;
        }
        ImGui::SameLine();
        if (ImGui::Button(ui_label("Import preset").c_str())) {
            static const SDL_DialogFileFilter filters[] = {{"Mod preset", "lasmods"}};
            SDL_ShowOpenFileDialog(picked_preset,NULL,SDL_GetKeyboardFocus(),filters,1,NULL,false);
        }
        ImGui::SetNextItemWidth(340*scale);
        if (ImGui::BeginCombo(ui_label("Saved presets").c_str(),preset_selected.c_str())) {
            for (int i = 0; i < preset_count; ++i) {
                std::string label = std::string(presets[i].name)+" ("+presets[i].game_version+")";
                if (ui_user_selectable(label.c_str(),presets[i].name,preset_selected == presets[i].name)) preset_selected = presets[i].name;
            }
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(preset_selected.empty());
        if (ImGui::Button(ui_label("Apply preset").c_str())) {
            char why[512] = ""; std::string error;
            bool ok = backup_before_change(profile,version,error);
            if (ok) ok = mods_preset_apply(rt_data_root(),game_root.c_str(),profile,version,preset_selected.c_str(),why,sizeof why);
            std::lock_guard<std::mutex> guard(lock);
            message = ok ? "Mod selection saved. Changes apply when you start the game." : error.empty() ? why : error;
            changed = true;
        }
        ImGui::EndDisabled(); ImGui::EndDisabled();
    }
    if (!count) ImGui::TextDisabled("%s", ui_tr("No mods installed. Choose a folder containing mod.ini and romfs."));
    if (ImGui::BeginTable("##mods", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn(ui_tr("Enabled"), ImGuiTableColumnFlags_WidthFixed, 100 * scale);
        ImGui::TableSetupColumn(ui_tr("Mod"));
        ImGui::TableSetupColumn(ui_tr("Status"), ImGuiTableColumnFlags_WidthFixed, 250 * scale);
        ImGui::TableHeadersRow();
        for (int i = 0; i < count; ++i) {
            ModInfo &m = mods[i]; ImGui::PushID(m.id); ImGui::TableNextRow(); ImGui::TableNextColumn();
            bool on = m.enabled;
            ImGui::BeginDisabled(locked || busy || (!on && (!m.compatible || *m.problem || m.conflicts)));
            if (ImGui::Checkbox("##enabled", &on)) {
                char why[512] = ""; std::string error;
                bool ok = backup_before_change(profile, version, error);
                if (ok) ok = mods_enable(rt_data_root(), game_root.c_str(), profile, version, m.id, on, why, sizeof why);
                std::lock_guard<std::mutex> guard(lock);
                message = ok ? "Mod selection saved. Changes apply when you start the game." : error.empty() ? why : error;
                changed = true;
            }
            ImGui::EndDisabled(); ImGui::TableNextColumn();
            if (ui_user_selectable(m.name, "select", selected == m.id)) selected = m.id;
            ImGui::TableNextColumn();
            const char *status = *m.problem ? "Invalid mod" : !m.compatible ? "Different game version" : m.conflicts ? "File conflict" : m.enabled ? "Enabled" : "Disabled";
            ImGui::TextUnformatted(ui_tr(status)); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    for (int i = 0; i < count; ++i) if (selected == mods[i].id) {
        const auto &m = mods[i]; ImGui::Separator(); ui_user_text(m.name);
        ui_user_text((std::string(ui_tr("Version")) + ": " + m.version + "   " + ui_tr("Author") + ": " + m.author).c_str(), true);
        ui_user_text(m.description, true);
        ImGui::Text(ui_tr("%d replacement files, %.1f MiB"), m.files, (double)m.bytes/(1024*1024));
        if (m.conflicts) ImGui::TextWrapped("%s", ui_tr("Another enabled mod replaces these resources. Disable it before enabling this mod."));
        if (*m.conflict_detail) ui_user_text(m.conflict_detail,true);
        if (*m.problem) ImGui::TextWrapped("%s", ui_tr(m.problem));
    }
}
