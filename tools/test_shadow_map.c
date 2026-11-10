// The Shadows setting (issue #602): which texture is the shadow map, its scale, and the shader patch that keeps the
// taps one texel apart (runtime/nvn/maxwell/translate.c shadow_taps). No game files, no window. Build + run:
// tools/test_shadow_map.py.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/settings.h"
#include "../runtime/nvn/output_scale.h"
#include "../runtime/nvn/maxwell/shader.h"

Settings g_settings;
bool g_gpu_integrated;
void pace_hitch(const char *why) { (void)why; }
void pace_settings_changed(const char *what) { (void)what; }

// A fragment shader in the game's shape: the x taps' offsets from c4[0x240] and, when `compare`, a compare lookup on a
// 2D array. Without the lookup a read of c4[0x240] is some other constant.
static char *translate(int stage, bool glsl, bool compare) {
    static uint8_t blob[0x80 + 8 * 16];  // (the zero program header first)
    memset(blob, 0, sizeof blob);
    uint64_t *code = (uint64_t *)(blob + 0x80);
    code[1] = 0x49a108900907000dull;  // ffma r13, r0, -c4[0x240], r17
    code[2] = 0x49a00d1009170012ull;  // ffma r18, r0, c4[0x244], r26
    if (compare) code[3] = 0xc03e0120b1770c0dull, code[5] = 0xe30000000007000full;  // tex, depth compare, 2D array; exit
    else code[3] = 0xe30000000007000full;  // exit (code[4] is the next control word)
    ShaderKey key;
    memset(&key, 0, sizeof key);
    key.glsl = glsl;
    key.flip_y = true;
    ShaderInfo *s = maxwell_translate(blob, 0x80 + 8 * 6, stage, &key);
    if (!s->msl) { fprintf(stderr, "translate failed: %s\n", s->error); exit(1); }
    return strdup(s->msl);
}


int main(void) {
    // the game's shadow map and nothing near it
    assert(output_scale_is_shadow_map(1, 1, 2048, 2048, 0x35, true));
    assert(output_scale_is_shadow_map(1, 0, 2048, 2048, 0x35, true));
    assert(!output_scale_is_shadow_map(1, 1, 2048, 2048, 0x35, false));  // (a depth texture that's no render target)
    assert(!output_scale_is_shadow_map(1, 1, 2048, 2048, 0x36, true));
    assert(!output_scale_is_shadow_map(4, 1, 2048, 2048, 0x35, true));
    assert(!output_scale_is_shadow_map(1, 4, 2048, 2048, 0x35, true));
    assert(!output_scale_is_shadow_map(1, 1, 1920, 1080, 0x35, true));
    assert(!output_scale_is_shadow_map(1, 1, 1024, 1024, 0x35, true));

    // Default is the Switch's size, 2x to 4x that many times it; an out-of-range value is Default
    g_settings.shadow_quality = SHADOW_DEFAULT;
    assert(output_scale_shadow() == 1);
    g_settings.shadow_quality = SHADOW_2X;
    assert(output_scale_shadow() == 2);
    g_settings.shadow_quality = SHADOW_3X;
    assert(output_scale_shadow() == 3);
    g_settings.shadow_quality = SHADOW_4X;
    assert(output_scale_shadow() == 4);
    g_settings.shadow_quality = SHADOW_HALF;
    assert(output_scale_shadow() == 0.5f && !output_scale_shadow_off());
    g_settings.shadow_quality = 7;
    assert(output_scale_shadow() == 1);
    assert(!output_scale_shadow_off());
    g_settings.shadow_quality = SHADOW_OFF;  // (Off: the map stays at the Switch's size, nothing drawn into it)
    assert(output_scale_shadow() == 1 && output_scale_shadow_off());

    // the texel size follows the map's scale in both shader languages, for the slot the compare lookup uses
    char *m = translate(STAGE_FRAGMENT, false, true);
    assert(strstr(m, "uint stap0 = as_type<uint>(as_type<float>(cb4[36][0]) / rscale[1][0]);"));
    assert(strstr(m, "uint stap1 = as_type<uint>(as_type<float>(cb4[36][1]) / rscale[1][0]);"));
    assert(strstr(m, "as_type<float>(stap0)"));
    char *g = translate(STAGE_FRAGMENT, true, true);
    assert(strstr(g, "uint stap0 = floatBitsToUint(uintBitsToFloat(cb4[36][0]) / rscale[1][0]);"));
    // no shadow array in the shader: the constant as it is
    m = translate(STAGE_FRAGMENT, false, false);
    assert(strstr(m, "uint stap0 = cb4[36][0];"));
    assert(!strstr(m, "rscale[1][0]"));
    // vertex shaders are not touched
    m = translate(STAGE_VERTEX, false, false);
    assert(!strstr(m, "stap0") && strstr(m, "cb4[36][0]"));
    puts("shadow map tests passed");
    return 0;
}
