#include "../mods.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
namespace fs = std::filesystem;
namespace {
struct Pack { ModInfo info{}; fs::path directory; std::map<std::string, fs::path> files; };
std::map<std::string, fs::path> active_files;
void need(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
void error(char *why, size_t cap, const std::exception &e) { if (cap) snprintf(why, cap, "%s", e.what()); }
bool id_ok(const std::string &id) {
    return !id.empty() && id.size() < 64 && id[0] >= 'a' && id[0] <= 'z' &&
           id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos;
}
void plain(const fs::path &path) {
    fs::path part;
    for (const auto &c : fs::absolute(path)) {
        part /= c;
        need(!fs::is_symlink(fs::symlink_status(part)), "Mod paths cannot contain links.");
#ifdef _WIN32
        auto attr = GetFileAttributesW(part.c_str());
        need(attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_REPARSE_POINT), "Mod paths cannot contain reparse points.");
#endif
    }
}
std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b-a+1);
}
bool valid_utf8(const std::string &text) {
    for (size_t i = 0; i < text.size();) {
        unsigned char first = text[i++];
        if (first < 0x80) continue;
        unsigned count, value, minimum;
        if (first >= 0xc2 && first <= 0xdf) count = 1, value = first & 31, minimum = 0x80;
        else if (first >= 0xe0 && first <= 0xef) count = 2, value = first & 15, minimum = 0x800;
        else if (first >= 0xf0 && first <= 0xf4) count = 3, value = first & 7, minimum = 0x10000;
        else return false;
        if (i + count > text.size()) return false;
        while (count--) {
            unsigned char c = text[i++];
            if ((c & 0xc0) != 0x80) return false;
            value = (value << 6) | (c & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
std::string read(const fs::path &p, uintmax_t limit) {
    plain(p);
    need(fs::is_regular_file(p) && fs::file_size(p) <= limit, "Invalid mod manifest or configuration.");
    std::ifstream f(p, std::ios::binary);
    need(bool(f), "Cannot read mod data.");
    std::string text((size_t)limit + 1, '\0');
    f.read(text.data(), (std::streamsize)text.size());
    text.resize((size_t)f.gcount());
    need(!f.bad() && text.size() <= limit, "Cannot read mod data within its size limit.");
    need(text.find('\0') == std::string::npos, "Mod text cannot contain NUL bytes.");
    need(valid_utf8(text), "Mod text must be valid UTF-8.");
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
    return text;
}
fs::path mod_root(const char *data) {
    auto p = fs::absolute(fs::u8path(data)) / "mods";
    plain(p); return p;
}
fs::path config(const fs::path &root, int profile) {
    need(profile >= 0 && profile <= 2, "Select a managed game installation first.");
    const char *names[] = {"enabled-existing.txt", "enabled-1.0.0.txt", "enabled-1.1.0.txt"};
    return root / names[profile];
}
std::set<std::string> enabled_ids(const fs::path &root, int profile) {
    std::set<std::string> ids;
    auto p = config(root, profile);
    plain(p);
    if (!fs::exists(p)) return ids;
    std::istringstream stream(read(p, 8192));
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty()) continue;
        need(id_ok(line) && ids.insert(line).second && ids.size() <= MODS_MAX, "Invalid enabled mod list.");
    }
    return ids;
}
void save_ids(const fs::path &root, int profile, const std::set<std::string> &ids) {
    fs::create_directories(root);
    auto target = config(root, profile), tmp = target;
    tmp += ".writing"; plain(tmp); plain(target);
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    for (const auto &id : ids) f << id << '\n';
    f.close(); need(bool(f), "Cannot save enabled mods.");
#ifdef _WIN32
    need(MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
         "Cannot replace mod configuration.");
#else
    fs::rename(tmp, target);
#endif
}
std::string path_key(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
bool safe_relative(const fs::path &p) {
    if (p.empty() || p.is_absolute()) return false;
    for (const auto &part : p) {
        auto s = part.u8string();
        if (s.empty() || s == "." || s == ".." || s.back() == '.' || s.back() == ' ' ||
            s.find_first_of("\\:<>|?*") != std::string::npos || s.find(char(34)) != std::string::npos) return false;
        for (unsigned char c : s) if (c < 32) return false;
    }
    return true;
}
Pack load_pack(const fs::path &directory, const fs::path &game, const char *version, bool check_files = true) {
    Pack pack; pack.directory = directory;
    plain(directory); plain(game);
    std::istringstream in(read(directory / "mod.ini", 8192));
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line); if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t at = line.find('='); need(at != std::string::npos, "Expected key=value in mod.ini.");
        auto key = trim(line.substr(0,at)), value = trim(line.substr(at+1));
        static const std::set<std::string> allowed = {"format", "id", "name", "version", "author", "description", "game_versions"};
        need(allowed.count(key) && fields.emplace(key, value).second, "Unknown or duplicate mod.ini field.");
        for (unsigned char c : value) need(c >= 32, "Invalid control character in mod.ini.");
    }
    need(fields["format"] == "1" && id_ok(fields["id"]) && !fields["name"].empty(), "mod.ini needs format=1, a valid id and a name.");
    auto copy = [&](const char *key, char *dest, size_t cap) {
        const auto &v = fields[key]; need(v.size() < cap, std::string("Mod field is too long: ") + key);
        memcpy(dest, v.c_str(), v.size()+1);
    };
    copy("id", pack.info.id, sizeof pack.info.id); copy("name", pack.info.name, sizeof pack.info.name);
    copy("version", pack.info.version, sizeof pack.info.version); copy("author", pack.info.author, sizeof pack.info.author);
    copy("description", pack.info.description, sizeof pack.info.description);
    std::istringstream versions(fields["game_versions"]); bool any = false;
    while (std::getline(versions, line, ',')) {
        line = trim(line); need(line == "1.0.0" || line == "1.1.0", "Unsupported game version in mod.ini.");
        any = true; pack.info.compatible |= line == version;
    }
    need(any, "mod.ini needs game_versions.");
    if (!check_files) return pack;
    auto romfs = directory / "romfs"; plain(romfs);
    need(fs::is_directory(romfs), "Mod folder needs a romfs directory.");
    for (auto it = fs::recursive_directory_iterator(romfs); it != fs::recursive_directory_iterator(); ++it) {
        plain(it->path()); need(it.depth() < 16, "Mod directory nesting exceeds the limit.");
        if (it->is_directory()) continue;
        need(it->is_regular_file(), "Mod entries must be ordinary files.");
        auto relative = it->path().lexically_relative(romfs);
        need(safe_relative(relative) && relative.generic_u8string().size() < 768, "Unsafe or oversized mod resource path.");
        auto original = game / "romfs" / relative; plain(original);
        need(fs::is_regular_file(original), "Mod resource is not present in the selected game's RomFS: " + relative.generic_u8string());
        auto size = it->file_size();
        need(size <= (1ull<<30) && pack.info.bytes + size <= (8ull<<30) && pack.info.files < 4096,
             "Mod exceeds the file count or size limits.");
        auto name = relative.generic_u8string();
        need(pack.files.emplace(path_key(name), it->path()).second, "Duplicate mod resource path.");
        pack.info.bytes += size; ++pack.info.files;
    }
    need(pack.info.files > 0, "Mod has no replacement files.");
    return pack;
}
std::map<std::string, fs::path> resolve(const fs::path &root, const fs::path &game, const char *version,
                                      const std::set<std::string> &ids) {
    std::map<std::string, fs::path> result;
    for (const auto &id : ids) {
        auto pack = load_pack(root/id, game, version);
        need(id == pack.info.id, "Mod folder and manifest ID differ.");
        need(pack.info.compatible, "Enabled mod does not support the selected game version: " + id);
        for (const auto &file : pack.files)
            need(result.emplace(file).second, "Enabled mods replace the same resource: " + file.first);
    }
    return result;
}
}

