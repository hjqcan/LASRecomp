// Portable backup storage, deliberately independent of the renderer and guest runtime.
#include "../save_backup.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
std::mutex lock;
struct File { std::string path; uint64_t size; uint32_t crc; };
struct Snapshot { SaveBackupInfo info{}; std::vector<File> files; };
void need(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void text(char *out, size_t n, const std::string &s) { if (out && n) snprintf(out, n, "%s", s.c_str()); }
bool link(const fs::path &p) {
    if (fs::is_symlink(fs::symlink_status(p))) return true;
#ifdef _WIN32
    DWORD attributes = GetFileAttributesW(p.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return false;
}
void plain_path(const fs::path &p) {
    fs::path part;
    for (const auto &component : fs::absolute(p)) {
        part /= component;
        need(!link(part), "Save and backup paths cannot contain symbolic links or reparse points.");
    }
}
fs::path root_path(const char *root) {
    need(root && *root, "No save folder selected.");
    auto p = fs::absolute(fs::u8path(root));
    plain_path(p); // inspect links before collapsing '..', matching the game's path resolution
    p = p.lexically_normal();
    need(p.has_filename() && p != p.root_path(), "Invalid save folder.");
    return p;
}
fs::path store_path(const fs::path &root) { auto p = root; p += ".backups"; plain_path(p); return p; }
bool valid_id(const std::string &s) {
    return s.size() >= 20 && s.size() < 64 && s.find_first_not_of("0123456789-") == std::string::npos;
}
bool safe_relative(const std::string &s) {
    if (s.empty() || s.size() > 512 || s.find_first_of("\\:\r\n\t") != std::string::npos) return false;
    auto p = fs::u8path(s);
    if (p.is_absolute() || p.has_root_name()) return false;
    for (const auto &c : p) if (c == ".." || c == "." || c.empty()) return false;
    return true;
}
uint32_t checksum(const fs::path &p, uint64_t expected) {
    need(!link(p) && fs::is_regular_file(p) && fs::file_size(p) == expected, "Backup file size changed or file is unsafe.");
    std::ifstream f(p, std::ios::binary);
    need(bool(f), "Cannot read a save file.");
    uint32_t crc = ~0u;
    uint64_t total = 0;
    char buffer[65536];
    while (f.read(buffer, sizeof buffer) || f.gcount()) {
        total += (uint64_t)f.gcount();
        need(total <= expected, "Save changed during backup.");
        for (std::streamsize i = 0; i < f.gcount(); ++i) {
            crc ^= (unsigned char)buffer[i];
            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
        }
    }
    need(f.eof() && total == expected, "Cannot finish reading a save file.");
    return ~crc;
}
std::vector<File> scan(const fs::path &root) {
    plain_path(root);
    need(fs::is_directory(root), "No saved game exists in this profile yet.");
    std::vector<File> result;
    uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        need(it.depth() < 16 && !link(it->path()), "Save contains an unsafe directory or link.");
        if (it->is_directory()) continue;
        need(it->is_regular_file(), "Save contains a non-regular file.");
        std::string rel = it->path().lexically_relative(root).generic_u8string();
        uint64_t size = it->file_size();
        need(safe_relative(rel) && result.size() < 1024 && size <= (16u << 20), "Save file name, count or size exceeds backup limits.");
        total += size;
        need(total <= (128u << 20), "Save exceeds the 128 MiB backup limit.");
        result.push_back({rel, size, checksum(it->path(), size)});
    }
    need(!result.empty(), "No saved game exists in this profile yet.");
    std::sort(result.begin(), result.end(), [](auto &a, auto &b) { return a.path < b.path; });
    return result;
}
std::string signature(const std::vector<File> &files) {
    std::ostringstream out;
    for (auto &f : files) out << f.crc << ' ' << f.size << ' ' << std::quoted(f.path) << '\n';
    return out.str();
}
Snapshot read(const fs::path &dir, bool verify) {
    plain_path(dir);
    for (auto &entry : fs::directory_iterator(dir))
        need(entry.path().filename() == "manifest.txt" || entry.path().filename() == "files", "Backup contains unmanaged files.");
    auto manifest = dir / "manifest.txt";
    need(!link(manifest) && fs::file_size(manifest) <= (1u << 20), "Invalid backup manifest.");
    std::ifstream in(manifest, std::ios::binary);
    std::string magic, reason, version;
    std::getline(in, magic);
    need(magic == "LASBACKUP1" && bool(in >> std::quoted(reason) >> std::quoted(version)) &&
         reason.size() < 64 && version.size() < 32, "Invalid backup manifest.");
    Snapshot result;
    text(result.info.id, sizeof result.info.id, dir.filename().u8string());
    text(result.info.reason, sizeof result.info.reason, reason);
    text(result.info.version, sizeof result.info.version, version);
    std::string line;
    std::getline(in, line);
    need(line.empty(), "Invalid backup manifest header.");
    while (std::getline(in, line)) {
        File f;
        std::istringstream entry(line);
        need(bool(entry >> f.crc >> f.size >> std::quoted(f.path)), "Incomplete backup file entry.");
        entry >> std::ws;
        need(entry.eof(), "Invalid backup file entry.");
        need(safe_relative(f.path) && result.files.size() < 1024 && f.size <= (16u << 20), "Invalid backup file entry.");
        need(result.files.empty() || result.files.back().path < f.path, "Duplicate or unordered backup file entry.");
        result.info.bytes += f.size;
        need(result.info.bytes <= (128u << 20), "Backup exceeds size limit.");
        result.files.push_back(f);
    }
    need(in.eof() && !result.files.empty(), "Incomplete backup manifest.");
    result.info.files = (unsigned)result.files.size();
    if (verify) need(signature(scan(dir / "files")) == signature(result.files), "Backup checksum mismatch; current saves were not changed.");
    return result;
}
std::vector<fs::path> snapshots(const fs::path &store) {
    std::vector<fs::path> dirs;
    if (!fs::exists(store)) return dirs;
    for (auto &entry : fs::directory_iterator(store))
        if (valid_id(entry.path().filename().u8string()) && !link(entry.path()) && entry.is_directory()) dirs.push_back(entry.path());
    std::sort(dirs.begin(), dirs.end(), std::greater<fs::path>());
    return dirs;
}
std::string new_id(const fs::path &store) {
    auto now = std::chrono::system_clock::now();
    time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32]; strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    for (int i = 0; i < 10000; ++i) {
        char id[64]; snprintf(id, sizeof id, "%s-%03d-%04d", stamp, (int)millis, i);
        if (!fs::exists(store / id) && !fs::exists(store / (std::string(".pending-") + id))) return id;
    }
    throw std::runtime_error("Cannot allocate a backup name.");
}
void copy_files(const fs::path &from, const fs::path &to, const std::vector<File> &files) {
    fs::create_directories(to);
    for (auto &f : files) {
        auto src = from / fs::u8path(f.path), dst = to / fs::u8path(f.path);
        plain_path(src);
        fs::create_directories(dst.parent_path());
        fs::copy_file(src, dst); // destination must be new; never overwrite
        need(checksum(dst, f.size) == f.crc, "Save changed during backup or copy verification failed.");
    }
}
void prune(const fs::path &store) {
    auto dirs = snapshots(store);
    for (size_t i = SAVE_BACKUP_KEEP; i < dirs.size(); ++i) {
        // Verify the tree before deleting only an older, managed snapshot. Leave foreign/corrupt files alone.
        try { read(dirs[i], true); fs::remove_all(dirs[i]); } catch (...) {}
    }
}
void create(const fs::path &root, const char *reason, const char *version, bool dedup) {
    need(reason && version && strlen(reason) < 64 && strlen(version) < 32, "Invalid backup description.");
    auto files = scan(root);
    auto store = store_path(root);
    fs::create_directories(store);
    auto old = snapshots(store);
    if (dedup && !old.empty()) {
        try { if (signature(read(old[0], true).files) == signature(files)) return; } catch (...) {}
    }
    std::string id = new_id(store);
    auto stage = store / (".pending-" + id);
    need(fs::create_directory(stage), "Cannot create backup staging directory.");
    try {
        copy_files(root, stage / "files", files);
        std::ofstream out(stage / "manifest.txt", std::ios::binary);
        out << "LASBACKUP1\n" << std::quoted(std::string(reason)) << '\n' << std::quoted(std::string(version)) << '\n' << signature(files);
        out.close(); need(bool(out), "Cannot write backup manifest.");
        read(stage, true);
        fs::rename(stage, store / id);
    } catch (...) { std::error_code ec; fs::remove_all(stage, ec); throw; }
    prune(store);
}
}

