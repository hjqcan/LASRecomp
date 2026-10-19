"""Exercise production cache snapshots against concurrent pipeline creators."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'runtime/nvn/vulkan/device.c').read_text()
def function(signature):
    start = source.index(signature); at = source.index('{', start); depth = 1; end = at+1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[start:end]+'\n'

harness = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef int VkResult,VkPipeline,VkGraphicsPipelineCreateInfo,VkComputePipelineCreateInfo;
#define VK_SUCCESS 0
#define LOG(...) ((void)0)
#define PCACHE_FILE "cache"
#define PCACHE_TAG "tag"
static struct { int dev,pcache; } g_vk;
static size_t pcache_saved;
static pthread_rwlock_t pcache_access=PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t schedule=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake=PTHREAD_COND_INITIALIZER;
static unsigned active,maximum,queries,writes,attempts;
static bool release_creators,copy_entered,release_copy;
static int compile_pipeline(void) {
    pthread_mutex_lock(&schedule); ++active; if(active>maximum)maximum=active;
    assert(!copy_entered || release_copy); pthread_cond_broadcast(&wake);
    while(!release_creators)pthread_cond_wait(&wake,&schedule);
    --active; pthread_mutex_unlock(&schedule); return 7;
}
static int vkCreateGraphicsPipelines(int d,int c,unsigned n,const int *i,void *a,int *o) {
    (void)d;(void)c;(void)n;(void)i;(void)a;(void)o;return compile_pipeline();
}
static int vkCreateComputePipelines(int d,int c,unsigned n,const int *i,void *a,int *o) {
    (void)d;(void)c;(void)n;(void)i;(void)a;(void)o;return compile_pipeline();
}
static int vkGetPipelineCacheData(int d,int c,size_t *size,void *data) {
    (void)d;(void)c;pthread_mutex_lock(&schedule);assert(!active);++queries;
    if(data) {
        assert(*size==16);copy_entered=true;pthread_cond_broadcast(&wake);
        while(!release_copy)pthread_cond_wait(&wake,&schedule);
        memset(data,0x55,16);
    }
    *size=16;pthread_mutex_unlock(&schedule);return VK_SUCCESS;
}
static bool shader_cache_write(const char *name,const void *p,size_t size) {
    (void)name;assert(size==16 && ((const uint8_t *)p)[0]==0x55);++writes;return true;
}
static void pipeline_cache_tag(char *p,size_t size) {snprintf(p,size,"tag");}
static bool shader_cache_tag_ok(const char *name,const char *tag) {(void)name;(void)tag;return true;}
static void shader_cache_tag_write(const char *name,const char *tag) {(void)name;(void)tag;}
'''
for signature in ('VkResult vk_create_graphics_pipelines(', 'VkResult vk_create_compute_pipelines(', 'void vk_pipeline_cache_save(void) {'):
    harness += function(signature)
harness += r'''
static void *creator(void *kind) {
    pthread_mutex_lock(&schedule);++attempts;pthread_cond_broadcast(&wake);pthread_mutex_unlock(&schedule);
    int p,info=0;int result=kind ? vk_create_compute_pipelines(1,&info,&p) : vk_create_graphics_pipelines(1,&info,&p);
    assert(result==7);return NULL;
}
static void *saver(void *unused) {(void)unused;vk_pipeline_cache_save();return NULL;}
int main(void) {
    pthread_t a,b,s,c;assert(!pthread_create(&a,NULL,creator,NULL));assert(!pthread_create(&b,NULL,creator,(void *)1));
    pthread_mutex_lock(&schedule);while(active<2)pthread_cond_wait(&wake,&schedule);
    pthread_mutex_unlock(&schedule);assert(!pthread_create(&s,NULL,saver,NULL));
    pthread_mutex_lock(&schedule);assert(!queries);release_creators=true;pthread_cond_broadcast(&wake);
    while(!copy_entered)pthread_cond_wait(&wake,&schedule);
    pthread_mutex_unlock(&schedule);assert(!pthread_create(&c,NULL,creator,NULL));
    pthread_mutex_lock(&schedule);while(attempts<3)pthread_cond_wait(&wake,&schedule);
    assert(!active);release_copy=true;pthread_cond_broadcast(&wake);pthread_mutex_unlock(&schedule);
    pthread_join(a,NULL);pthread_join(b,NULL);pthread_join(c,NULL);pthread_join(s,NULL);
    assert(maximum==2 && queries==2 && writes==1 && pcache_saved==16);
    vk_pipeline_cache_save();assert(queries==3 && writes==1); // unchanged cache is not rewritten
    puts("PASS: parallel graphics/compute compilation, exclusive size/copy snapshot and unchanged-cache reuse");
}
'''
with tempfile.TemporaryDirectory(prefix='pipeline-cache-') as tmp:
    folder=Path(tmp); src=folder/'test.c';src.write_text(harness)
    exe=folder/('test.exe' if os.name=='nt' else 'test')
    subprocess.run(shlex.split(os.environ.get('CC','clang'))+['-std=c11','-O1','-pthread',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=20)
# Every production use of this shared cache must participate in the barrier.
for path in (root/'runtime/nvn/vulkan').glob('*.c'):
    if path.name != 'device.c':
        assert 'g_vk.pcache' not in path.read_text(), path
