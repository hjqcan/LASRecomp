#include "save_backup.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
namespace fs = std::filesystem;
static void write(const fs::path &p, const std::string &s) { fs::create_directories(p.parent_path()); std::ofstream(p, std::ios::binary) << s; }
static std::string read(const fs::path &p) { std::ifstream in(p, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
int main(int argc, char **argv) {
    assert(argc >= 2);
    std::string path = argv[1];
    fs::path root = fs::u8path(path), store = fs::u8path(path + ".backups");
    char why[1024]; SaveBackupInfo items[30];
    auto create = [&](bool dedup = false) { return save_backup_create(path.c_str(), "Manual", "1.0.0", dedup, why, sizeof why); };
    auto list = [&] { return save_backup_list(path.c_str(), items, 30, why, sizeof why); };
    if (argc > 2) { // optional real, scratch-only save roundtrip driven by the test runner
        assert(create()); assert(list() == 1);
        std::string id = items[0].id;
        assert(save_backup_restore(path.c_str(), id.c_str(), "1.0.0", why, sizeof why));
        std::cout << "Scratch save roundtrip passed\n"; return 0;
    }
    assert(!create()); assert(!fs::exists(root));
    write(root / "savedata0.bin", "original slot");
    write(root / "screenshots" / "slot 0.bin", std::string(5000, '\x80'));
    assert(create()); assert(list() == 1 && items[0].files == 2 && items[0].bytes == 5013);
    std::string first = items[0].id;
    assert(create(true)); assert(list() == 1); // identical autosaves do not consume retention
    write(root / "savedata0.bin", "new progress");
    assert(save_backup_restore(path.c_str(), first.c_str(), "1.0.0", why, sizeof why));
    assert(read(root / "savedata0.bin") == "original slot");
    assert(list() == 2 && std::string(items[0].reason) == "Before restore");
    auto damaged = store / first / "files" / "savedata0.bin";
    write(damaged, "corrupt bytes"); // same size, checksum must detect this
    assert(!save_backup_restore(path.c_str(), first.c_str(), "1.0.0", why, sizeof why));
    assert(read(root / "savedata0.bin") == "original slot");
    write(damaged, "original slot");
    auto manifest = store / first / "manifest.txt";
    auto original_manifest = read(manifest);
    write(manifest, "LASBACKUP1\n\"Manual\"\n\"1.0.0\"\n0 10 \"../outside\"\n");
    assert(!save_backup_restore(path.c_str(), first.c_str(), "1.0.0", why, sizeof why));
    write(manifest, original_manifest + "123 "); // truncated manifest must also fail
    assert(!save_backup_restore(path.c_str(), first.c_str(), "1.0.0", why, sizeof why));
    write(manifest, original_manifest);
    assert(!save_backup_restore(path.c_str(), "../outside", "1.0.0", why, sizeof why));
    assert(read(root / "savedata0.bin") == "original slot");
    std::error_code ec;
    fs::create_symlink(root / "savedata0.bin", root / "linked.bin", ec);
    if (!ec) { assert(!create()); fs::remove(root / "linked.bin"); }
    write(root / "oversize.bin", ""); fs::resize_file(root / "oversize.bin", (16u << 20) + 1);
    assert(!create()); fs::remove(root / "oversize.bin");
    for (int i = 0; i < 25; ++i) { write(root / "savedata0.bin", std::to_string(i)); assert(create()); }
    assert(list() == SAVE_BACKUP_KEEP);
    assert(!fs::exists(store / first));
    std::string last = items[0].id;
    // A failed copy must leave source and existing snapshots intact.
    fs::rename(root, root.string() + ".hidden");
    write(root, "not a directory");
    assert(!create()); assert(list() == SAVE_BACKUP_KEEP);
    assert(!save_backup_restore(path.c_str(), last.c_str(), "1.0.0", why, sizeof why));
    assert(read(root) == "not a directory");
    fs::remove(root); fs::rename(root.string() + ".hidden", root);
    fs::remove_all(root); fs::create_directory(root);
    assert(save_backup_restore(path.c_str(), last.c_str(), "1.0.0", why, sizeof why));
    assert(read(root / "savedata0.bin") == "24");
    assert(!fs::exists(store / ".pending"));
    // Profile roots used by the launcher must never share history or restore data.
    auto vanilla = root.parent_path() / "profiles" / "save" / "save_0";
    auto seed_a = root.parent_path() / "profiles" / "rando" / "seed-a" / "save" / "save_0";
    auto seed_b = root.parent_path() / "profiles" / "rando" / "seed-b" / "save" / "save_0";
    for (const auto &profile : {vanilla, seed_a, seed_b}) {
        write(profile / "slot.bin", profile.generic_u8string());
        assert(save_backup_create(profile.u8string().c_str(), "Manual", "1.0.0", false, why, sizeof why));
    }
    assert(save_backup_list(seed_a.u8string().c_str(), items, 30, why, sizeof why) == 1);
    std::string seed_id = items[0].id;
    write(seed_a / "slot.bin", "changed seed a");
    assert(save_backup_restore(seed_a.u8string().c_str(), seed_id.c_str(), "1.0.0", why, sizeof why));
    for (const auto &profile : {vanilla, seed_a, seed_b})
        assert(read(profile / "slot.bin") == profile.generic_u8string());
    assert(save_backup_list(vanilla.u8string().c_str(), items, 30, why, sizeof why) == 1);
    assert(save_backup_list(seed_b.u8string().c_str(), items, 30, why, sizeof why) == 1);
    std::cout << "Save backups: roundtrip, retention, deduplication, pre-restore safety, corruption, traversal, limits and empty-save restore passed\n";
}
