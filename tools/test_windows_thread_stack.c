#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include "../runtime/platform/windows_threads.h"
#include <psapi.h>

static HANDLE ready, release_threads;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned waiting, finished;
static size_t stack_reserved, stack_committed;

__attribute__((noinline)) static unsigned grow_stack(unsigned n) {
    volatile uint8_t pages[16384];
    for (unsigned i=0;i<sizeof pages;i+=4096) pages[i]=(uint8_t)n;
    unsigned result=n ? grow_stack(n-1) : 0;
    return result+pages[0]+pages[4096]+pages[8192]+pages[12288];
}
static void *worker(void *argument) {
    unsigned index=(unsigned)(uintptr_t)argument;
    if (!index) {
        MEMORY_BASIC_INFORMATION info; VirtualQuery(&index,&info,sizeof info);
        uint8_t *base=info.AllocationBase, *at=base;
        while (VirtualQuery(at,&info,sizeof info) && info.AllocationBase==base) {
            stack_reserved+=info.RegionSize;
            if (info.State==MEM_COMMIT) stack_committed+=info.RegionSize;
            at+=info.RegionSize;
        }
        assert(grow_stack(256)>0); // grow beyond the small initial commit
    }
    pthread_mutex_lock(&lock);
    if (++waiting==16) SetEvent(ready);
    pthread_mutex_unlock(&lock);
    assert(WaitForSingleObject(release_threads,30000)==WAIT_OBJECT_0);
    pthread_mutex_lock(&lock); finished++; pthread_mutex_unlock(&lock);
    return NULL;
}
static size_t private_bytes(void) {
    PROCESS_MEMORY_COUNTERS_EX p={0}; p.cb=sizeof p;
    assert(GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS *)&p,sizeof p));
    return p.PrivateUsage;
}
int main(void) {
    ready=CreateEventW(NULL,TRUE,FALSE,NULL); release_threads=CreateEventW(NULL,TRUE,FALSE,NULL);
    assert(ready && release_threads); size_t before=private_bytes();
    uintptr_t threads[16];
    for(unsigned i=0;i<16;i++) { threads[i]=guest_thread_create(worker,(void *)(uintptr_t)i,256u<<20); assert(threads[i]); }
    assert(WaitForSingleObject(ready,30000)==WAIT_OBJECT_0);
    size_t after=private_bytes();
    assert(stack_reserved>=(256u<<20) && stack_committed<(4u<<20));
    assert(after<before+(64u<<20));
    SetEvent(release_threads);
    for(unsigned i=0;i<16;i++) assert(guest_thread_join(threads[i]));
    assert(finished==16); CloseHandle(ready); CloseHandle(release_threads);
    printf("PASS: 16 threads reserve 256 MB each, commit growth %zu KB, stack growth, pthread locking and joins\n",(after-before)/1024);
}
