// The settings menus' plastic pieces on Vulkan (runtime/ui/plastic.mm has the shader and the why): the pipeline the
// pieces draw with inside ImGui's command buffer, and the small blurred copy of the game image (64 x 32) the glare
// guard reads, made only while the menu is open.
#include <string.h>

#include "vk.h"
#include "record.h"

void vk_frame_suspend(void);  // device.c
void vk_frame_resume(void);

static VkPipelineLayout piece_layout, bg_layout;
static VkDescriptorSetLayout piece_set_layout, bg_set_layout;
static VkPipeline piece_pipe, bg_pipe;
static VkFormat piece_fmt;
static VkShaderModule m_vs, m_fs, m_bg_fs, m_bg_vs;
static VkDescriptorPool pool;
static VkDescriptorSet piece_set;  // (the small copy, for the pieces' vertex shader)
static VkImage bg_img;
static VmaAllocation bg_mem;
static VkImageView bg_view;
enum { BG_W = 64, BG_H = 32 };

static VkPipeline make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout, VkFormat fmt, bool blend) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = blend ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs_ = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    if (blend) {  // (premultiplied)
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = g_vk.vids ? 3u : 2u, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs_,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = layout};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[plastic] pipeline failed\n");
    return p;
}

static VkPipelineLayout make_layout(VkDescriptorSetLayout *sl, VkShaderStageFlags set_stages, uint32_t push) {
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, set_stages, NULL};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, sl));
    VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, push};
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = sl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

// Once (the first frame a menu is drawn): shaders, layouts, the small copy's image and the pieces' set. Before the
// caller records anything directly (its barrier goes into the upload command buffer, through the recorder).
bool vk_plastic_init(const char *vs, const char *fs, const char *bg_fs) {
    static int done = -1;
    if (done >= 0) return done;
    done = 0;
    m_vs = vk_compile_glsl(vs, STAGE_VERTEX, "plastic.vert");
    m_fs = vk_compile_glsl(fs, STAGE_FRAGMENT, "plastic.frag");
    m_bg_fs = vk_compile_glsl(bg_fs, STAGE_FRAGMENT, "plastic_bg.frag");
    m_bg_vs = vk_builtin_shader("post.vert");  // (a fullscreen triangle)
    if (!m_vs || !m_fs || !m_bg_fs) return false;
    piece_layout = make_layout(&piece_set_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 112);
    bg_layout = make_layout(&bg_set_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 16);
    bg_pipe = make_pipeline(m_bg_vs, m_bg_fs, bg_layout, VK_FORMAT_R8G8B8A8_UNORM, false);
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {BG_W, BG_H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &bg_img, &bg_mem, NULL) != VK_SUCCESS) return false;
    vk_image_init_layout(vk_upload_cmd(), bg_img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = bg_img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &bg_view));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &pci, NULL, &pool));
    VkDescriptorSetAllocateInfo sa = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
                                      .descriptorSetCount = 1, .pSetLayouts = &piece_set_layout};
    VK_CHECK(vkAllocateDescriptorSets(g_vk.dev, &sa, &piece_set));
    VkDescriptorImageInfo ii = {vk_default_sampler(true), bg_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = piece_set, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    done = bg_pipe != VK_NULL_HANDLE;
    return done;
}

// The small copy of the game image (scene: its view, uv: the part shown), in the window image's command buffer: its
// rendering ends for this pass and begins again after it (contents kept). Then the pieces read it.
void vk_plastic_bg(void *scene, const float uv[4]) {
    if (!bg_pipe || !scene) return;
    VkCommandBuffer cb = vk_cmd();
    vk_frame_suspend();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = bg_view,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {BG_W, BG_H}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    rec_CmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(bg_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), (VkImageView)scene, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    rec_UpdateDescriptorSets(1, &w);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, bg_pipe);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, bg_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, BG_W, BG_H, 0, 1};
    VkRect2D sc = {{0, 0}, {BG_W, BG_H}};
    rec_CmdSetViewport(cb, 0, 1, &vp);
    rec_CmdSetScissor(cb, 0, 1, &sc);
    rec_CmdPushConstants(cb, bg_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, uv);
    if (g_vk.vids) {
        VkVertexInputBindingDescription2EXT b = {.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT};
        VkVertexInputAttributeDescription2EXT a = {.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT};
        rec_CmdSetVertexInputEXT(cb, 0, &b, 0, &a);
    }
    rec_CmdDraw(cb, 3, 1, 0, 0);
    rec_CmdEndRendering(cb);
    vk_full_barrier(cb);
    vk_frame_resume();
}

// One piece, recorded straight into ImGui's command buffer (from its draw callback, on ImGui's thread): push: the
// piece and the frame's values (112 bytes); x, y, w, h: its clip rect; vw, vh: the window image's size
void vk_plastic_draw(VkCommandBuffer cb, const void *push, uint32_t size, int x, int y, uint32_t w, uint32_t h, float vw,
                     float vh, bool has_bg, int repeat) {
    (void)has_bg;
    if (!m_vs) return;
    if (!piece_pipe || piece_fmt != g_vk.sc_format) {
        piece_fmt = g_vk.sc_format;
        piece_pipe = make_pipeline(m_vs, m_fs, piece_layout, piece_fmt, true);
        if (!piece_pipe) return;
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, piece_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, piece_layout, 0, 1, &piece_set, 0, NULL);
    VkViewport vp = {0, 0, vw, vh, 0, 1};
    VkRect2D sc = {{x, y}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, piece_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, size, push);
    if (g_vk.vids && g_vk.set_vertex_input) g_vk.set_vertex_input(cb, 0, NULL, 0, NULL);
    for (int i = 0; i < (repeat < 1 ? 1 : repeat); i++) vkCmdDraw(cb, 4, 1, 0, 0);
}
