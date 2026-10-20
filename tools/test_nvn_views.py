"""Check both guest view ABIs and snapshots consumed by descriptor pools/commands."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
objects = (ROOT / 'runtime/nvn/objects.c').read_text(encoding='utf8')
views = objects[objects.index('// Legacy views retain'):objects.index('NVN(nvnTextureWriteTexels)')]
registration = objects[objects.index('static void register_texture('):objects.index('NVN(nvnSamplerPoolInitialize)')]
source = r'''
#include <assert.h>
#include <stdarg.h>
#include "hle.h"
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#include "nvn/cmdbuf.c"
static TitleProfile profile;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static NvnDescPool *g_texture_pools[64];
static unsigned format_notes;
void backend_texture_view_format(NvnTexture *t,uint32_t format) {
    assert(t && format==0x12345678);format_notes++;
}
const TitleProfile *title_profile_current(void) { return &profile; }
void nvn_sync_arm(uint64_t guest) { (void)guest; }
void guest_call(Cpu *c,uint64_t address) { (void)c;(void)address;abort(); }
void rt_fatal_msg(const char *fmt,...) { va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap); }
void rt_fatal_trace(void) { abort(); }
''' + views + registration + r'''
static void guards(const uint8_t *storage,unsigned size) {
    for(unsigned i=0;i<8;i++) assert(storage[i]==0xcc);
    for(unsigned i=8+size;i<64;i++) assert(storage[i]==0xcc);
}
static CmdRec *find(uint64_t handle,unsigned op) {
    CmdRec *r=(CmdRec *)(uintptr_t)((CmdRec *)(uintptr_t)handle)->x[0];
    while(r->op!=op) { assert(r->op!=CMD_END);r=(CmdRec *)((uint8_t *)r+r->size); }
    return r;
}
int main(void) {
    for(unsigned major=53;major<=55;major+=2) {
        profile.nvn_major=major;
        unsigned size=major==55 ? 16 : 40;
        __attribute__((aligned(8))) uint8_t storage[64],other[64];
        memset(storage,0xcc,sizeof storage);uint8_t *view=storage+8;
        Cpu c={0};c.x[0]=(uintptr_t)view;hle_nvnTextureViewSetDefaults(&c);guards(storage,size);
        uint32_t out[4];c.x[1]=(uintptr_t)out;c.x[2]=(uintptr_t)(out+1);
        hle_nvnTextureViewGetLevels(&c);assert(!c.x[0] && out[0]==0 && out[1]==0);
        c.x[0]=(uintptr_t)view;c.x[1]=3;c.x[2]=4;hle_nvnTextureViewSetLevels(&c);
        c.x[1]=0x123;c.x[2]=0x456;hle_nvnTextureViewSetLayers(&c);
        c.x[1]=0x12345678;hle_nvnTextureViewSetFormat(&c);
        c.x[1]=2;c.x[2]=3;c.x[3]=4;c.x[4]=5;hle_nvnTextureViewSetSwizzle(&c);
        c.x[1]=1;hle_nvnTextureViewSetDepthStencilMode(&c);
        c.x[1]=9;hle_nvnTextureViewSetTarget(&c);guards(storage,size);
        if(major==55) {
            const uint64_t expected[2]={UINT64_C(0x7f09040304560123),UINT64_C(0x0504030212345678)};
            assert(!memcmp(view,expected,16));
        }
        NvnTextureView expanded=nvn_texture_view_read(view);
        assert(expanded.setmask==63 && expanded.base_level==3 && expanded.num_levels==4);
        assert(expanded.min_layer==0x123 && expanded.num_layers==0x456 && expanded.target==9 && expanded.ds_mode==1);
        c.x[1]=(uintptr_t)out;c.x[2]=(uintptr_t)(out+1);hle_nvnTextureViewGetLevels(&c);
        assert(c.x[0]==1 && out[0]==3 && out[1]==4);c.x[0]=(uintptr_t)view;
        hle_nvnTextureViewGetLayers(&c);assert(c.x[0]==1 && out[0]==0x123 && out[1]==0x456);c.x[0]=(uintptr_t)view;
        hle_nvnTextureViewGetFormat(&c);assert(c.x[0]==1 && out[0]==0x12345678);c.x[0]=(uintptr_t)view;
        for(unsigned i=0;i<4;i++) c.x[1+i]=(uintptr_t)(out+i);
        hle_nvnTextureViewGetSwizzle(&c);assert(c.x[0]==1 && out[0]==2 && out[3]==5);c.x[0]=(uintptr_t)view;
        hle_nvnTextureViewGetTarget(&c);assert(c.x[0]==1 && out[0]==9);c.x[0]=(uintptr_t)view;
        hle_nvnTextureViewGetDepthStencilMode(&c);assert(c.x[0]==1 && out[0]==1);
        memcpy(other,storage,sizeof other);c.x[0]=(uintptr_t)view;c.x[1]=(uintptr_t)(other+8);
        hle_nvnTextureViewCompare(&c);assert(c.x[0]==1);
        NvnTexture texture={0};uint64_t tex=(uintptr_t)&texture;
        NvnTexEntry entry={0};NvnDescPool descriptors={.count=1,.entries=&entry};uint64_t descriptor_handle=(uintptr_t)&descriptors;
        c.x[0]=(uintptr_t)&descriptor_handle;c.x[1]=0;c.x[2]=(uintptr_t)&tex;c.x[3]=(uintptr_t)view;
        hle_nvnTexturePoolRegisterTexture(&c);assert(entry.has_view && !memcmp(&entry.view,&expanded,sizeof expanded));
        // Record all view-capturing paths, then mutate the guest buffer.
        uint8_t memory[8192],control[256];NvnPool pool={.storage=(uintptr_t)memory,.size=sizeof memory};
        uint64_t ph=(uintptr_t)&pool,cmd=0;
        c.x[0]=(uintptr_t)&cmd;hle_nvnCommandBufferInitialize(&c);
        c.x[0]=(uintptr_t)&cmd;c.x[1]=(uintptr_t)control;c.x[2]=sizeof control;hle_nvnCommandBufferAddControlMemory(&c);
        c.x[1]=(uintptr_t)&ph;c.x[2]=0;c.x[3]=sizeof memory;hle_nvnCommandBufferAddCommandMemory(&c);hle_nvnCommandBufferBeginRecording(&c);
        uint64_t colors[]={(uintptr_t)&tex},view_array[]={(uintptr_t)view};
        c.x[1]=1;c.x[2]=(uintptr_t)colors;c.x[3]=(uintptr_t)view_array;c.x[4]=(uintptr_t)&tex;c.x[5]=(uintptr_t)view;
        hle_nvnCommandBufferSetRenderTargets(&c);
        int32_t region[6]={0,0,0,1,1,1};float color[4]={0};
        c.x[1]=(uintptr_t)&tex;c.x[2]=(uintptr_t)view;c.x[3]=(uintptr_t)region;c.x[4]=(uintptr_t)color;
        hle_nvnCommandBufferClearTexture(&c);
        c.x[1]=(uintptr_t)&tex;c.x[2]=(uintptr_t)view;c.x[3]=(uintptr_t)region;
        c.x[4]=(uintptr_t)&tex;c.x[5]=(uintptr_t)view;c.x[6]=(uintptr_t)region;c.x[7]=0;
        hle_nvnCommandBufferCopyTextureToTexture(&c);
        hle_nvnCommandBufferEndRecording(&c);uint64_t handle=c.x[0];
        memset(view,0,size);
        const RtRec *rt=(const RtRec *)find(handle,CMD_SET_RENDER_TARGETS)->extra;
        assert(rt->has_view[0] && rt->has_dview && !memcmp(&rt->views[0],&expanded,sizeof expanded));
        assert(!memcmp(&rt->dview,&expanded,sizeof expanded));
        const ClearTexRec *clear=(const ClearTexRec *)find(handle,CMD_CLEAR_TEXTURE)->extra;
        assert(clear->has_view && !memcmp(&clear->view,&expanded,sizeof expanded));
        const TexRef *copy=(const TexRef *)find(handle,CMD_COPY_TEXTURE_TO_TEXTURE)->extra;
        assert(copy[0].has_view && copy[1].has_view && !memcmp(&copy[0].view,&expanded,sizeof expanded));
        assert(!memcmp(&copy[1].view,&expanded,sizeof expanded) && !memcmp(&entry.view,&expanded,sizeof expanded));
        assert(format_notes==6*(major==53 ? 1u : 2u));
        guards(storage,size);
        nvn_command_handle_destroy(handle);c.x[0]=(uintptr_t)&cmd;hle_nvnCommandBufferFinalize(&c);
        c.x[0]=(uintptr_t)view;c.x[1]=(uintptr_t)(other+8);hle_nvnTextureViewCompare(&c);assert(!c.x[0]);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary); path = root / 'views.c'; exe = root / ('views.exe' if os.name == 'nt' else 'views')
    path.write_text(source, encoding='utf8')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
        '-std=gnu11', '-O2', '-fno-strict-aliasing', '-pthread', '-I' + str(ROOT / 'runtime'), str(path), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=20)
print('PASS: NVN 53/55 view boundaries, packed fields, getters, descriptor conversion and render/clear/copy snapshots')
