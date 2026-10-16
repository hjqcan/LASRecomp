#include "build_cache.h"
#include "ui/cache_sha256.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
namespace fs = std::filesystem;
static void write(const fs::path &p, const std::string &s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
static std::string read(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
int main(int argc, char **argv) {
    assert(argc == 2);
    CacheSha256 empty;
    assert(empty.finish() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CacheSha256 abc;
    abc.add("a", 1);
    abc.add("bc", 2);
    assert(abc.finish() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CacheSha256 million;
    std::string thousand(1000, 'a');
    for (int i = 0; i < 1000; ++i)
        million.add(thousand.data(), thousand.size());
    assert(million.finish() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    fs::path root = fs::u8path(argv[1]);
    fs::create_directories(root);
    std::string data = (root / "input").u8string(), game = (root / "extracted").u8string();
    write(fs::u8path(data), "binary input");
    const char *inputs[] = {data.c_str()};
    char why[1024] = "", hash[65];
    auto open = [&](const char *spec = "compiler and flags") {
        return build_cache_open(game.c_str(), spec, inputs, 1, why, sizeof why);
    };
    BuildCache *cache = open();
    assert(cache);
    assert(!open());
    assert(!build_cache_clear(game.c_str(), why, sizeof why)); // another process/window cannot erase an active build
    fs::path work = fs::u8path(game) / ".gamebuild";
    assert(build_cache_prepare_generation(cache, "generator 1", inputs, 1, why, sizeof why));
    write(work / "main/main_000.c", "int example;");
    write(work / "main/main_table.c", "int table;");
    auto source = (work / "main/main_000.c").u8string();
    int chunks = 0;
    assert(!build_cache_generation(cache, "main", &chunks));
    assert(build_cache_save_generation(cache, "main", 1, why, sizeof why));
    assert(build_cache_generation(cache, "main", &chunks) && chunks == 1);
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 0);
    write(work / "main_000.o.tmp", "partial or completed object");
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 0); // no success receipt yet
    assert(build_cache_save_object(cache, "main_000", source.c_str(), hash, why, sizeof why));
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 1);
    build_cache_close(cache);
    cache = open();
    assert(cache);
    assert(build_cache_generation(cache, "main", &chunks));
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 1);
    assert(build_cache_prepare_generation(cache, "generator 2", inputs, 1, why, sizeof why));
    assert(!build_cache_generation(cache, "main", &chunks));
    assert(fs::exists(work / "main_000.receipt"));
    write(work / "main/main_000.c", "int example;");
    write(work / "main/main_table.c", "int table;");
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 1);
    assert(build_cache_save_generation(cache, "main", 1, why, sizeof why));
    write(work / "main_000.o", "damaged object");
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 0);
    write(work / "main_000.o.tmp", "repaired object");
    assert(build_cache_save_object(cache, "main_000", source.c_str(), hash, why, sizeof why));
    write(fs::u8path(source), "changed source");
    assert(!build_cache_generation(cache, "main", &chunks));
    assert(!build_cache_save_object(cache, "main_000", source.c_str(), hash, why, sizeof why));
    write(fs::u8path(source), "int example;");
    assert(build_cache_object(cache, "main_000", source.c_str(), hash) == 1);
    write(fs::u8path(game) / "lasgame.dll", "old library");
    write(fs::u8path(game) / "lasgame.dll.id", "old id");
    assert(!build_cache_publish(cache, "lasgame.dll", "new-runtime", why, sizeof why));
    assert(read(fs::u8path(game) / "lasgame.dll") == "old library");
    write(work / "lasgame.dll.tmp", "new library");
    assert(build_cache_publish(cache, "lasgame.dll", "new-runtime", why, sizeof why));
    assert(read(fs::u8path(game) / "lasgame.dll.id") == "new-runtime\n");
    // Simulate a crash between installing the library and its metadata, then restart.
    write(work / "previous-library", "old library");
    write(work / "previous-id", "old id");
    write(work / "install-journal", "LASINSTALL1\nlasgame.dll\n1 1\n");
    build_cache_close(cache);
    cache = open();
    assert(cache);
    assert(read(fs::u8path(game) / "lasgame.dll") == "old library");
    assert(read(fs::u8path(game) / "lasgame.dll.id") == "old id");
    build_cache_close(cache);
    write(fs::u8path(data), "different binary input");
    cache = open();
    assert(cache);
    assert(!fs::exists(work / "main_000.receipt"));
    assert(!build_cache_generation(cache, "main", &chunks));
    write(work / "marker", "stale object");
    build_cache_close(cache);
    cache = open("different compiler or flags");
    assert(cache && !fs::exists(work / "marker"));
    build_cache_close(cache);
    // Clearing after interrupted publication must recover before discarding rollback files.
    write(work / "previous-library", "old library");
    write(work / "previous-id", "old id");
    write(work / "install-journal", "LASINSTALL1\nlasgame.dll\n1 1\n");
    write(fs::u8path(game) / "lasgame.dll", "incompletely published library");
    assert(build_cache_clear(game.c_str(), why, sizeof why));
    assert(read(fs::u8path(game) / "lasgame.dll") == "old library");
    assert(read(fs::u8path(game) / "lasgame.dll.id") == "old id");
    std::error_code ec;
    fs::create_directory_symlink(root / "extracted", root / "linked", ec);
    if (!ec) {
        assert(!build_cache_open((root / "linked").u8string().c_str(), "flags", inputs, 1, why, sizeof why));
    }
    std::cout << "SHA-256, cache resume, locks, corruption, input invalidation, publish recovery and path isolation "
                 "passed\n";
}