extern "C" int mods_list(const char *data_root, const char *game_root, int profile, const char *version,
                          ModInfo *out, int cap, char *why, size_t why_cap) {
    try {
        auto root = mod_root(data_root); auto enabled = enabled_ids(root, profile);
        if (!fs::exists(root)) return 0;
        std::vector<Pack> packs;
        for (const auto &e : fs::directory_iterator(root)) {
            auto id = e.path().filename().u8string();
            if (!id_ok(id) || !e.is_directory()) continue;
            need(packs.size() < MODS_MAX, "Too many installed mods.");
            Pack pack;
            try {
                pack = load_pack(e.path(), fs::u8path(game_root), version);
                need(id == pack.info.id, "Mod folder and manifest ID differ.");
            } catch (const std::exception &ex) {
                snprintf(pack.info.id, sizeof pack.info.id, "%s", id.c_str());
                if (!*pack.info.name) snprintf(pack.info.name, sizeof pack.info.name, "%s", id.c_str());
                snprintf(pack.info.problem, sizeof pack.info.problem, "%s", ex.what());
                pack.files.clear();
            }
            pack.info.enabled = enabled.count(id) != 0;
            packs.push_back(std::move(pack));
        }
        for (const auto &id : enabled) {
            if (std::none_of(packs.begin(), packs.end(), [&](const Pack &p) { return id == p.info.id; })) {
                Pack missing; missing.info.enabled = true;
                snprintf(missing.info.id, sizeof missing.info.id, "%s", id.c_str());
                snprintf(missing.info.name, sizeof missing.info.name, "%s", id.c_str());
                snprintf(missing.info.problem, sizeof missing.info.problem, "Enabled mod folder is missing.");
                packs.push_back(std::move(missing));
            }
        }
        for (auto &a : packs) for (const auto &b : packs)
            if (b.info.enabled && strcmp(a.info.id, b.info.id))
                for (const auto &f : a.files) a.info.conflicts += b.files.count(f.first) != 0;
        std::sort(packs.begin(), packs.end(), [](const Pack &a, const Pack &b) { return strcmp(a.info.id,b.info.id)<0; });
        need((int)packs.size() <= cap, "Mod list buffer is too small.");
        for (size_t i = 0; i < packs.size(); ++i) out[i] = packs[i].info;
        return (int)packs.size();
    } catch (const std::exception &e) { error(why, why_cap, e); return -1; }
}
extern "C" bool mods_import(const char *data_root, const char *game_root, const char *version, const char *source,
                             char *why, size_t cap) {
    fs::path stage;
    try {
        auto root = mod_root(data_root);
        auto source_path = fs::absolute(fs::u8path(source)); plain(source_path);
        auto pack = load_pack(source_path, fs::u8path(game_root), version);
        need(pack.info.compatible, "This mod does not support the selected game version.");
        fs::create_directories(root);
        auto target = root / pack.info.id; plain(target);
        need(!fs::exists(target), "A mod with this ID is already installed.");
        unsigned count = 0;
        for (const auto &e : fs::directory_iterator(root)) if (e.is_directory() && id_ok(e.path().filename().u8string())) ++count;
        need(count < MODS_MAX, "Too many installed mods.");
        stage = root / (".import-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        need(fs::create_directory(stage), "Cannot create a mod staging directory.");
        fs::copy_file(source_path/"mod.ini", stage/"mod.ini");
        for (const auto &file : pack.files) {
            auto relative = file.second.lexically_relative(source_path/"romfs");
            auto to = stage/"romfs"/relative;
            fs::create_directories(to.parent_path()); fs::copy_file(file.second, to);
        }
        auto checked = load_pack(stage, fs::u8path(game_root), version);
        need(checked.info.files == pack.info.files && checked.info.bytes == pack.info.bytes &&
             !strcmp(checked.info.id, pack.info.id), "Mod changed while importing.");
        fs::rename(stage, target); stage.clear();
        return true;
    } catch (const std::exception &e) {
        if (!stage.empty()) { std::error_code ec; fs::remove_all(stage,ec); }
        error(why, cap, e); return false;
    }
}
extern "C" bool mods_enable(const char *data_root, const char *game_root, int profile, const char *version,
                             const char *id, bool enabled, char *why, size_t cap) {
    try {
        need(id && id_ok(id), "Invalid mod ID.");
        auto root = mod_root(data_root); auto ids = enabled_ids(root, profile);
        if (enabled) { ids.insert(id); need(ids.size() <= MODS_MAX, "Too many enabled mods."); resolve(root, fs::u8path(game_root), version, ids); }
        else ids.erase(id);
        save_ids(root, profile, ids);
        return true;
    } catch (const std::exception &e) { error(why, cap, e); return false; }
}
extern "C" bool mods_load_active(const char *data_root, const char *game_root, int profile, const char *version,
                                  char *why, size_t cap) {
    active_files.clear();
    try {
        // Explicit custom roots have no managed mod configuration.
        if (profile < 0) return true;
        auto root = mod_root(data_root);
        active_files = resolve(root, fs::u8path(game_root), version, enabled_ids(root,profile));
        fprintf(stderr, "[mods] %zu replacement resources active for %s\n", active_files.size(), version);
        return true;
    } catch (const std::exception &e) { error(why, cap, e); return false; }
}
extern "C" bool mods_romfs_override(const char *relative, char *out, size_t cap) {
    if (!relative) return false;
    auto it = active_files.find(path_key(relative));
    if (it == active_files.end()) return false;
    auto name = it->second.u8string();
    if (name.size() >= cap) return false;
    memcpy(out, name.c_str(), name.size()+1); return true;
}
extern "C" void mods_clear_active(void) { active_files.clear(); }
