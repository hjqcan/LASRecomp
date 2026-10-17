#include "../runtime/game_install.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
namespace fs = std::filesystem;
static std::string read(const fs::path &path) {
    std::ifstream input(path, std::ios::binary); return {std::istreambuf_iterator<char>(input), {}};
}
static bool valid(const char *root, char *why, size_t size) {
    fs::path path = fs::u8path(root);
    if (read(path / "compiled") == "compiled:" + read(path / "content") && !read(path / "content").empty()) return true;
    snprintf(why, size, "Synthetic build is incomplete or mismatched"); return false;
}
int main(int argc, char **argv) {
    assert(argc >= 3);
    const char *root = argv[1], *mode = argv[2];
    char why[512] = {}, resolved[4096];
    if (!strcmp(mode, "watch")) {
        puts("watching"); fflush(stdout);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!fs::exists(fs::u8path(root) / "watch-stop")) {
            if (std::chrono::steady_clock::now() >= deadline) return 3;
            if (!game_install_resolve(root, resolved, sizeof resolved, why, sizeof why)) return 1;
            if (read(fs::u8path(resolved) / "content").empty()) return 2;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 0;
    }
    if (!strcmp(mode, "resolve")) {
        if (!game_install_resolve(root, resolved, sizeof resolved, why, sizeof why)) { fprintf(stderr, "%s\n", why); return 1; }
        puts(resolved); return 0;
    }
    if (!strcmp(mode, "pending")) return game_install_pending(root) ? 0 : 1;
    if (!strcmp(mode, "clear")) return game_install_clear(root, why, sizeof why) ? 0 : 1;
    GameInstall *install = game_install_begin(root, !strcmp(mode, "stage") || !strcmp(mode, "clone"), why, sizeof why);
    if (!install) { fprintf(stderr, "%s\n", why); return 2; }
    fs::path candidate = fs::u8path(game_install_candidate(install));
    bool ok = true;
    if (!strcmp(mode, "stage")) {
        assert(argc == 4);
        std::ofstream(candidate / "content") << argv[3];
    } else if (!strcmp(mode, "complete")) {
        std::ofstream(candidate / "compiled") << "compiled:" << read(candidate / "content");
    } else if (!strcmp(mode, "publish")) {
        ok = game_install_publish(install, "fixture/1.0.0", valid, why, sizeof why);
    } else if (!strcmp(mode, "clone")) {
        assert(game_install_resolve(root, resolved, sizeof resolved, why, sizeof why));
        ok = game_install_clone(install, resolved, why, sizeof why);
    } else if (!strcmp(mode, "resume-clone")) {
        ok = game_install_resume_clone(install, why, sizeof why);
    } else if (!strcmp(mode, "lock")) {
        puts("locked"); fflush(stdout); getchar();
    }
    if (ok) puts(candidate.u8string().c_str()); else fprintf(stderr, "%s\n", why);
    game_install_close(install);
    return ok ? 0 : 1;
}
