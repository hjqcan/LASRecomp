// Exercise the production allocator/attachment classification without a GPU or game files.
// Fake Vulkan allocations record physical dimensions; late promotion must record a full-image
// preserving blit, not leave the right/bottom of the logical render target untouched.
#include <assert.h>
#include "../runtime/nvn/vulkan/textures.c"

VkG g_vk;
static uintptr_t next_image = 1;
static unsigned creates, blits, clears;
static VkImageBlit last_blit;
uint64_t prof_up_ns, prof_views_created;
// These entry points belong to unrelated upload/alias/sampler paths. Fail if this fixture
// unexpectedly reaches one rather than silently substituting a renderer operation.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
const char *game_level_name(void) { abort(); }
void vk_alias_texture_destroyed(NvnTexture *t) { abort(); }
void vk_alias_reset(void) { abort(); }
void vk_convert_nv12(TexBackend *b, const void *p, uint32_t w, uint32_t h, uint32_t s, uint32_t uv) { abort(); }
// Depth-copy contents are covered by test_depth_copy_gpu; this fixture promotes color images only.
void vk_blit_depth_nearest(VkCommandBuffer cb, const TexBackend *src, const TexBackend *dst, const VkImageBlit *region) { abort(); }
VkSpan vk_transient(uint64_t s, uint64_t a) { abort(); }
VkSpan vk_transient_copy(const void *p, uint64_t s, uint64_t a) { abort(); }
void vk_dl_note_upload(void *i, uint32_t w, uint32_t h, uint32_t lv) { abort(); }
uint64_t vk_frame_counter(void) { abort(); }
int vk_pages_changed(uint64_t a, uint64_t s, uint32_t since, uint32_t *now) { abort(); }
int texpatch_identify(const NvnTexture *t) { abort(); }
uint32_t texpatch_generation(void) { abort(); }
void texpatch_apply(int id, uint32_t lv, uint8_t *p, uint32_t w, uint32_t h, uint32_t row) { abort(); }
void rt_fatal_msg(const char *fmt, ...) { abort(); }
#pragma GCC diagnostic pop
uint64_t renderer_present_count(void) { return 1; }
uint64_t renderer_exec_frame(void) { return 1; }
bool rq_on(void) { return false; }
void rq_upload(NvnTexture *t, int k, void *p, uint32_t w, uint32_t h, uint32_t s, uint32_t o) { abort(); }
VkCommandBuffer vk_cmd(void) { return (VkCommandBuffer)1; }
VkCommandBuffer vk_upload_cmd(void) { return (VkCommandBuffer)2; }
uint64_t vk_pending_seq(void) { return 1; }
uint64_t vk_completed_seq(void) { return 0; }
void vk_full_barrier(VkCommandBuffer cb) { (void)cb; }
void rt_fatal_trace(void) { abort(); }
void vk_device_fault_report(void) { abort(); }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p, VkFormat f, VkFormatProperties *o) {
    (void)p; (void)f; *o = (VkFormatProperties){.optimalTilingFeatures = ~0u};
}
VKAPI_ATTR VkResult VKAPI_CALL vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *i,
    const VmaAllocationCreateInfo *c, VkImage *image, VmaAllocation *m, VmaAllocationInfo *info) {
    (void)a; (void)c; (void)info; assert(i->extent.width && i->extent.height);
    *image = (VkImage)next_image++; *m = (VmaAllocation)next_image++; creates++; return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vmaDestroyImage(VmaAllocator a, VkImage i, VmaAllocation m) { (void)a; (void)i; (void)m; }
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)d; (void)v; (void)a; }
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer c, const VkDependencyInfo *d) { (void)c; (void)d; }
VKAPI_ATTR void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer c, VkImage i, VkImageLayout l,
    const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    (void)c; (void)i; (void)l; (void)r; assert(n == 1 && v->depth == 0 && v->stencil == 0); clears++;
}
VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer c, VkImage i, VkImageLayout l,
    const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    (void)c; (void)i; (void)l; (void)r; assert(n == 1);
    for (unsigned j=0; j<4; j++) assert(v->uint32[j] == 0);
    clears++;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer c, VkImage s, VkImageLayout sl,
    VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) {
    (void)c; (void)sl; (void)dl;
    assert(s != d && n == 1 && f == VK_FILTER_NEAREST); last_blit = *r; blits++;
}

static NvnTexture screen(uint32_t fmt, uint32_t flags) {
    return (NvnTexture){.target=1, .format=fmt, .flags=flags, .width=960, .height=540, .depth=1, .levels=1};
}
int main(void) {
    renderer_set_alloc_scale(2.0f/3, 2.0f/3);
    renderer_set_scale(2.0f/3, 2.0f/3);
    // The ACNH half-size depth attachment and its uncompressed AO/light buffers must match.
    NvnTexture depth = screen(0x34, 8);
    vk_mark_render_target(&depth);
    TexBackend *d = depth.backend;
    assert(d->width == 640 && d->height == 360);
    const uint32_t formats[] = {1, 5, 0x11};
    NvnTexture colors[3];
    for (unsigned i=0; i<3; i++) {
        colors[i] = screen(formats[i], 0);
        unsigned before = creates;
        vk_mark_render_target(&colors[i]);
        TexBackend *b = colors[i].backend;
        assert(b->width == d->width && b->height == d->height);
        assert(creates == before+1 && b->scalable && b->gpu_owned);
        assert(!renderer_texture_frame_fresh(&colors[i])); // retain the game's own clear/load semantics
    }
    // Merely sampled CPU images (including video/captures) retain their original dimensions.
    NvnTexture sampled = screen(0x25, 0);
    TexBackend *b = tex_backend(&sampled);
    assert(b->width == 960 && b->height == 540 && !b->scalable);
    b->uploaded = true;
    vk_mark_render_target(&sampled);
    assert(b->width == 640 && b->height == 360 && blits == 1);
    assert(last_blit.srcOffsets[1].x == 960 && last_blit.srcOffsets[1].y == 540);
    assert(last_blit.dstOffsets[1].x == 640 && last_blit.dstOffsets[1].y == 360);
    vk_mark_render_target(&sampled);
    assert(blits == 1); // usage classification is stable, no repeated reallocation
    renderer_set_alloc_scale(1.5f, 1.5f);
    renderer_set_scale(1.5f, 1.5f);
    vk_mark_render_target(&sampled);
    assert(b->width == 1440 && b->height == 810);
    unsigned allocations = creates;
    renderer_set_scale(0.75f, 0.75f);
    renderer_mark_gpu_owned(&sampled);
    float fx, fy;
    renderer_texture_fraction(&sampled, &fx, &fy);
    assert(creates == allocations && b->width == 1440 && b->height == 810 && fx == 0.5f && fy == 0.5f);
    NvnTexture square = screen(1, 0); square.width = square.height = 512;
    vk_mark_render_target(&square);
    assert(((TexBackend *)square.backend)->width == 512 && !((TexBackend *)square.backend)->scalable);
    NvnTexture array = screen(1, 0); array.target = 4;
    assert(!tex_scalable(&array, true));
    NvnTexture msaa = screen(1, 0); msaa.samples = 4;
    assert(!tex_scalable(&msaa, true));
    assert(clears == creates);
    puts("PASS: Vulkan attachment scaling, matching depth/color extents, sampled images and preserving late promotion");
    return 0;
}
