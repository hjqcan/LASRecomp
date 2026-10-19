// Some drivers expose depth attachments and transfers without BLIT_DST. Keep
// nearest depth/stencil copies on the GPU using raw aspect transfers in that case.
#include "vk.h"

static VkDescriptorSetLayout depth_copy_set;
static VkPipelineLayout depth_copy_layout;
static VkPipeline depth_copy_pipeline;
static const char depth_copy_shader[] =
    "#version 450\n"
    "layout(local_size_x=64) in;\n"
    "layout(set=0,binding=0,std430) readonly buffer Source { uint src[]; };\n"
    "layout(set=0,binding=1,std430) writeonly buffer Target { uint dst[]; };\n"
    "layout(push_constant) uniform Params { uint sw,sh,dw,dh,bytes; } p;\n"
    "void main() {\n"
    " uint word=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*64u, first=word*4u;\n"
    " if(first>=p.dw*p.dh*p.bytes) return;\n"
    " uint result=0u;\n"
    " for(uint lane=0u;lane<4u;lane++) {\n"
    "  uint byte=first+lane, pixel=byte/p.bytes;\n"
    "  if(pixel>=p.dw*p.dh) break;\n"
    // Integer texel-center mapping avoids float roundoff at odd dimensions.
    "  uint x=pixel%p.dw, y=pixel/p.dw;\n"
    "  uint sx=((2u*x+1u)*p.sw)/(2u*p.dw);\n"
    "  uint sy=((2u*y+1u)*p.sh)/(2u*p.dh);\n"
    "  uint at=(sy*p.sw+sx)*p.bytes+byte%p.bytes;\n"
    "  result|=((src[at/4u]>>((at%4u)*8u))&255u)<<(lane*8u);\n"
    " }\n"
    " dst[word]=result;\n"
    "}\n";

static uint32_t aspect_bytes(VkFormat format, VkImageAspectFlags aspect) {
    if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) return 1;
    if (aspect != VK_IMAGE_ASPECT_DEPTH_BIT) return 0;
    switch (format) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_D16_UNORM_S8_UINT: return 2;
    case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return 4;
    default: return 0;
    }
}

