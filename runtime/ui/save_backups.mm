#include "imgui.h"
#include "localization.h"
#include "../save_backup.h"
#include "../gameversion.h"
#include "../gamelib.h"
#include "../game_profiles.h"
#include "../rando_profile.h"
#include <filesystem>
#include <algorithm>
#include <string>
#include <vector>
extern "C" {
#include "../settings.h"
void save_editor_draw(float scale);
int game_in_play(void);
}

static std::string game_main = "extracted/exefs/main";
extern "C" void save_backups_set_game_root(const char *root) { game_main = std::string(root) + "/exefs/main"; }
static const char *backup_version(int installation) {
    std::string main = installation == GAME_PROFILE_EXISTING ? "extracted/exefs/main" : std::string(game_profile_dir(installation)) + "/exefs/main";
    if (installation == game_profile_active()) main = game_main;
    GameVersion v = game_version_file(main.c_str());
    return v == GAME_UNSUPPORTED ? "Unknown" : game_version_name(v);
}

extern "C" void save_backups_draw(float scale, bool in_game) {
    static bool editor;
    if (ImGui::RadioButton(ui_label("Backups").c_str(), !editor)) editor = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(!in_game || !game_in_play());
    if (ImGui::RadioButton(ui_label("Save Editor").c_str(), editor)) editor = true;
    ImGui::EndDisabled();
    if (editor && in_game && game_in_play()) { save_editor_draw(scale); return; }
    editor = false;
    ImGui::TextWrapped("%s", ui_tr("Automatic backups keep the latest 20 snapshots per save area, including all slots. Unchanged saves are skipped."));
    bool locked = gamelib_tables() != NULL;
    if (locked) ImGui::TextWrapped("%s", ui_tr("Restart Multi to create or restore a backup from the start screen."));
    static std::string profile, selected, message;
    static bool confirmed;
    static int seen = -10;
    static double refresh_at;
    static SaveBackupInfo items[SAVE_BACKUP_KEEP];
    static int count;
    static std::vector<std::string> profiles;
    static int installation;
    if (seen != ImGui::GetFrameCount() - 1) {
        installation = game_profile_active();
        profile = g_settings.rando_seed; selected.clear(); confirmed = false; refresh_at = 0;
    }
    if (!getenv("LARECOMP_SAVE_DIR")) {
        ImGui::SetNextItemWidth(480 * scale);
        if (ImGui::BeginCombo(ui_label("Game version").c_str(), ui_tr(game_profile_name(installation)))) {
            for (int i = 0; i < GAME_PROFILE_COUNT; ++i)
                if (ImGui::Selectable(ui_tr(game_profile_name(i)), installation == i)) {
                    installation = i; profile.clear(); selected.clear(); confirmed = false; refresh_at = 0; message.clear();
                }
            ImGui::EndCombo();
        }
    }
    if (refresh_at == 0) {
        profiles = {""};
        std::error_code ec;
        if (!getenv("LARECOMP_SAVE_DIR"))
            for (auto &e : std::filesystem::directory_iterator("rando", ec))
                if (e.is_directory(ec) && !e.is_symlink(ec) && rando_seed_compatible(e.path().u8string().c_str(),
                    !strcmp(backup_version(installation), "1.1.0") ? GAME_110 : GAME_100)) profiles.push_back(e.path().filename().u8string());
    }
    seen = ImGui::GetFrameCount();
    if (getenv("LARECOMP_SAVE_DIR")) profile.clear();
    ImGui::SetNextItemWidth(320 * scale);
    if (ImGui::BeginCombo(ui_label("Save profile").c_str(), profile.empty() ? ui_tr("Vanilla") : profile.c_str())) {
        for (auto &p : profiles)
            if (ImGui::Selectable(p.empty() ? ui_tr("Vanilla") : p.c_str(), profile == p)) {
                profile = p; selected.clear(); confirmed = false; refresh_at = 0; message.clear();
            }
        ImGui::EndCombo();
    }
    std::string base = getenv("LARECOMP_SAVE_DIR") ? getenv("LARECOMP_SAVE_DIR") :
                       profile.empty() ? game_profile_save_dir(installation) : "rando/" + profile + "/save";
    static int area;
    static std::vector<int> areas;
    if (refresh_at == 0) {
        areas = {0};
        for (int i = 1; i <= 255; ++i) {
            std::error_code ec;
            auto path = std::filesystem::u8path(base + "/save_" + std::to_string(i));
            if (std::filesystem::exists(path, ec) || std::filesystem::exists(path.u8string() + ".backups", ec)) areas.push_back(i);
        }
        if (std::find(areas.begin(), areas.end(), area) == areas.end()) area = 0;
    }
    if (areas.size() > 1) {
        char label[16]; snprintf(label, sizeof label, "%d", area);
        ImGui::SameLine(); ImGui::SetNextItemWidth(100 * scale);
        if (ImGui::BeginCombo(ui_label("Save area").c_str(), label)) {
            for (int a : areas) {
                snprintf(label, sizeof label, "%d", a);
                if (ImGui::Selectable(label, area == a)) { area = a; refresh_at = 0; selected.clear(); confirmed = false; }
            }
            ImGui::EndCombo();
        }
    } else area = 0;
    std::string root = base + "/save_" + std::to_string(area);
    if (ImGui::Button(ui_label("Refresh").c_str())) refresh_at = 0;
    ImGui::SameLine();
    ImGui::BeginDisabled(locked);
    if (ImGui::Button(ui_label("Create backup").c_str())) {
        char why[512];
        bool ok = save_backup_create(root.c_str(), "Manual", backup_version(installation), false, why, sizeof why);
        message = ok ? "Backup created." : why; refresh_at = 0;
    }
    ImGui::EndDisabled();
    if (ImGui::GetTime() >= refresh_at) {
        char why[512];
        count = save_backup_list(root.c_str(), items, SAVE_BACKUP_KEEP, why, sizeof why);
        if (count < 0) message = why;
        refresh_at = ImGui::GetTime() + 5;
    }
    ImGui::Spacing();
    if (count <= 0) ImGui::TextWrapped("%s", ui_tr("No backups yet. A backup is made when an existing save is opened and after each completed game save."));
    for (int i = 0; i < count; ++i) {
        auto &b = items[i];
        char row[256];
        snprintf(row, sizeof row, "%s  |  %s  |  %s  |  %.0f KB###%s", b.id, ui_tr(b.version), ui_tr(b.reason), b.bytes / 1024.0, b.id);
        if (ImGui::Selectable(row, selected == b.id)) { selected = b.id; confirmed = false; }
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(locked || selected.empty());
    if (!confirmed) {
        if (ImGui::Button(ui_label("Restore selected backup").c_str())) confirmed = true;
    } else {
        ImGui::TextWrapped("%s", ui_tr("Restore all slots in this save area? The current files will be backed up first."));
        if (ImGui::Button(ui_label("Confirm restore").c_str())) {
            char why[512];
            bool ok = save_backup_restore(root.c_str(), selected.c_str(), backup_version(installation), why, sizeof why);
            message = ok ? "Backup restored. You can now start the game." : why;
            confirmed = false; refresh_at = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button(ui_label("Cancel").c_str())) confirmed = false;
    }
    ImGui::EndDisabled();
    if (!message.empty()) ImGui::TextWrapped("%s", ui_tr(message.c_str()));
}