extern "C" bool save_backup_create(const char *root, const char *reason, const char *version, bool dedup, char *why, size_t cap) {
    std::lock_guard<std::mutex> guard(lock);
    text(why, cap, "");
    try { create(root_path(root), reason, version, dedup); return true; }
    catch (const std::exception &e) { text(why, cap, e.what()); return false; }
}
extern "C" int save_backup_list(const char *root, SaveBackupInfo *out, int capacity, char *why, size_t cap) {
    std::lock_guard<std::mutex> guard(lock);
    text(why, cap, ""); int n = 0;
    try {
        for (auto &p : snapshots(store_path(root_path(root)))) {
            if (n >= capacity) break;
            try { auto info = read(p, false).info; out[n++] = info; } catch (...) {} // damaged manifests aren't offered
        }
    } catch (const std::exception &e) { text(why, cap, e.what()); return -1; }
    return n;
}
extern "C" bool save_backup_restore(const char *root_name, const char *id, const char *version, char *why, size_t cap) {
    std::lock_guard<std::mutex> guard(lock);
    text(why, cap, "");
    fs::path stage;
    bool owned_stage = false;
    try {
        need(id && valid_id(id), "Invalid backup selection.");
        auto root = root_path(root_name), store = store_path(root), selected = store / id;
        auto snapshot = read(selected, true); // validate everything before touching the current save
        std::string restore_id = new_id(store);
        stage = store / (".restore-" + restore_id);
        need(fs::create_directory(stage), "Cannot create restore staging directory.");
        owned_stage = true;
        copy_files(selected / "files", stage / "files", snapshot.files);
        bool had_save = fs::exists(root);
        if (had_save && !fs::is_empty(root)) create(root, "Before restore", version, false);
        auto previous = store / (".previous-" + restore_id);
        if (had_save) fs::rename(root, previous);
        try { fs::rename(stage / "files", root); }
        catch (...) {
            if (had_save) fs::rename(previous, root);
            throw;
        }
        // The new save is installed. A cleanup failure must not report that restore failed.
        std::error_code ec;
        if (had_save) fs::remove_all(previous, ec);
        fs::remove_all(stage, ec);
        return true;
    } catch (const std::exception &e) {
        if (owned_stage) { std::error_code ec; fs::remove_all(stage, ec); }
        text(why, cap, e.what()); return false;
    }
}
