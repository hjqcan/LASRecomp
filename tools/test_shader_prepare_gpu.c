// Real Vulkan, synthetic shaders only: concurrent reuse, failure wakeup and a
// single first-use compute dispatch that must initialize its destination.
#include "../runtime/nvn/vulkan/shaders.h"
#include "../runtime/nvn/vulkan/shader_jobs.h"
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

VkG g_vk;
const char smaa_hlsl_src[] = ""; // this fixture never requests built-in SMAA shaders
void rt_fatal_msg(const char *fmt, ...) { va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); }
void rt_fatal_trace(void) { abort(); }
void vk_device_fault_report(void) { abort(); }
static const char *source = "#version 450\nlayout(local_size_x=1) in;\n"
    "layout(set=0,binding=0,std430) buffer Result { uint value; };\n"
    "void main() { value=0x12345678u; }\n";
static const VkPreparedShader *prepared[16];
static void *prepare(void *index) {
    prepared[(uintptr_t)index] = vk_prepare_shader(source, STAGE_COMPUTE);
    return NULL;
}
typedef struct { ShaderJob job; VkPipeline pipeline; VkPipelineLayout layout; VkDescriptorSetLayout set; } Init;
static void build(void *ptr) {
    Init *init = ptr;
    const VkPreparedShader *shader = vk_prepare_shader(source, STAGE_COMPUTE);
    assert(shader->module && shader->nbinds == 1);
    init->set = vk_set_layout(shader->binds, shader->nbinds, 0, VK_SHADER_STAGE_COMPUTE_BIT);
    VkPipelineLayoutCreateInfo layout = {.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount=1,.pSetLayouts=&init->set};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev,&layout,NULL,&init->layout));
    VkComputePipelineCreateInfo pi = {.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=init->layout,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
                .module=shader->module,.pName="main"}};
    VK_CHECK(vkCreateComputePipelines(g_vk.dev,VK_NULL_HANDLE,1,&pi,NULL,&init->pipeline));
}
int main(void) {
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VK_CHECK(vkCreateInstance(&ii,NULL,&g_vk.inst));
    uint32_t count = 0; VK_CHECK(vkEnumeratePhysicalDevices(g_vk.inst,&count,NULL)); assert(count);
    VkPhysicalDevice *devices = calloc(count,sizeof *devices);
    VK_CHECK(vkEnumeratePhysicalDevices(g_vk.inst,&count,devices)); g_vk.phys=devices[0]; free(devices);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys,&count,NULL);
    VkQueueFamilyProperties *families = calloc(count,sizeof *families);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys,&count,families);
    for (g_vk.qfam=0;g_vk.qfam<count;g_vk.qfam++) if (families[g_vk.qfam].queueFlags&VK_QUEUE_COMPUTE_BIT) break;
    assert(g_vk.qfam<count); free(families);
    float priority=1;
    VkDeviceQueueCreateInfo qi = {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=g_vk.qfam,
        .queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo di = {.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi};
    VK_CHECK(vkCreateDevice(g_vk.phys,&di,NULL,&g_vk.dev)); vkGetDeviceQueue(g_vk.dev,g_vk.qfam,0,&g_vk.queue);
    vk_shaders_init(); shader_jobs_init(2);
    pthread_t threads[16];
    for (uintptr_t i=0;i<16;i++) pthread_create(&threads[i],NULL,prepare,(void *)i);
    for (int i=0;i<16;i++) pthread_join(threads[i],NULL);
    for (int i=0;i<16;i++) assert(prepared[i]==prepared[0] && prepared[i]->module);
    Init init={0}; shader_job_submit(&init.job,build,&init);
    shader_job_wait(&init.job); assert(init.pipeline);

    VkBuffer buffer; VkDeviceMemory memory;
    VkBufferCreateInfo bi = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=4,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VK_CHECK(vkCreateBuffer(g_vk.dev,&bi,NULL,&buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(g_vk.dev,buffer,&req);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(g_vk.phys,&mp);
    uint32_t type;
    for(type=0;type<mp.memoryTypeCount;type++) if ((req.memoryTypeBits&(1u<<type)) &&
        (mp.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) break;
    assert(type<mp.memoryTypeCount);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,.memoryTypeIndex=type};
    VK_CHECK(vkAllocateMemory(g_vk.dev,&ai,NULL,&memory)); VK_CHECK(vkBindBufferMemory(g_vk.dev,buffer,memory,0));
    uint32_t *value; VK_CHECK(vkMapMemory(g_vk.dev,memory,0,4,0,(void **)&value)); *value=0;
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
    VkDescriptorPoolCreateInfo dpi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
    VkDescriptorPool dp; VK_CHECK(vkCreateDescriptorPool(g_vk.dev,&dpi,NULL,&dp));
    VkDescriptorSetAllocateInfo dsi={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,.descriptorSetCount=1,.pSetLayouts=&init.set};
    VkDescriptorSet ds; VK_CHECK(vkAllocateDescriptorSets(g_vk.dev,&dsi,&ds));
    VkDescriptorBufferInfo dbi={buffer,0,4};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=ds,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,.pBufferInfo=&dbi};
    vkUpdateDescriptorSets(g_vk.dev,1,&write,0,NULL);
    VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev,&pci,NULL,&g_vk.pool));
    VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=g_vk.pool,
        .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cb; VK_CHECK(vkAllocateCommandBuffers(g_vk.dev,&cai,&cb));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; VK_CHECK(vkBeginCommandBuffer(cb,&begin));
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,init.pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,init.layout,0,1,&ds,0,NULL);
    vkCmdDispatch(cb,1,1,1);
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,NULL,0,NULL);
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cb};
    VK_CHECK(vkQueueSubmit(g_vk.queue,1,&submit,VK_NULL_HANDLE)); VK_CHECK(vkQueueWaitIdle(g_vk.queue));
    assert(*value==0x12345678u);
    shader_jobs_drain(); vk_shader_cache_reset();
    // A reset must permit rebuilding the same source, not retain destroyed modules.
    assert(vk_prepare_shader(source,STAGE_COMPUTE)->module); vk_shader_cache_reset();
    source="#version 450\ninvalid shader;\n";
    for (uintptr_t i=0;i<16;i++) pthread_create(&threads[i],NULL,prepare,(void *)i);
    for (int i=0;i<16;i++) pthread_join(threads[i],NULL);
    for (int i=0;i<16;i++) assert(prepared[i]==prepared[0] && !prepared[i]->module);
    vk_shader_cache_reset();
    vkDestroyPipeline(g_vk.dev,init.pipeline,NULL); vkDestroyPipelineLayout(g_vk.dev,init.layout,NULL);
    vkDestroyDescriptorSetLayout(g_vk.dev,init.set,NULL); vkDestroyDescriptorPool(g_vk.dev,dp,NULL);
    vkDestroyCommandPool(g_vk.dev,g_vk.pool,NULL); vkUnmapMemory(g_vk.dev,memory);
    vkDestroyBuffer(g_vk.dev,buffer,NULL); vkFreeMemory(g_vk.dev,memory,NULL);
    vkDestroyDevice(g_vk.dev,NULL); vkDestroyInstance(g_vk.inst,NULL);
    puts("PASS: concurrent stage reuse, one-shot GPU initialization, cache reset and failed compilation wakeup");
}
