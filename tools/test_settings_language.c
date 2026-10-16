// Exercise the real settings load/resolve/save path without starting the game.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "settings.h"

// Unrelated cosmetics/input settings are not part of these fixtures.
const char *const g_tunic_colours[N_TUNIC_COLOURS] = {"Default"};
const char *const g_heart_colours[N_HEART_COLOURS] = {"Default"};
bool bind_settings_load(const char *key, const char *val) { (void)key; (void)val; return false; }
void bind_settings_save(FILE *file) { (void)file; }

int main(int argc, char **argv) {
    assert(argc == 3);
    settings_load();
    assert(!strcmp(settings_language(), argv[1]));
    assert(!strcmp(settings_language_name(), argv[2]));
    settings_save();
    return 0;
}
