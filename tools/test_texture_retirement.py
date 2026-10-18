"""Exercise production Vulkan retirement with a deterministic concurrent append.

The allocator holds queue growth open while another guest thread retires an
unrelated image. Guard storage makes the old out-of-capacity write observable
without relying on a heap crash or a Vulkan driver.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'runtime/nvn/vulkan/textures.c').read_text()

def function(signature):
    start = source.index(signature); at = source.index('{', start); depth = 1; end = at+1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[start:end]+'\n'

harness = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef uint64_t VkImage,VmaAllocation,VkImageView;
typedef struct TexBackend TexBackend;
static struct { int dev,vma; } g_vk;
static uint64_t completed,pending;
static unsigned released,seen[64],order[64];
static uint64_t vk_completed_seq(void) {return completed;}
static uint64_t vk_pending_seq(void) {return pending;}
static void vkDestroyImageView(int dev,VkImageView view,void *allocator) {
    (void)dev;(void)allocator;assert(view<64 && !seen[view]++);order[released++]=(unsigned)view;
}
static void vmaDestroyImage(int vma,VkImage image,VmaAllocation memory) {
    (void)vma;assert(image<64 && memory==image+10000 && !seen[image]++);order[released++]=(unsigned)image;
}
static void *race_realloc(void *,size_t);
#define realloc race_realloc
#define FATAL(...) abort()
'''
start = source.index('typedef struct { VkImage img; VmaAllocation mem; VkImageView view; uint64_t seq; } Dead;')
end = source.index('static void create_image(', start)
harness += source[start:end]
for signature in ('static void defer(', 'void vk_defer_image(', 'void vk_defer_view('):
    harness += function(signature)
harness += r'''
#undef realloc
static pthread_mutex_t schedule=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake=PTHREAD_COND_INITIALIZER;
static bool growing,second_ready,second_finished,forced_growth;
static Dead old_storage[2],new_storage[4];
static unsigned allocations;
static void *race_realloc(void *before,size_t bytes) {
    if(!forced_growth)return realloc(before,bytes);
    assert(bytes<=sizeof new_storage);
    if(allocations++) {assert(before==new_storage);return new_storage;}
    assert(before==old_storage);
    pthread_mutex_lock(&schedule);growing=true;pthread_cond_broadcast(&wake);
    while(!second_ready)pthread_cond_wait(&wake,&schedule);
    struct timespec deadline;timespec_get(&deadline,TIME_UTC);
    deadline.tv_nsec+=200000000;
    if(deadline.tv_nsec>=1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
    // The repaired queue keeps the second writer waiting until growth finishes.
    while(!second_finished && !pthread_cond_timedwait(&wake,&schedule,&deadline)) {}
    memcpy(new_storage,old_storage,sizeof(Dead));
    pthread_mutex_unlock(&schedule);
    return new_storage;
}
static void *first_writer(void *unused) {(void)unused;vk_defer_image(20,10020);return NULL;}
static void *second_writer(void *unused) {
    (void)unused;pthread_mutex_lock(&schedule);
    while(!growing)pthread_cond_wait(&wake,&schedule);
    second_ready=true;pthread_cond_broadcast(&wake);pthread_mutex_unlock(&schedule);
    vk_defer_image(30,10030);
    pthread_mutex_lock(&schedule);second_finished=true;pthread_cond_broadcast(&wake);pthread_mutex_unlock(&schedule);
    return NULL;
}
int main(void) {
    pending=7;completed=6;
    vk_defer_view(1);vk_defer_image(2,10002);vk_collect_dead();assert(!released && ndead==2);
    completed=7;vk_collect_dead();assert(released==2 && !ndead && order[0]==1 && order[1]==2);
    free(dead);dead=NULL;cap_dead=ndead=0;
    // A skipped future entry survives compaction and is destroyed exactly once.
    pending=10;vk_defer_image(3,10003);pending=8;vk_defer_image(4,10004);
    completed=8;vk_collect_dead();assert(ndead==1 && seen[4]==1 && !seen[3]);
    completed=10;vk_collect_dead();assert(!ndead && seen[3]==1);
    free(dead);
    old_storage[0]=(Dead){.img=10,.mem=10010,.seq=11};
    old_storage[1]=(Dead){.seq=0xbadcafe};
    dead=old_storage;cap_dead=ndead=1;pending=11;completed=10;forced_growth=true;
    pthread_t first,second;
    assert(!pthread_create(&first,NULL,first_writer,NULL));
    assert(!pthread_create(&second,NULL,second_writer,NULL));
    pthread_join(first,NULL);pthread_join(second,NULL);
    assert(old_storage[1].seq==0xbadcafe && !old_storage[1].img); // no write beyond the old capacity
    assert(ndead==3 && cap_dead>=ndead);
    vk_collect_dead();assert(ndead==3 && !seen[10] && !seen[20] && !seen[30]);
    completed=11;vk_collect_dead();assert(!ndead && seen[10]==1 && seen[20]==1 && seen[30]==1);
    vk_collect_dead();assert(released==7);
    puts("PASS: concurrent texture retirement during growth, completed-sequence gating, ordered view/image release and exactly-once collection");
}
'''
with tempfile.TemporaryDirectory(prefix='texture-retirement-') as temporary:
    directory=Path(temporary);file=directory/'case.c';file.write_text(harness)
    exe=directory/('case.exe' if os.name=='nt' else 'case')
    subprocess.run(shlex.split(os.environ.get('CC','clang'))+['-std=gnu11','-O1','-pthread',str(file),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)
