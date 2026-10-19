#include <assert.h>
#include <stdio.h>
#include "../runtime/services/audio_workbuffer.h"

int main(void) {
    uint32_t p[13] = {48000,240,102,16,96,1,32,0,0,96,384,0,0};
    // Evaluated independently from the pinned C# reference's capacity methods.
    // VoiceInfo/VoiceState footprints were checked with Unsafe.SizeOf: 0x218/0xd8.
    const struct { unsigned revision; uint64_t normal, performance; } cases[] = {
        {1,0x1f2000,0x1f5000}, {2,0x208000,0x20c000}, {4,0x208000,0x20c000},
        {5,0x208000,0x20e000}, {8,0x208000,0x20e000}, {12,0x21b000,0x221000},
        {15,0x21b000,0x221000}, {16,0x21b000,0x221000}
    };
    for (unsigned i=0; i<sizeof cases/sizeof *cases; i++) {
        p[12]=0x30564552u+(cases[i].revision<<24);
        uint64_t size=0;
        p[7]=0; assert(audio_workbuffer_size(p,sizeof p,&size) && size==cases[i].normal);
        p[7]=3; assert(audio_workbuffer_size(p,sizeof p,&size) && size==cases[i].performance);
        assert(!(size & 0xfff));
    }
    uint64_t sentinel=123;
    for (size_t n=0; n<sizeof p; n++) {
        assert(!audio_workbuffer_size(p,n,&sentinel)); assert(sentinel==123);
    }
    uint8_t unaligned[sizeof p+3]; memset(unaligned,0xcc,sizeof unaligned);
    memcpy(unaligned+1,p,sizeof p);
    uint64_t size; assert(audio_workbuffer_size(unaligned+1,sizeof p+2,&size) && size==0x221000);
    uint32_t saved=p[12];
    for(unsigned i=0;i<3;i++) {
        p[12]=(uint32_t[]){0x30564552,0x41564552,0x3f564553}[i];
        assert(!audio_workbuffer_size(p,sizeof p,&sentinel) && sentinel==123);
    }
    p[12]=saved;
    // Arithmetic cannot wrap a huge request into a small successful allocation.
    p[3]=UINT32_MAX;
    assert(!audio_workbuffer_size(p,sizeof p,&sentinel) && sentinel==123);
    p[3]=16; p[1]=p[2]=UINT32_MAX;
    assert(!audio_workbuffer_size(p,sizeof p,&sentinel) && sentinel==123);
    uint64_t full=UINT64_MAX;
    assert(!audio_work_reserve(&full,0,1,0x1000) && full==UINT64_MAX);
    assert(!audio_work_reserve(&full,1,1,1) && full==UINT64_MAX);
    full=0; assert(!audio_work_reserve(&full,UINT64_MAX,2,1) && !full);
    puts("PASS: audio work-buffer reference capacities, revision gates, performance history, alignment, short/unaligned requests and overflow");
}
