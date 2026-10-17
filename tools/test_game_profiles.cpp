#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "../runtime/game_profiles.h"

int main(int argc, char **argv) {
    assert(argc == 2);
    namespace fs = std::filesystem;
    fs::path root = fs::absolute(fs::u8path(argv[1]));
    std::string root_string = root.u8string();
    for (int i = 0; i < GAME_PROFILE_COUNT; i++) {
        fs::path game = root / fs::u8path(game_profile_dir(i));
        fs::create_directories(game);
        std::string game_string = game.u8string();
        assert(game_profile_index(game_string.c_str(), root_string.c_str()) == i);
        assert(game_profile_from_id(game_profile_id(i)) == i);
        game_profile_activate(game_string.c_str(), root_string.c_str());
        assert(game_profile_active() == i);
        if (i) assert(game_profile_match(game_profile_title(i), game_profile_version(i)) == i);
    }
    assert(game_profile_from_id("acnh/1.1.0") == -1);
    assert(game_profile_match("acnh", "1.1.0") == -1);
    assert(game_profile_match("unknown", "1.0.0") == -1);
    assert(game_profile_index((root / "games/acnh/v1.0.0/extra").u8string().c_str(), root_string.c_str()) == -1);
    assert(strcmp(game_profile_save_dir(GAME_PROFILE_BASE), game_profile_save_dir(GAME_PROFILE_ACNH)));
    puts("PASS: stable launch IDs, title/version selection and separate managed game/save folders");
}
