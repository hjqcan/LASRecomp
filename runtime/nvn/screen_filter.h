// Screen filter (Cosmetics tab "Screen filter"), live: applied in the present pass, after the upscaler, to the whole
// game image - the game's own HUD included (the original's look), our overlay menus not. The image is gamma-encoded
// there. Shared by the Metal (metal.m present_src) and Vulkan (vulkan/shaders.c present.frag) present shaders: `c` is
// the colour (float3 / vec3), pf.x the mode.
//   1 Game Boy: the original's four greens (#0F380F #306230 #8BAC0F #9BBC0F), chosen by luminance
//   2 Game Boy Color: more saturated and a little brighter, 6 levels a channel (its 15-bit colours, posterised)
//   3 Grayscale: luminance only, no tint, contrast raised a little (a gentle S-curve)
#pragma once
#include "../settings.h"

enum { SCREEN_FILTER_OFF, SCREEN_FILTER_GB, SCREEN_FILTER_GBC, SCREEN_FILTER_GRAY };

// pf for the present shader (x: the mode)
static inline void screen_filter_params(float pf[4]) {
    pf[0] = (float)g_settings.screen_filter;
    pf[1] = pf[2] = pf[3] = 0;
}

#define SCREEN_FILTER_SRC(V3)                                                                                          \
    "  if (pf.x > 0.5) {\n"                                                                                            \
    "    float l = dot(c, " V3 "(0.299, 0.587, 0.114));\n"                                                             \
    "    if (pf.x < 1.5) {\n"                                                                                          \
    "      float q = clamp((l - 0.06) / 0.66, 0.0, 0.999) * 4.0;\n"                                                    \
    "      c = q < 1.0 ? " V3 "(0.059, 0.220, 0.059) : q < 2.0 ? " V3 "(0.188, 0.384, 0.188) :\n"                      \
    "          q < 3.0 ? " V3 "(0.545, 0.675, 0.059) : " V3 "(0.608, 0.737, 0.059);\n"                                 \
    "    } else if (pf.x < 2.5) {\n"                                                                                   \
    "      c = clamp(mix(" V3 "(l), c, 1.4) * 1.06, 0.0, 1.0);\n"                                                      \
    "      c = floor(c * 5.0 + 0.5) / 5.0;\n"                                                                          \
    "    } else {\n"                                                                                                   \
    "      l = clamp((l - 0.5) * 1.2 + 0.5, 0.0, 1.0);\n"                                                              \
    "      c = " V3 "(l * l * (3.0 - 2.0 * l) * 0.35 + l * 0.65);\n"                                                   \
    "    }\n"                                                                                                          \
    "  }\n"
