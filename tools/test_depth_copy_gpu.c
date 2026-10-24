// Synthetic Vulkan images only. Exercise the production fallback, including
// packed aspect bytes, unequal extents, nonzero mip/layer/offsets and canaries.
#include "../runtime/nvn/vulkan/vk.h"
#include "../runtime/nvn/vulkan/record.h"
#define rec_CmdCopyImageToBuffer vkCmdCopyImageToBuffer
#define rec_CmdBindPipeline vkCmdBindPipeline
#define rec_CmdBindDescriptorSets vkCmdBindDescriptorSets
#define rec_CmdPushConstants vkCmdPushConstants
#define rec_CmdDispatch vkCmdDispatch
#define rec_CmdCopyBufferToImage vkCmdCopyBufferToImage
#define rec_CmdBlitImage vkCmdBlitImage
#define rec_UpdateDescriptorSets(n,w) vkUpdateDescriptorSets(g_vk.dev,n,w,0,NULL)
#include "../runtime/nvn/vulkan/depth_copy.c"
#include <shaderc/shaderc.h>
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

VkG g_vk;
static VkDescriptorPool descriptor_pool;
static unsigned errors;
typedef struct { VkBuffer buffer; VkDeviceMemory memory; void *mapped; } Buffer;
static Buffer buffers[256];
static unsigned buffer_count;
void rt_fatal_msg(const char *fmt,...) { va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap); }
void rt_fatal_trace(void) { abort(); }
void vk_device_fault_report(void) { abort(); }
VkResult vk_create_compute_pipelines(uint32_t n,const VkComputePipelineCreateInfo *info,VkPipeline *out) {
    return vkCreateComputePipelines(g_vk.dev,VK_NULL_HANDLE,n,info,NULL,out);
}
VkShaderModule vk_compile_glsl(const char *source,int stage,const char *name) {
    assert(stage==STAGE_COMPUTE);
    shaderc_compiler_t compiler=shaderc_compiler_initialize();
    shaderc_compilation_result_t result=shaderc_compile_into_spv(compiler,source,strlen(source),shaderc_compute_shader,name,"main",NULL);
    if (shaderc_result_get_compilation_status(result)!=shaderc_compilation_status_success)
        fprintf(stderr,"%s\n",shaderc_result_get_error_message(result));
    assert(shaderc_result_get_compilation_status(result)==shaderc_compilation_status_success);
    VkShaderModuleCreateInfo info={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize=shaderc_result_get_length(result),.pCode=(const uint32_t *)shaderc_result_get_bytes(result)};
    VkShaderModule module;VK_CHECK(vkCreateShaderModule(g_vk.dev,&info,NULL,&module));
    shaderc_result_release(result);shaderc_compiler_release(compiler);return module;
}
static uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(g_vk.phys,&p);
    for(unsigned i=0;i<p.memoryTypeCount;i++) if ((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags) return i;
    abort();
}
VkSpan vk_transient(uint64_t size,uint64_t align) {
    (void)align;assert(buffer_count<256);Buffer *b=&buffers[buffer_count++];
    VkBufferCreateInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,
        .usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VK_CHECK(vkCreateBuffer(g_vk.dev,&info,NULL,&b->buffer));
    VkMemoryRequirements req;vkGetBufferMemoryRequirements(g_vk.dev,b->buffer,&req);
    VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,
        .memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VK_CHECK(vkAllocateMemory(g_vk.dev,&allocation,NULL,&b->memory));
    VK_CHECK(vkBindBufferMemory(g_vk.dev,b->buffer,b->memory,0));
    VK_CHECK(vkMapMemory(g_vk.dev,b->memory,0,size,0,&b->mapped));memset(b->mapped,0xCD,size);
    return (VkSpan){b->buffer,0,b->mapped};
}
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    VkDescriptorSetAllocateInfo info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=descriptor_pool,.descriptorSetCount=1,.pSetLayouts=&layout};
    VkDescriptorSet set;VK_CHECK(vkAllocateDescriptorSets(g_vk.dev,&info,&set));return set;
}
void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo dep={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&barrier};
    vkCmdPipelineBarrier2(cb,&dep);
}
static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,const VkDebugUtilsMessengerCallbackDataEXT *data,void *user) {
    (void)type;(void)user;
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) { errors++;fprintf(stderr,"%s\n",data->pMessage); }
    return VK_FALSE;
}
typedef struct { TexBackend tex; VkDeviceMemory memory; } Image;
static Image make_image(VkFormat format,VkImageAspectFlags aspects,VkCommandBuffer cb) {
    Image im={.tex={.format=format,.width=20,.height=16,.depth=1,.levels=2,.layers=2,.samples=1,.aspect=aspects}};
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=format,
        .extent={20,16,1},.mipLevels=2,.arrayLayers=2,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,
        .usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VK_CHECK(vkCreateImage(g_vk.dev,&ci,NULL,&im.tex.image));
    VkMemoryRequirements req;vkGetImageMemoryRequirements(g_vk.dev,im.tex.image,&req);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,
        .memoryTypeIndex=memory_type(req.memoryTypeBits,0)};
    VK_CHECK(vkAllocateMemory(g_vk.dev,&ai,NULL,&im.memory));VK_CHECK(vkBindImageMemory(g_vk.dev,im.tex.image,im.memory,0));
    VkImageMemoryBarrier2 barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask=VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .image=im.tex.image,.subresourceRange={aspects,0,2,0,2},
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo dep={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier};
    vkCmdPipelineBarrier2(cb,&dep);
    VkClearDepthStencilValue clear={1.0f,0x7E};VkImageSubresourceRange range={aspects,0,2,0,2};
    vkCmdClearDepthStencilImage(cb,im.tex.image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);return im;
}
static uint32_t pattern(VkFormat format,VkImageAspectFlags aspect,unsigned index) {
    if(aspect==VK_IMAGE_ASPECT_STENCIL_BIT)return (index*13+5)&255;
    if(format==VK_FORMAT_D32_SFLOAT_S8_UINT || format==VK_FORMAT_D32_SFLOAT) {
        float v=(index+1)/128.0f;uint32_t bits;memcpy(&bits,&v,4);return bits;
    }
    return (index+1)*257;
}
static void run_case(VkFormat format,VkImageAspectFlags aspect,unsigned sw,unsigned sh,unsigned dw,unsigned dh,bool alias) {
    VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(g_vk.phys,format,&properties);
    VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if((properties.optimalTilingFeatures&needed)!=needed) { printf("SKIP unsupported format %d\n",format);return; }
    VkCommandBufferAllocateInfo alloc={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=g_vk.pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cb;VK_CHECK(vkAllocateCommandBuffers(g_vk.dev,&alloc,&cb));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK_CHECK(vkBeginCommandBuffer(cb,&begin));
    VkImageAspectFlags all=format==VK_FORMAT_D32_SFLOAT_S8_UINT || format==VK_FORMAT_D24_UNORM_S8_UINT ?
        VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:aspect;
    Image src=make_image(format,all,cb),dst=alias?src:make_image(format,all,cb);
    unsigned bytes=aspect_bytes(format,aspect);VkSpan upload=vk_transient(10*8*bytes,16);
    for(unsigned i=0;i<80;i++) { uint32_t value=pattern(format,aspect,i);memcpy((char *)upload.ptr+i*bytes,&value,bytes); }
    VkBufferImageCopy from={.imageSubresource={aspect,1,1,1},.imageExtent={10,8,1}};
    vk_full_barrier(cb);vkCmdCopyBufferToImage(cb,upload.buf,src.tex.image,VK_IMAGE_LAYOUT_GENERAL,1,&from);vk_full_barrier(cb);
    VkImageBlit blit={.srcSubresource={aspect,1,1,1},.srcOffsets={{1,2,0},{1+(int)sw,2+(int)sh,1}},
        .dstSubresource={aspect,0,0,1},.dstOffsets={{3,4,0},{3+(int)dw,4+(int)dh,1}}};
    if(format==VK_FORMAT_D32_SFLOAT_S8_UINT && sw==7)
        vk_blit_depth_nearest(cb,&src.tex,&dst.tex,&blit); // real capability selection
    else copy_depth_via_buffer(cb,&src.tex,&dst.tex,&blit); // exercise fallback on every GPU
    // Check the copied rectangle and untouched pixels, layer, source mip and padding.
    VkSpan readback=vk_transient((20*16*2+80)*bytes+16,16);
    VkBufferImageCopy reads[3]={
        {.imageSubresource={aspect,0,0,1},.imageExtent={20,16,1}},
        {.bufferOffset=20*16*bytes,.imageSubresource={aspect,0,1,1},.imageExtent={20,16,1}},
        {.bufferOffset=20*16*2*bytes,.imageSubresource={aspect,1,1,1},.imageExtent={10,8,1}}};
    vkCmdCopyImageToBuffer(cb,dst.tex.image,VK_IMAGE_LAYOUT_GENERAL,readback.buf,2,reads);
    vkCmdCopyImageToBuffer(cb,src.tex.image,VK_IMAGE_LAYOUT_GENERAL,readback.buf,1,&reads[2]);
    VkMemoryBarrier2 host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT,.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT,.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dep={.sType=VK_STRUCTURE_TYPE_DEPENDENCY_INFO,.memoryBarrierCount=1,.pMemoryBarriers=&host};vkCmdPipelineBarrier2(cb,&dep);
    VK_CHECK(vkEndCommandBuffer(cb));VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    VK_CHECK(vkQueueSubmit(g_vk.queue,1,&submit,VK_NULL_HANDLE));VK_CHECK(vkQueueWaitIdle(g_vk.queue));
    uint32_t mask=aspect==VK_IMAGE_ASPECT_STENCIL_BIT?255:bytes==2?65535:format==VK_FORMAT_D24_UNORM_S8_UINT?0xFFFFFF:0xFFFFFFFF;
    uint32_t clear=aspect==VK_IMAGE_ASPECT_STENCIL_BIT?0x7E:bytes==2?65535:format==VK_FORMAT_D24_UNORM_S8_UINT?0xFFFFFF:0x3F800000;
    for(unsigned layer=0;layer<2;layer++)for(unsigned y=0;y<16;y++)for(unsigned x=0;x<20;x++) {
        uint32_t expected=clear,value=0;
        if(!layer && x>=3 && x<3+dw && y>=4 && y<4+dh) {
            unsigned sx=(unsigned)floor(((x-3)+0.5)*sw/dw)+1,sy=(unsigned)floor(((y-4)+0.5)*sh/dh)+2;
            expected=pattern(format,aspect,sy*10+sx);
        }
        memcpy(&value,(char *)readback.ptr+((layer*16+y)*20+x)*bytes,bytes);
        if((value&mask)!=(expected&mask))fprintf(stderr,"format %d aspect %u at %u,%u,%u: %x != %x\n",format,aspect,layer,x,y,value,expected);
        assert((value&mask)==(expected&mask));
    }
    for(unsigned i=0;i<80;i++) { uint32_t value=0;memcpy(&value,(char *)readback.ptr+(640+i)*bytes,bytes);assert((value&mask)==(pattern(format,aspect,i)&mask)); }
    for(unsigned i=0;i<16;i++)assert(((uint8_t *)readback.ptr)[720*bytes+i]==0xCD);
    vkFreeCommandBuffers(g_vk.dev,g_vk.pool,1,&cb);
    if(!alias) { vkDestroyImage(g_vk.dev,dst.tex.image,NULL);vkFreeMemory(g_vk.dev,dst.memory,NULL); }
    vkDestroyImage(g_vk.dev,src.tex.image,NULL);vkFreeMemory(g_vk.dev,src.memory,NULL);
}
int main(void) {
    uint32_t count=0;vkEnumerateInstanceLayerProperties(&count,NULL);VkLayerProperties *layers=calloc(count,sizeof *layers);
    vkEnumerateInstanceLayerProperties(&count,layers);const char *layer="VK_LAYER_KHRONOS_validation";bool validate=false;
    for(unsigned i=0;i<count;i++)if(!strcmp(layers[i].layerName,layer))validate=true;free(layers);
    const char *extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkDebugUtilsMessengerCreateInfoEXT debug={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
        .pfnUserCallback=validation};
    VkInstanceCreateInfo ii={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,.pNext=validate?&debug:NULL,
        .enabledLayerCount=validate,.ppEnabledLayerNames=&layer,.enabledExtensionCount=validate,.ppEnabledExtensionNames=&extension};
    VK_CHECK(vkCreateInstance(&ii,NULL,&g_vk.inst));VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
    if(validate)VK_CHECK(((PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst,"vkCreateDebugUtilsMessengerEXT"))(g_vk.inst,&debug,NULL,&messenger));
    VK_CHECK(vkEnumeratePhysicalDevices(g_vk.inst,&count,NULL));assert(count);VkPhysicalDevice *devices=calloc(count,sizeof *devices);
    VK_CHECK(vkEnumeratePhysicalDevices(g_vk.inst,&count,devices));g_vk.phys=devices[0];free(devices);vkGetPhysicalDeviceProperties(g_vk.phys,&g_vk.props);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys,&count,NULL);VkQueueFamilyProperties *families=calloc(count,sizeof *families);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys,&count,families);
    for(g_vk.qfam=0;g_vk.qfam<count;g_vk.qfam++)if((families[g_vk.qfam].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))break;
    assert(g_vk.qfam<count);free(families);float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=g_vk.qfam,.queueCount=1,.pQueuePriorities=&priority};
    VkPhysicalDeviceVulkan13Features features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.synchronization2=VK_TRUE};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&features,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VK_CHECK(vkCreateDevice(g_vk.phys,&di,NULL,&g_vk.dev));vkGetDeviceQueue(g_vk.dev,g_vk.qfam,0,&g_vk.queue);
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=g_vk.qfam};VK_CHECK(vkCreateCommandPool(g_vk.dev,&pi,NULL,&g_vk.pool));
    VkDescriptorPoolSize poolsize={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,128};
    VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=64,.poolSizeCount=1,.pPoolSizes=&poolsize};VK_CHECK(vkCreateDescriptorPool(g_vk.dev,&dpi,NULL,&descriptor_pool));
    g_vk.props.limits.maxComputeWorkGroupCount[0]=1; // force the dispatch's second dimension on tiny images
    run_case(VK_FORMAT_D32_SFLOAT_S8_UINT,VK_IMAGE_ASPECT_DEPTH_BIT,7,5,5,3,false);
    run_case(VK_FORMAT_D32_SFLOAT_S8_UINT,VK_IMAGE_ASPECT_STENCIL_BIT,7,5,5,3,false);
    run_case(VK_FORMAT_D32_SFLOAT_S8_UINT,VK_IMAGE_ASPECT_DEPTH_BIT,3,2,17,11,true);
    run_case(VK_FORMAT_D32_SFLOAT_S8_UINT,VK_IMAGE_ASPECT_STENCIL_BIT,3,2,17,11,true);
    run_case(VK_FORMAT_D16_UNORM,VK_IMAGE_ASPECT_DEPTH_BIT,7,5,1,1,false);
    run_case(VK_FORMAT_D16_UNORM,VK_IMAGE_ASPECT_DEPTH_BIT,3,2,17,11,false);
    run_case(VK_FORMAT_D24_UNORM_S8_UINT,VK_IMAGE_ASPECT_DEPTH_BIT,7,5,5,3,false);
    run_case(VK_FORMAT_D24_UNORM_S8_UINT,VK_IMAGE_ASPECT_STENCIL_BIT,3,2,17,11,false);
    for(unsigned i=0;i<buffer_count;i++){vkUnmapMemory(g_vk.dev,buffers[i].memory);vkDestroyBuffer(g_vk.dev,buffers[i].buffer,NULL);vkFreeMemory(g_vk.dev,buffers[i].memory,NULL);}
    vkDestroyPipeline(g_vk.dev,depth_copy_pipeline,NULL);vkDestroyPipelineLayout(g_vk.dev,depth_copy_layout,NULL);vkDestroyDescriptorSetLayout(g_vk.dev,depth_copy_set,NULL);
    vkDestroyDescriptorPool(g_vk.dev,descriptor_pool,NULL);vkDestroyCommandPool(g_vk.dev,g_vk.pool,NULL);vkDestroyDevice(g_vk.dev,NULL);
    if(validate)((PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst,"vkDestroyDebugUtilsMessengerEXT"))(g_vk.inst,messenger,NULL);
    vkDestroyInstance(g_vk.inst,NULL);assert(!errors);
    printf("PASS: GPU depth/stencil nearest copies, bit patterns, rectangles, mips, layers, source preservation and canaries (validation=%d)\n",validate);
}
