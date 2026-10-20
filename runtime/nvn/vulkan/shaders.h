// Shader compilation and reflection for the Vulkan backend (shaders.c).
#pragma once
#include <stddef.h>

#include "vk.h"

typedef struct { uint32_t set, binding; VkDescriptorType type; } VkBindingInfo;
typedef struct {
    VkShaderModule module;
    VkShaderModule module_late; // fragment variant without early tests, owned by the same cache entry
    VkBindingInfo binds[128];
    int nbinds;
} VkPreparedShader;

void vk_shaders_init(void);
bool vk_shaders_ready(void);
// Identical stages share compilation, reflection and their module across programs.
const VkPreparedShader *vk_prepare_shader(const char *src, int stage);
void vk_shader_cache_reset(void); // all preparation jobs joined; no new callers
uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords);  // malloc'd, NULL on error
VkShaderModule vk_module(const uint32_t *spv, size_t nwords);
// Descriptor bindings declared by a SPIR-V module.
int vk_reflect(const uint32_t *spv, size_t nwords, VkBindingInfo *out, int max);
VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages);
