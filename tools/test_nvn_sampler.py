"""Run production sampler APIs in guarded NVN 53/55 guest builders."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
objects = (ROOT / 'runtime/nvn/objects.c').read_text(encoding='utf8')
samplers = objects[objects.index('// ================================================================== samplers'):
                   objects.index('// ================================================================== descriptor pools')]
register = objects[objects.index('NVN(nvnSamplerPoolRegisterSampler)'):
                   objects.index('NvnDescPool *nvn_find_pool')]
source = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#include "nvn/nvn.h"
static TitleProfile profile;
const TitleProfile *title_profile_current(void) { return &profile; }
''' + samplers + register + r'''
typedef struct { uint64_t before[2]; unsigned char bytes[80]; uint64_t after[2]; } Guarded;
static void guards(const Guarded *g) {
    assert(g->before[0]==0x1122 && g->before[1]==0x3344 && g->after[0]==0x5566 && g->after[1]==0x7788);
    if (profile.nvn_major==55) for(unsigned i=64;i<80;i++) assert(g->bytes[i]==0xcd);
}
int main(void) {
  for(unsigned major=53;major<=55;major+=2) {
    profile.nvn_major=major;
    Guarded g={{0x1122,0x3344},{0},{0x5566,0x7788}};
    memset(g.bytes,0xcd,sizeof g.bytes);
    Cpu c={0};c.x[0]=(uintptr_t)g.bytes;c.x[1]=0x123456789;
    hle_nvnSamplerBuilderSetDevice(&c);hle_nvnSamplerBuilderSetDefaults(&c);guards(&g);
    hle_nvnSamplerBuilderGetDevice(&c);assert(c.x[0]==0x123456789);c.x[0]=(uintptr_t)g.bytes;
    c.x[1]=5;c.x[2]=1;hle_nvnSamplerBuilderSetMinMagFilter(&c);
    c.x[1]=2;c.x[2]=3;c.x[3]=4;hle_nvnSamplerBuilderSetWrapMode(&c);
    (*(float *)&c.v[0])=2.5f;(*(float *)&c.v[1])=13.5f;hle_nvnSamplerBuilderSetLodClamp(&c);
    (*(float *)&c.v[0])=-0.75f;hle_nvnSamplerBuilderSetLodBias(&c);
    (*(float *)&c.v[0])=16.0f;hle_nvnSamplerBuilderSetMaxAnisotropy(&c);
    (*(float *)&c.v[0])=0.5f;hle_nvnSamplerBuilderSetLodSnap(&c);
    c.x[1]=1;c.x[2]=6;hle_nvnSamplerBuilderSetCompare(&c);
    c.x[1]=2;hle_nvnSamplerBuilderSetReductionFilter(&c);
    uint32_t border[4]={0x80000000,0xffffffff,0x7fc00001,0x12345678};
    c.x[1]=(uintptr_t)border;hle_nvnSamplerBuilderSetBorderColorui(&c);guards(&g);
    uint32_t out[4]={0};float floats[2]={0};
    c.x[1]=(uintptr_t)&out[0];c.x[2]=(uintptr_t)&out[1];c.x[3]=(uintptr_t)&out[2];
    hle_nvnSamplerBuilderGetMinMagFilter(&c);assert(out[0]==5 && out[1]==1);
    hle_nvnSamplerBuilderGetWrapMode(&c);assert(out[0]==2 && out[1]==3 && out[2]==4);
    hle_nvnSamplerBuilderGetCompare(&c);assert(out[0]==1 && out[1]==6);
    c.x[1]=(uintptr_t)&floats[0];c.x[2]=(uintptr_t)&floats[1];
    hle_nvnSamplerBuilderGetLodClamp(&c);assert(floats[0]==2.5f && floats[1]==13.5f);
    hle_nvnSamplerBuilderGetLodBias(&c);assert((*(float *)&c.v[0])==-0.75f);
    hle_nvnSamplerBuilderGetMaxAnisotropy(&c);assert((*(float *)&c.v[0])==16.0f);
    hle_nvnSamplerBuilderGetLodSnap(&c);assert((*(float *)&c.v[0])==0.0f); // existing unsupported behavior
    hle_nvnSamplerBuilderGetReductionFilter(&c);assert(c.x[0]==2);c.x[0]=(uintptr_t)g.bytes;
    c.x[1]=(uintptr_t)out;hle_nvnSamplerBuilderGetBorderColorui(&c);assert(!memcmp(out,border,sizeof out));
    uint64_t sampler=0;c.x[0]=(uintptr_t)&sampler;c.x[1]=(uintptr_t)g.bytes;
    hle_nvnSamplerInitialize(&c);assert(c.x[0]==1);guards(&g);
    NvnSamplerDesc *desc=(NvnSamplerDesc *)(uintptr_t)sampler;
    assert(!desc->backend && desc->min_filter==5 && desc->wrap[2]==4 && desc->compare_func==6);
    assert(desc->lod_min==2.5f && desc->lod_max==13.5f && desc->lod_bias==-0.75f && desc->max_aniso==16.0f);
    assert(!memcmp(desc->border,border,sizeof border));
    NvnSamplerDesc entries[2]={0};NvnDescPool pool={.count=2,.entries=entries};uint64_t handle=(uintptr_t)&pool;
    c.x[0]=(uintptr_t)&handle;c.x[1]=0;c.x[2]=(uintptr_t)&sampler;hle_nvnSamplerPoolRegisterSampler(&c);
    c.x[1]=1;c.x[2]=(uintptr_t)g.bytes;hle_nvnSamplerPoolRegisterSamplerBuilder(&c);
    assert(!memcmp(&entries[0],&entries[1],sizeof entries[0]));guards(&g);
    c.x[0]=(uintptr_t)&sampler;hle_nvnSamplerFinalize(&c);assert(!sampler);
  }
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary); path = root / 'sampler.c'; exe = root / ('sampler.exe' if os.name == 'nt' else 'sampler')
    path.write_text(source, encoding='utf8')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + [
        '-std=gnu11', '-O2', '-fno-strict-aliasing', '-I' + str(ROOT / 'runtime'), str(path), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=20)
print('PASS: sampler builder storage guards, property roundtrips, host-only backend pointer and pool conversion')
