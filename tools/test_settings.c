// Exercise the actual parser/resolver/save path with isolated configuration files.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "settings.h"

const char *const g_tunic_colours[N_TUNIC_COLOURS] = {"Default"};
const char *const g_heart_colours[N_HEART_COLOURS] = {"Default"};
bool bind_settings_load(const char *key, const char *val) { (void)key; (void)val; return false; }
void bind_settings_save(FILE *file) { (void)file; }

int main(int argc, char **argv) {
    assert(argc == 3);
    settings_load();
    assert(!strcmp(settings_language(), argv[1]));
    assert(!strcmp(g_settings.rando_seed, argv[2]));
    assert(g_settings.aspect == ASPECT_ORIGINAL);
    assert(g_settings.upscaler == UPSCALE_BILINEAR);
    assert(g_settings.prompts == PROMPTS_XBOX);
    assert(g_settings.volume == 1.25f);
    settings_save();
    return 0;
}
