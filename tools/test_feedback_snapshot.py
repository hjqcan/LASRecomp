"""Exercise the real Vulkan feedback copy and sampled-view code with a mock GPU.

The regression is a six-level HDR image read at LOD 2 while another mip is an
attachment. A level-zero-only copy silently samples the sharp, bright base mip.
No Vulkan SDK or device is needed; API mocks record allocation, copy and views.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
execution = (ROOT/'runtime/nvn/vulkan/exec.c').read_text()
textures = (ROOT/'runtime/nvn/vulkan/textures.c').read_text()

def function(source, signature):
    start = source.index(signature)
    first = source.index('{', start)
    depth = 1
    end = first + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t VkImage, VkImageView, VmaAllocation, VkCommandBuffer;
typedef unsigned VkFormat, VkImageViewType, VkImageAspectFlags, VkSampleCountFlagBits, VkComponentSwizzle;
enum { VK_NULL_HANDLE=0, VK_SUCCESS=0, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO=1,
 VK_IMAGE_TYPE_1D=1, VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_3D,
 VK_IMAGE_VIEW_TYPE_1D=10, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D,
 VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
 VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT=1, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT=2,
 VK_IMAGE_TILING_OPTIMAL=0, VK_IMAGE_USAGE_SAMPLED_BIT=1, VK_IMAGE_USAGE_TRANSFER_DST_BIT=2,
 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE=1, VK_IMAGE_LAYOUT_GENERAL=1,
 VK_IMAGE_ASPECT_COLOR_BIT=1, VK_IMAGE_ASPECT_DEPTH_BIT=2, VK_IMAGE_ASPECT_STENCIL_BIT=4,
 VK_COMPONENT_SWIZZLE_IDENTITY=0, VK_COMPONENT_SWIZZLE_ZERO=1, VK_COMPONENT_SWIZZLE_ONE=2 };
typedef struct { uint32_t width,height,depth; } Extent;
typedef struct { uint32_t aspectMask,mipLevel,baseArrayLayer,layerCount; } Subresource;
typedef struct { Subresource srcSubresource; int srcOffset[3]; Subresource dstSubresource; int dstOffset[3]; Extent extent; } VkImageCopy;
typedef struct { int sType,flags,imageType; VkFormat format; Extent extent; uint32_t mipLevels,arrayLayers;
                 VkSampleCountFlagBits samples; int tiling,usage; } VkImageCreateInfo;
typedef struct { int usage; } VmaAllocationCreateInfo;
typedef struct { VkComponentSwizzle r,g,b,a; } VkComponentMapping;
static const VkComponentMapping IDENTITY={0};
typedef struct TexBackend { VkImage image; VmaAllocation mem; VkFormat format; VkImageViewType base_type;
 uint32_t width,height,depth,layers,levels,samples; VkImageAspectFlags aspect; bool gpu_owned; } TexBackend;
typedef struct { uint32_t setmask; uint16_t base_level,num_levels,min_layer,num_layers;
 uint32_t format,target,ds_mode; uint8_t swizzle[4]; } NvnTextureView;
typedef struct { uint32_t format,target,width,height,depth,layers; uint64_t addr; uint8_t swizzle[4]; TexBackend *backend; } NvnTexture;
typedef struct { NvnTexture *object; } ShaderTexture;
typedef struct { int ntex; ShaderTexture tex[4]; } ShaderInfo;
typedef struct { ShaderInfo *vi,*fi; } Prog;
static struct { struct { NvnTexture *depth,*color[8]; int ncolor; } rt; bool rt_dirty; uint8_t ds[2]; } st;
static struct { int vma; } g_vk;
static bool g_debug_frame;
static unsigned stat_feedback_copies;
static uint32_t level_extent(uint32_t x,uint32_t lv) { x>>=lv; return x?x:1; }
static NvnTexture *shader_texture_object(int stage,ShaderTexture *s) { (void)stage; return s->object; }
static TexBackend *vk_tex(NvnTexture *t,bool render) { (void)render; return t->backend; }
static unsigned creates,copies,forgot,deferred,view_count;
static VkImageCreateInfo allocations[128];
static double mip_data[128][16];
static VkImageCopy copied[256];
static struct { VkImage image; VkImageViewType type; VkFormat format; uint32_t base,levels,layer,layers; VkComponentMapping sw; } views[128];
static int vmaCreateImage(int vma,const VkImageCreateInfo *ci,const VmaAllocationCreateInfo *ai,VkImage *image,VmaAllocation *mem,void *info) {
 (void)vma;(void)ai;(void)info; *image=++creates+10;*mem=*image;allocations[*image]=*ci;return 0; }
static void vkCmdCopyImage(VkCommandBuffer cb,VkImage src,int sl,VkImage dst,int dl,unsigned n,const VkImageCopy *r) {
 (void)cb;(void)sl;(void)dl;
 for(unsigned i=0;i<n;i++) { copied[copies++]=r[i]; mip_data[dst][r[i].dstSubresource.mipLevel]=mip_data[src][r[i].srcSubresource.mipLevel]; }
}
static VkImageView vk_image_view(TexBackend *b,VkImageViewType type,VkFormat format,uint32_t base,uint32_t levels,uint32_t layer,
                                uint32_t layers,VkComponentMapping sw,VkImageAspectFlags aspect) {
 (void)aspect;unsigned v=++view_count;views[v].image=b->image;views[v].type=type;views[v].format=format;
 views[v].base=base;views[v].levels=levels;views[v].layer=layer;views[v].layers=layers;views[v].sw=sw;return v;
}
static double sample_lod(VkImageView view,unsigned lod) { unsigned n=views[view].levels; if(lod>=n)lod=n-1;return mip_data[views[view].image][views[view].base+lod]; }
static void vk_views_forget(TexBackend *d) {(void)d;forgot++;}
static void vk_defer_image(VkImage i,VmaAllocation m) {(void)i;(void)m;deferred++;}
static VkCommandBuffer vk_cmd(void) {return 1;}
static void vk_image_init_layout(VkCommandBuffer cb,VkImage i,VkImageAspectFlags a) {(void)cb;(void)i;(void)a;}
static void vk_full_barrier(VkCommandBuffer cb) {(void)cb;}
static void vk_end_pass(void) {}
static bool ct_on(void) {return false;}
static void debug_save_depth(TexBackend *b,const char *p) {(void)b;(void)p;}
static VkImageViewType view_type_for(int type) {return (VkImageViewType)type;}
static VkImageView vk_dummy_view(int type) {(void)type;assert(!"unexpected dummy view");return 0;}
typedef struct { VkFormat vf; bool alpha_one; } VkFmt;
static VkFmt fmt_of(uint32_t f) {return (VkFmt){f,false};}
static VkFormat vk_format(uint32_t f) {return f;}
#define VK_CHECK(x) assert((x)==0)
#define LOG(...) ((void)0)
#define gp_mark(...) ((void)0)
'''
start = execution.index('static NvnTexture *feedback_tex[4];')
end = execution.index('static NvnTexture *shader_texture_object(', start)
harness += execution[start:end]
for sig in ('static bool is_attachment(', 'static bool snapshot_matches(', 'static TexBackend *snapshot_image(', 'static void resolve_feedback('):
    harness += function(execution, sig)
harness += function(textures, 'VkImageView vk_view_backend(')
harness += r'''
int main(void) {
 TexBackend src={.image=1,.format=63,.base_type=VK_IMAGE_VIEW_TYPE_2D,.width=512,.height=256,.depth=1,.layers=1,.levels=6,.samples=1,.aspect=1};
 NvnTexture texture={.format=63,.width=512,.height=256,.depth=1,.backend=&src,.swizzle={2,3,4,1}};
 ShaderInfo vs={0},fs={.ntex=1,.tex={{&texture}}};Prog program={&vs,&fs};
 st.rt.color[0]=&texture;st.rt.ncolor=1;
 for(unsigned i=0;i<6;i++)mip_data[1][i]=1024.0/(1u<<(i*2));
 resolve_feedback(&program);
 assert(nfeedback==1 && creates==1 && copies==6);
 TexBackend *snapshot=feedback_copy[0]; assert(snapshot->levels==6 && snapshot->layers==1);
 assert(allocations[snapshot->image].mipLevels==6);
 for(unsigned i=0;i<6;i++) {assert(copied[i].srcSubresource.mipLevel==i && copied[i].dstSubresource.mipLevel==i);
   assert(copied[i].extent.width==(512u>>i) && copied[i].extent.height==(256u>>i));}
 VkImageView view=vk_view_backend(&texture,snapshot,NULL,false,false,VK_IMAGE_VIEW_TYPE_2D);
 assert(sample_lod(view,2)==64.0); // prior code returned the 1024-valued base mip
 assert(sample_lod(view,5)==1.0 && views[view].sw.a==VK_COMPONENT_SWIZZLE_ONE);
 resolve_feedback(&program); assert(copies==6); // a valid snapshot is reusable
 snapshots_invalidate(&texture);mip_data[1][2]=17.0;resolve_feedback(&program);assert(copies==12);
 assert(sample_lod(view,2)==17.0);
 // Each descriptor retains its own mip range, reinterpretation and swizzle.
 NvnTextureView sub={.setmask=1|4|8,.base_level=2,.num_levels=2,.format=62,.swizzle={4,3,2,0}};
 view=vk_view_backend(&texture,snapshot,&sub,true,false,VK_IMAGE_VIEW_TYPE_2D);
 assert(views[view].base==2 && views[view].levels==2 && views[view].format==62);
 assert(views[view].sw.r==5 && views[view].sw.b==3 && views[view].sw.a==VK_COMPONENT_SWIZZLE_ZERO);
 assert(sample_lod(view,0)==17.0 && sample_lod(view,20)==16.0);
 // Same width/height but a different mip count invalidates the cached image.
 src.levels=4;resolve_feedback(&program);assert(creates==2 && copies==16 && forgot==1 && deferred==1);
 // Cube/array snapshots keep all layers and remain sampleable through a layer view.
 src.base_type=VK_IMAGE_VIEW_TYPE_CUBE;src.layers=6;src.width=src.height=64;src.levels=4;
 resolve_feedback(&program);snapshot=feedback_copy[0];assert(creates==3 && copies==20);
 assert(snapshot->layers==6 && (allocations[snapshot->image].flags&VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT));
 for(unsigned i=16;i<20;i++)assert(copied[i].srcSubresource.layerCount==6 && copied[i].dstSubresource.layerCount==6);
 sub=(NvnTextureView){.setmask=1|2,.base_level=1,.num_levels=2,.min_layer=3,.num_layers=1};
 view=vk_view_backend(&texture,snapshot,&sub,true,false,VK_IMAGE_VIEW_TYPE_2D);
 assert(views[view].base==1 && views[view].layer==3 && views[view].layers==1);
 // Volume copies reduce depth with the mip, rather than dropping all but one slice.
 src.base_type=VK_IMAGE_VIEW_TYPE_3D;src.layers=1;src.depth=8;resolve_feedback(&program);assert(creates==4 && copies==24);
 for(unsigned i=0;i<4;i++)assert(copied[20+i].extent.depth==(8u>>i));
 puts("PASS: feedback mip chain, LOD clamps, refresh, per-binding views, cube layers and volume depth");
}
'''
with tempfile.TemporaryDirectory(prefix='feedback-snapshot-') as temp:
    temp = Path(temp)
    source = temp/'test.c'; source.write_text(harness)
    exe = temp/('test.exe' if os.name == 'nt' else 'test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang'))+['-std=gnu11',str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)
