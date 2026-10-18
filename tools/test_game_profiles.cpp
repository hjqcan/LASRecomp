#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include "../runtime/game_profiles.h"
extern "C" {
#include "../runtime/settings.h"
Settings g_settings={};
}

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
    int current=game_profile_match("acnh","3.0.1");
    assert(current>0);
    std::string normal=game_profile_dir(current), save=game_profile_save_dir(current);
    g_settings.acnh_speedfix60=true;
    assert(game_profile_dir(current)==normal+"/.acnh60");
    fs::path variant=root/fs::u8path(game_profile_dir(current));fs::create_directories(variant);
    assert(game_profile_index(variant.u8string().c_str(),root_string.c_str())==current);
    assert(game_profile_index((variant/"extra").u8string().c_str(),root_string.c_str())==-1);
    assert(game_profile_index((root/"games/acnh/v1.0.0/.acnh60").u8string().c_str(),root_string.c_str())==-1);
    assert(game_profile_base_dir(current)==normal && game_profile_save_dir(current)==save);
    assert(std::string(game_profile_dir(GAME_PROFILE_BASE)).find(".acnh60")==std::string::npos);
    g_settings.acnh_speedfix60=false;assert(game_profile_dir(current)==normal);
    puts("PASS: stable launch IDs, title/version selection and separate managed game/save folders");
}
