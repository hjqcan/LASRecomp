#include "../game_install.h"
#include "cache_sha256.h"
#include "host_path.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace fs = std::filesystem;
struct GameInstall {
    fs::path slot, meta, candidate;
    std::string path;
#ifdef _WIN32
    HANDLE lock = INVALID_HANDLE_VALUE;
    ~GameInstall() { if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock); }
#else
    int lock = -1;
    ~GameInstall() { if (lock >= 0) { flock(lock, LOCK_UN); close(lock); } }
#endif
};
namespace {
void need(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
void checkpoint(const char *name) {
#ifdef GAME_INSTALL_TESTING
    const char *crash = std::getenv("LARECOMP_INSTALL_CRASH_AT");
    if (crash && !strcmp(crash, name)) std::_Exit(86);
#else
    (void)name;
#endif
}
bool link(const fs::path &path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        throw std::system_error((int)error, std::system_category(), "Cannot inspect installation path");
    return false;
#else
    return fs::is_symlink(fs::symlink_status(path));
#endif
}
void plain(const fs::path &path) {
    // Iterating/reassembling \\?\C:\ components can lose the extended prefix
    // in libstdc++. Check complete ancestor paths instead, including the leaf.
    auto part = fs::absolute(host_path::logical(path)).lexically_normal();
    for (;;) {
        need(!link(host_path::io(part)), "Installation paths cannot contain links or reparse points");
        auto parent = part.parent_path();
        if (parent == part || parent.empty()) break;
        part = parent;
    }
}
std::string read(const fs::path &path) {
    plain(path);
    need(fs::is_regular_file(path) && fs::file_size(path) <= 4096, "Invalid installation record");
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    need(file != INVALID_HANDLE_VALUE, "Cannot read installation record");
    LARGE_INTEGER length;
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 || length.QuadPart > 4096) {
        CloseHandle(file); throw std::runtime_error("Invalid installation record");
    }
    std::string result((size_t)length.QuadPart, '\0'); DWORD count = 0;
    bool ok = ReadFile(file, result.data(), (DWORD)result.size(), &count, nullptr) && count == result.size();
    CloseHandle(file); need(ok, "Cannot read installation record"); return result;
#else
    std::ifstream input(path, std::ios::binary);
    need(bool(input), "Cannot read installation record");
    return {std::istreambuf_iterator<char>(input), {}};
#endif
}
void write(const fs::path &path, const std::string &value) {
    plain(path);
    auto temporary = path; temporary += ".writing";
    plain(temporary);
    { std::ofstream output(temporary, std::ios::binary | std::ios::trunc); output << value; output.close();
      need(bool(output), "Cannot write installation record"); }
#ifdef _WIN32
    need(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
         "Cannot publish installation record");
#else
    int file = open(temporary.c_str(), O_RDONLY);
    need(file >= 0, "Cannot flush installation record");
    int synced = fsync(file); close(file); need(synced == 0, "Cannot flush installation record");
    fs::rename(temporary, path);
    int directory = open(path.parent_path().c_str(), O_RDONLY);
    if (directory >= 0) { fsync(directory); close(directory); }
#endif
}
bool generation(const std::string &name) {
    return name.size() > 6 && name.size() < 80 && name.rfind("build-", 0) == 0 &&
           name.substr(6).find_first_not_of("0123456789abcdef-") == std::string::npos;
}
std::string pointer(const fs::path &meta, const char *name) {
    const auto path = meta / name;
    if (!fs::exists(path)) return {};
    std::istringstream input(read(path)); std::string magic, result, extra;
    need(bool(std::getline(input, magic)) && magic == "LASINSTALL2" && bool(std::getline(input, result)) &&
         generation(result) && !std::getline(input, extra), "Invalid installation pointer; repair this installation");
    plain(meta / result);
    need(fs::is_directory(meta / result), "An installation candidate is missing; repair this installation");
    return result;
}
void owner(const fs::path &meta) {
    plain(meta);
    need(read(meta / "owner") == "LASINSTALL2\n", "Unknown installation metadata; refusing to replace it");
}
std::unique_ptr<GameInstall> acquire(const char *slot) {
    auto install = std::make_unique<GameInstall>();
    install->slot = host_path::io(fs::u8path(slot));
    install->meta = install->slot / ".installations";
    plain(install->meta);
    fs::create_directories(install->meta);
    const auto lock = install->meta / "lock"; plain(lock);
#ifdef _WIN32
    install->lock = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    need(install->lock != INVALID_HANDLE_VALUE, "This installation is already being changed by another process");
#else
    install->lock = open(lock.c_str(), O_CREAT | O_RDWR, 0600);
    need(install->lock >= 0 && flock(install->lock, LOCK_EX | LOCK_NB) == 0,
         "This installation is already being changed by another process");
#endif
    if (fs::exists(install->meta / "owner")) owner(install->meta);
    else {
        for (const auto &item : fs::directory_iterator(install->meta))
            need(item.path().filename() == "lock" ||
                 (item.path().filename() == "owner.writing" && read(item.path()) == "LASINSTALL2\n"),
                 "Unknown installation metadata; refusing to replace it");
        write(install->meta / "owner", "LASINSTALL2\n");
    }
    return install;
}
std::string digest(const fs::path &path) {
    plain(path);
    std::ifstream input(path, std::ios::binary); need(bool(input), "Cannot read candidate content");
    CacheSha256 hash; char bytes[1 << 16];
    while (input.read(bytes, sizeof bytes) || input.gcount()) hash.add(bytes, (size_t)input.gcount());
    need(input.eof(), "Cannot finish reading candidate content");
    return hash.finish();
}
std::vector<fs::path> files(const fs::path &root) {
    std::vector<fs::path> result;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        need(it.depth() < 32 && !link(it->path()), "Unsafe installation content");
        if (it->is_regular_file()) result.push_back(it->path());
    }
    std::sort(result.begin(), result.end()); return result;
}
void fail(char *why, size_t size, const std::exception &error) { snprintf(why, size, "%s", error.what()); }
}
extern "C" bool game_install_resolve(const char *slot, char *out, size_t size, char *why, size_t cap) {
    try {
        fs::path root = host_path::io(fs::u8path(slot));
        const auto meta = root / ".installations";
        if (fs::exists(meta / "current")) {
            owner(meta);
            auto current = pointer(meta, "current");
            if (!current.empty()) {
                root = meta / current / "content";
                need(read(root / "installation-receipt").rfind("LASRECEIPT1\n", 0) == 0,
                     "The published installation receipt is missing or invalid");
            }
        }
        std::string path = host_path::logical(root).u8string();
        need(path.size() < size, "Installation path is too long");
        snprintf(out, size, "%s", path.c_str()); return true;
    } catch (const std::exception &error) { fail(why, cap, error); return false; }
}
extern "C" bool game_install_pending(const char *slot) {
    try {
        auto meta = host_path::io(fs::u8path(slot)) / ".installations";
        if (!fs::exists(meta)) return false;
        owner(meta);
        auto pending = pointer(meta, "pending");
        return !pending.empty() && pending != pointer(meta, "current");
    } catch (...) { return false; }
}
extern "C" bool game_install_pending_path(const char *slot, char *out, size_t size) {
    try {
        if (!game_install_pending(slot)) return false;
        auto meta = host_path::io(fs::u8path(slot)) / ".installations";
        auto path = host_path::logical(meta / pointer(meta, "pending") / "content").u8string();
        if (path.size() >= size) return false;
        snprintf(out, size, "%s", path.c_str()); return true;
    } catch (...) { return false; }
}
extern "C" GameInstall *game_install_begin(const char *slot, bool fresh, char *why, size_t cap) {
    try {
        auto install = acquire(slot);
        std::string name = pointer(install->meta, "pending");
        if (name == pointer(install->meta, "current")) name.clear();
        if (fresh || name.empty()) {
            auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            for (unsigned n = 0;; ++n) {
                char value[80]; snprintf(value, sizeof value, "build-%llx-%x", (unsigned long long)stamp, n); name = value;
                if (fs::create_directory(install->meta / name)) break;
            }
            write(install->meta / "pending", "LASINSTALL2\n" + name + "\n");
        }
        install->candidate = install->meta / name / "content";
        fs::create_directories(install->candidate);
        install->path = host_path::logical(install->candidate).u8string();
        return install.release();
    } catch (const std::exception &error) { fail(why, cap, error); return nullptr; }
}
extern "C" const char *game_install_candidate(GameInstall *install) { return install->path.c_str(); }
extern "C" bool game_install_clone(GameInstall *install, const char *source, char *why, size_t cap) {
    try {
        fs::path root = host_path::io(fs::u8path(source)); plain(root);
        need(root != install->candidate, "Cannot clone an installation onto itself");
        const auto record = install->candidate.parent_path() / "clone-source";
        const auto complete = install->candidate.parent_path() / "clone-complete";
        const std::string identity = "LASCLONE1\n" + host_path::logical(root).u8string();
        if (fs::exists(record)) need(read(record) == identity, "Clone source changed; start a new installation");
        else write(record, identity);
        if (fs::exists(complete)) {
            need(read(complete) == "LASCLONE1\n", "Invalid clone completion record");
            return true;
        }
        for (const char *name : {"exefs", "romfs", "romfs.bin", "icon.ico", "import-manifest.json", ".gamebuild"}) {
            auto path = root / name; plain(path);
            if (!fs::exists(path)) continue;
            if (fs::is_directory(path)) {
                fs::create_directories(install->candidate / name);
                for (const auto &item : fs::recursive_directory_iterator(path)) {
                    need(!link(item.path()), "Unsafe installation content");
                    if (item.is_directory()) fs::create_directories(install->candidate / host_path::relative(item.path(), root));
                }
            }
            auto entries = fs::is_directory(path) ? files(path) : std::vector<fs::path>{path};
            for (const auto &file : entries) {
                if (file.filename() == ".lock" || file.extension() == ".tmp" || file.extension() == ".writing") continue;
                auto relative = host_path::relative(file, root), target = install->candidate / relative;
                plain(target); fs::create_directories(target.parent_path());
                fs::copy_file(file, target, fs::copy_options::overwrite_existing);
                checkpoint("clone-file");
            }
        }
        write(complete, "LASCLONE1\n");
        return true;
    } catch (const std::exception &error) { fail(why, cap, error); return false; }
}
extern "C" bool game_install_resume_clone(GameInstall *install, char *why, size_t size) {
    try {
        const auto record = install->candidate.parent_path() / "clone-source";
        if (!fs::exists(record)) return true;
        const std::string source = read(record);
        constexpr size_t prefix = sizeof("LASCLONE1\n") - 1;
        need(source.rfind("LASCLONE1\n", 0) == 0 && source.size() > prefix, "Invalid clone source record");
        return game_install_clone(install, source.substr(prefix).c_str(), why, size);
    } catch (const std::exception &error) { fail(why, size, error); return false; }
}
extern "C" bool game_install_publish(GameInstall *install, const char *id, GameInstallValidator validate, char *why, size_t cap) {
    try {
        need(id && *id && !strchr(id, '\n'), "Invalid installation launch ID");
        if (!validate || !validate(install->path.c_str(), why, cap)) return false;
        CacheSha256 tree;
        for (const auto &path : files(install->candidate)) {
            auto relative = host_path::relative(path, install->candidate).generic_u8string();
            if (relative.rfind(".gamebuild/", 0) == 0 || relative == "installation-receipt") continue;
            auto hash = digest(path); tree.add(relative.data(), relative.size() + 1); tree.add(hash.data(), hash.size());
        }
        write(install->candidate / "installation-receipt", std::string("LASRECEIPT1\n") + id + "\n" + tree.finish() + "\n");
        checkpoint("receipt");
        write(install->meta / "current", "LASINSTALL2\n" + install->candidate.parent_path().filename().u8string() + "\n");
        checkpoint("published");
        // Publication is complete. Cleanup failure must not report a failed installation.
        std::error_code ignored; fs::remove(install->meta / "pending", ignored);
        return true;
    } catch (const std::exception &error) { fail(why, cap, error); return false; }
}
extern "C" void game_install_close(GameInstall *install) { delete install; }
extern "C" bool game_install_clear(const char *slot, char *why, size_t cap) {
    try {
        const auto meta = host_path::io(fs::u8path(slot)) / ".installations"; plain(meta);
        if (!fs::exists(meta)) return true;
        auto install = acquire(slot);
        std::vector<fs::path> owned;
        for (const auto &item : fs::directory_iterator(meta)) {
            auto name = item.path().filename().u8string();
            need(generation(name) || name == "current" || name == "pending" || name == "owner" || name == "lock" ||
                 name == "current.writing" || name == "pending.writing", "Unknown file in installation metadata; refusing cleanup");
            plain(item.path());
            if (fs::is_directory(item.path())) { files(item.path()); owned.push_back(item.path()); }
        }
        for (const char *name : {"current", "pending", "current.writing", "pending.writing"}) fs::remove(meta / name);
        for (const auto &path : owned) fs::remove_all(path); // checked owned children of the exact slot above
        return true;
    } catch (const std::exception &error) { fail(why, cap, error); return false; }
}
