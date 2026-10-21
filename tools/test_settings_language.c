// Exercise the real settings load/resolve/save path without starting the game.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "settings.h"

// Unrelated cosmetics/input settings are not part of these fixtures.
bool bind_settings_load(const char *key, const char *val) { (void)key; (void)val; return false; }
void bind_settings_save(FILE *file) { (void)file; }

int main(int argc, char **argv) {
    assert(argc == 3 || argc == 4);
    settings_load();
    assert(!strcmp(settings_language(), argv[1]));
    assert(!strcmp(settings_language_name(), argv[2]));
    if (argc == 4) assert(g_settings.game_profile == atoi(argv[3]));
    settings_save();
    return 0;
}