static void copy_depth_via_buffer(VkCommandBuffer cb, const TexBackend *src, const TexBackend *dst,
                                  const VkImageBlit *region) {
    uint32_t bytes=aspect_bytes(src->format,region->srcSubresource.aspectMask);
    int sw=region->srcOffsets[1].x-region->srcOffsets[0].x;
    int sh=region->srcOffsets[1].y-region->srcOffsets[0].y;
    int dw=region->dstOffsets[1].x-region->dstOffsets[0].x;
    int dh=region->dstOffsets[1].y-region->dstOffsets[0].y;
    if (!bytes || src->format!=dst->format || src->samples!=1 || dst->samples!=1 ||
        region->srcSubresource.aspectMask!=region->dstSubresource.aspectMask ||
        region->srcSubresource.layerCount!=1 || region->dstSubresource.layerCount!=1 ||
        sw<=0 || sh<=0 || dw<=0 || dh<=0 || sw>16384 || sh>16384 || dw>16384 || dh>16384 ||
        region->srcOffsets[1].z-region->srcOffsets[0].z!=1 ||
        region->dstOffsets[1].z-region->dstOffsets[0].z!=1)
        FATAL("Unsupported Vulkan depth/stencil copy geometry or format\n");
    if (!depth_copy_layout) {
        VkDescriptorSetLayoutBinding bindings[2]={
            {0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL},
            {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,NULL}};
        VkDescriptorSetLayoutCreateInfo si={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount=2,.pBindings=bindings};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev,&si,NULL,&depth_copy_set));
        VkPushConstantRange push={VK_SHADER_STAGE_COMPUTE_BIT,0,5*sizeof(uint32_t)};
        VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount=1,.pSetLayouts=&depth_copy_set,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev,&li,NULL,&depth_copy_layout));
        VkShaderModule module=vk_compile_glsl(depth_copy_shader,STAGE_COMPUTE,"depth-copy.comp");
        if (!module) FATAL("Vulkan depth/stencil copy shader failed\n");
        VkComputePipelineCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .layout=depth_copy_layout,.stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage=VK_SHADER_STAGE_COMPUTE_BIT,.module=module,.pName="main"}};
        VK_CHECK(vk_create_compute_pipelines(1,&pi,&depth_copy_pipeline));
        vkDestroyShaderModule(g_vk.dev,module,NULL);
        LOG("[vk] depth/stencil scaled copies use GPU buffer resampling\n");
    }
    uint64_t sizes[2]={((uint64_t)sw*sh*bytes+3)&~3ull,((uint64_t)dw*dh*bytes+3)&~3ull};
    if (sizes[0]>g_vk.props.limits.maxStorageBufferRange || sizes[1]>g_vk.props.limits.maxStorageBufferRange)
        FATAL("Vulkan depth/stencil copy exceeds the storage-buffer range\n");
    uint64_t align=g_vk.props.limits.minStorageBufferOffsetAlignment;
    VkSpan buffers[2]={vk_transient(sizes[0],align),vk_transient(sizes[1],align)};
    VkBufferImageCopy from={.bufferOffset=buffers[0].off,.imageSubresource=region->srcSubresource,
        .imageOffset=region->srcOffsets[0],.imageExtent={(uint32_t)sw,(uint32_t)sh,1}};
    vkCmdCopyImageToBuffer(cb,src->image,VK_IMAGE_LAYOUT_GENERAL,buffers[0].buf,1,&from);
    vk_full_barrier(cb);
    VkDescriptorSet set=vk_alloc_set(depth_copy_set);
    VkDescriptorBufferInfo bi[2]={{buffers[0].buf,buffers[0].off,sizes[0]},
                                 {buffers[1].buf,buffers[1].off,sizes[1]}};
    VkWriteDescriptorSet writes[2];
    for (unsigned i=0;i<2;i++) writes[i]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&bi[i]};
    vkUpdateDescriptorSets(g_vk.dev,2,writes,0,NULL);
    uint32_t params[5]={(uint32_t)sw,(uint32_t)sh,(uint32_t)dw,(uint32_t)dh,bytes};
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,depth_copy_pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,depth_copy_layout,0,1,&set,0,NULL);
    vkCmdPushConstants(cb,depth_copy_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof params,params);
    uint32_t groups=(uint32_t)((sizes[1]/4+63)/64), gx=groups;
    if (gx>g_vk.props.limits.maxComputeWorkGroupCount[0]) gx=g_vk.props.limits.maxComputeWorkGroupCount[0];
    uint32_t gy=(groups+gx-1)/gx;
    if (gy>g_vk.props.limits.maxComputeWorkGroupCount[1]) FATAL("Vulkan depth/stencil copy dispatch is too large\n");
    vkCmdDispatch(cb,gx,gy,1);
    vk_full_barrier(cb);
    VkBufferImageCopy to={.bufferOffset=buffers[1].off,.imageSubresource=region->dstSubresource,
        .imageOffset=region->dstOffsets[0],.imageExtent={(uint32_t)dw,(uint32_t)dh,1}};
    vkCmdCopyBufferToImage(cb,buffers[1].buf,dst->image,VK_IMAGE_LAYOUT_GENERAL,1,&to);
    vk_full_barrier(cb);
}

void vk_blit_depth_nearest(VkCommandBuffer cb, const TexBackend *src, const TexBackend *dst,
                           const VkImageBlit *region) {
    static struct { VkFormat format; bool blit; } formats[16];
    static unsigned count;
    bool blit=false,known=false;
    for (unsigned i=0;i<count;i++) if (formats[i].format==src->format) { blit=formats[i].blit;known=true;break; }
    if (!known) {
        VkFormatProperties p;vkGetPhysicalDeviceFormatProperties(g_vk.phys,src->format,&p);
        VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;
        blit=(p.optimalTilingFeatures&needed)==needed;
        if (count<16) { formats[count].format=src->format;formats[count++].blit=blit; }
    }
    if (src->format==dst->format && blit)
        vkCmdBlitImage(cb,src->image,VK_IMAGE_LAYOUT_GENERAL,dst->image,VK_IMAGE_LAYOUT_GENERAL,1,region,VK_FILTER_NEAREST);
    else copy_depth_via_buffer(cb,src,dst,region);
}
