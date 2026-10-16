#include "../build_cache.h"
#include "cache_sha256.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace fs = std::filesystem;
struct BuildCache {
    fs::path root;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~BuildCache() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
#else
    int handle = -1;
    ~BuildCache() {
        if (handle >= 0) {
            flock(handle, LOCK_UN);
            close(handle);
        }
    }
#endif
};
namespace {
void need(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void error(char *out, size_t cap, const std::exception &e) {
    if (cap)
        snprintf(out, cap, "%s", e.what());
}
bool is_link(const fs::path &p) {
    if (fs::is_symlink(fs::symlink_status(p)))
        return true;
#ifdef _WIN32
    auto a = GetFileAttributesW(p.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT))
        return true;
#endif
    return false;
}
void plain(const fs::path &p) {
    fs::path part;
    for (const auto &c : fs::absolute(p)) {
        part /= c;
        need(!is_link(part), "Build paths cannot contain links or reparse points.");
    }
}
void plain_tree(const fs::path &p) {
    plain(p);
    for (auto it = fs::recursive_directory_iterator(p); it != fs::recursive_directory_iterator(); ++it)
        need(it.depth() < 32 && !is_link(it->path()), "Unsafe build cache entry.");
}
std::string digest(const fs::path &p) {
    plain(p);
    need(fs::is_regular_file(p), "A build input is missing.");
    std::ifstream f(p, std::ios::binary);
    need(bool(f), "Cannot read a build input.");
    CacheSha256 hash;
    char block[65536];
    while (f.read(block, sizeof block) || f.gcount())
        hash.add(block, (size_t)f.gcount());
    need(f.eof(), "Cannot finish reading a build input.");
    return hash.finish();
}
std::string read(const fs::path &p, uintmax_t limit = 1 << 20) {
    plain(p);
    need(fs::file_size(p) <= limit, "Invalid build cache record.");
    std::ifstream f(p, std::ios::binary);
    need(bool(f), "Cannot read the build cache.");
    return {std::istreambuf_iterator<char>(f), {}};
}
void replace(const fs::path &from, const fs::path &to) {
    plain(from);
    plain(to);
#ifdef _WIN32
    need(MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
         "Cannot replace a build file.");
#else
    fs::rename(from, to);
#endif
}
void write(const fs::path &p, const std::string &data) {
    plain(p);
    auto tmp = p;
    tmp += ".writing";
    plain(tmp);
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << data;
    f.close();
    need(bool(f), "Cannot write a build cache record.");
    replace(tmp, p);
}
bool valid_stem(const std::string &s) {
    return !s.empty() && s.size() < 64 &&
           s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos;
}
std::string generation(BuildCache *c, const char *module, int chunks) {
    need((!strcmp(module, "main") || !strcmp(module, "sdk")) && chunks > 0 && chunks <= 10000,
         "Invalid generated module.");
    std::ostringstream out;
    out << "LASGEN1\n" << chunks << '\n';
    for (int i = 0; i <= chunks; ++i) {
        char name[80];
        if (i == chunks)
            snprintf(name, sizeof name, "%s_table.c", module);
        else
            snprintf(name, sizeof name, "%s_%03d.c", module, i);
        out << name << ' ' << digest(c->root / module / name) << '\n';
    }
    return out.str();
}
std::unique_ptr<BuildCache> acquire(const char *game_dir) {
    auto c = std::make_unique<BuildCache>();
    c->root = fs::absolute(fs::u8path(game_dir)) / ".gamebuild";
    plain(c->root);
    fs::create_directories(c->root);
    auto lock = c->root / ".lock";
    plain(lock);
#ifdef _WIN32
    c->handle =
        CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    need(c->handle != INVALID_HANDLE_VALUE, "This game folder is already being compiled by another process.");
#else
    c->handle = open(lock.c_str(), O_CREAT | O_RDWR, 0600);
    need(c->handle >= 0 && flock(c->handle, LOCK_EX | LOCK_NB) == 0,
         "This game folder is already being compiled by another process.");
#endif
    plain_tree(c->root);
    return c;
}
bool library_name(const std::string &s) {
    return s == "lasgame.dll" || s == "lasgame.so" || s == "lasgame.dylib";
}
void recover(BuildCache *c) {
    auto journal = c->root / "install-journal";
    if (!fs::exists(journal))
        return;
    std::istringstream in(read(journal, 256));
    std::string magic, name;
    int had_lib, had_id;
    need(bool(in >> magic >> name >> had_lib >> had_id) && magic == "LASINSTALL1" && library_name(name) &&
             (had_lib == 0 || had_lib == 1) && (had_id == 0 || had_id == 1),
         "Invalid interrupted library installation.");
    const char *old[] = {"previous-library", "previous-id"};
    for (int i = 0; i < 2; ++i) {
        auto destination = c->root.parent_path() / (name + (i ? ".id" : ""));
        plain(destination);
        if (i ? had_id : had_lib) {
            auto tmp = c->root / (std::string(old[i]) + ".restore");
            plain(tmp);
            fs::copy_file(c->root / old[i], tmp, fs::copy_options::overwrite_existing);
            replace(tmp, destination);
        } else if (fs::exists(destination))
            fs::remove(destination);
    }
    fs::remove(journal);
}
} // namespace
extern "C" BuildCache *build_cache_open(const char *game_dir, const char *spec, const char *const *inputs, int n,
                                        char *why, size_t cap) {
    try {
        auto c = acquire(game_dir);
        auto owner = c->root / "owner";
        if (fs::exists(owner))
            need(read(owner, 64) == "LASBUILD3\n", "Unknown build cache; clear it from Game Files and retry.");
        else {
            for (auto &e : fs::directory_iterator(c->root))
                need(e.path().filename() == ".lock", "Old build cache; clear it from Game Files and retry.");
            write(owner, "LASBUILD3\n");
        }
        recover(c.get());
        CacheSha256 key;
        key.add(spec, strlen(spec) + 1);
        for (int i = 0; i < n; ++i) {
            auto h = digest(fs::canonical(fs::u8path(inputs[i])));
            key.add(h.data(), h.size());
        }
        std::string identity = "LASBUILD3\n" + key.finish() + "\n";
        auto record = c->root / "identity";
        if (!fs::exists(record) || read(record, 128) != identity) {
            for (auto &e : fs::directory_iterator(c->root))
                if (e.path().filename() != ".lock" && e.path().filename() != "owner")
                    fs::remove_all(e.path());
            write(record, identity);
        }
        return c.release();
    } catch (const std::exception &e) {
        error(why, cap, e);
        return NULL;
    }
}
// A changed generator invalidates source receipts, but retains object receipts to compare regenerated C.
extern "C" bool build_cache_prepare_generation(BuildCache *c, const char *spec, const char *const *inputs, int n,
                                                char *why, size_t cap) {
    try {
        CacheSha256 key;
        key.add(spec, strlen(spec) + 1);
        for (int i = 0; i < n; ++i) {
            auto h = digest(fs::canonical(fs::u8path(inputs[i])));
            key.add(h.data(), h.size());
        }
        std::string identity = "LASGENKEY1\n" + key.finish() + "\n";
        auto record = c->root / "generation-identity";
        if (!fs::exists(record) || read(record, 128) != identity) {
            for (const char *name : {"main.generation", "sdk.generation", "main", "sdk"})
                fs::remove_all(c->root / name);
            write(record, identity);
        }
        return true;
    } catch (const std::exception &e) {
        error(why, cap, e);
        return false;
    }
}
extern "C" void build_cache_close(BuildCache *c) {
    delete c;
}
extern "C" bool build_cache_generation(BuildCache *c, const char *module, int *chunks) {
    try {
        need(!strcmp(module, "main") || !strcmp(module, "sdk"), "Invalid module name.");
        auto record = read(c->root / (std::string(module) + ".generation"));
        std::istringstream in(record);
        std::string magic;
        int n;
        need(bool(in >> magic >> n) && magic == "LASGEN1", "Invalid generation record.");
        if (record != generation(c, module, n))
            return false;
        *chunks = n;
        return true;
    } catch (...) {
        return false;
    }
}
extern "C" bool build_cache_save_generation(BuildCache *c, const char *module, int chunks, char *why, size_t cap) {
    try {
        auto data = generation(c, module, chunks);
        write(c->root / (std::string(module) + ".generation"), data);
        return true;
    } catch (const std::exception &e) {
        error(why, cap, e);
        return false;
    }
}
extern "C" int build_cache_object(BuildCache *c, const char *stem, const char *source, char source_hash[65]) {
    try {
        auto h = digest(fs::u8path(source));
        memcpy(source_hash, h.c_str(), 65);
    } catch (...) {
        return -1;
    }
    try {
        need(valid_stem(stem), "Invalid object name.");
        std::string expected =
            "LASOBJ1\n" + std::string(source_hash) + "\n" + digest(c->root / (std::string(stem) + ".o")) + "\n";
        return read(c->root / (std::string(stem) + ".receipt"), 256) == expected ? 1 : 0;
    } catch (...) {
        return 0;
    }
}
extern "C" bool build_cache_save_object(BuildCache *c, const char *stem, const char *source, const char *source_hash,
                                        char *why, size_t cap) {
    try {
        need(valid_stem(stem), "Invalid object name.");
        need(digest(fs::u8path(source)) == source_hash, "Generated source changed while being compiled.");
        auto tmp = c->root / (std::string(stem) + ".o.tmp"), obj = c->root / (std::string(stem) + ".o");
        need(fs::file_size(tmp) > 0, "The compiler did not produce an object.");
        auto hash = digest(tmp);
        replace(tmp, obj);
        write(c->root / (std::string(stem) + ".receipt"), "LASOBJ1\n" + std::string(source_hash) + "\n" + hash + "\n");
        return true;
    } catch (const std::exception &e) {
        error(why, cap, e);
        return false;
    }
}
extern "C" bool build_cache_publish(BuildCache *c, const char *name, const char *runtime_id,
                                    char *why, size_t cap) {
    try {
        need(library_name(name), "Invalid game library filename.");
        auto lib = c->root.parent_path() / name, id = lib;
        id += ".id";
        plain(lib);
        plain(id);
        auto tmp = c->root / (std::string(name) + ".tmp");
        need(fs::file_size(tmp) > 0, "The linker did not produce a game library.");
        plain(tmp);
        write(c->root / "next-id", std::string(runtime_id) + "\n");
        bool had_lib = fs::exists(lib), had_id = fs::exists(id);
        if (had_lib)
            fs::copy_file(lib, c->root / "previous-library", fs::copy_options::overwrite_existing);
        if (had_id)
            fs::copy_file(id, c->root / "previous-id", fs::copy_options::overwrite_existing);
        write(c->root / "install-journal",
              std::string("LASINSTALL1\n") + name + "\n" + (had_lib ? "1" : "0") + " " + (had_id ? "1" : "0") + "\n");
        replace(tmp, lib);
        replace(c->root / "next-id", id);
        fs::remove(c->root / "install-journal");
        std::error_code ec;
        fs::remove(c->root / "previous-library", ec);
        fs::remove(c->root / "previous-id", ec);
        return true;
    } catch (const std::exception &e) {
        error(why, cap, e);
        try {
            recover(c);
        } catch (const std::exception &rollback) {
            error(why, cap, rollback);
        }
        return false;
    }
}

extern "C" bool build_cache_clear(const char *game_dir, char *why, size_t cap) {
    try {
        auto c = acquire(game_dir);
        // Finish rollback before deleting the only copy of a previously working library.
        recover(c.get());
        // Explicit cleanup is also how old, pre-receipt build directories are upgraded.
        for (auto &e : fs::directory_iterator(c->root))
            if (e.path().filename() != ".lock")
                fs::remove_all(e.path());
        write(c->root / "owner", "LASBUILD3\n");
        return true;
    } catch (const std::exception &e) {
        error(why, cap, e);
        return false;
    }
}
