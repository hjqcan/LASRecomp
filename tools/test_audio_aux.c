#include <assert.h>
#include <stdio.h>
#include "../runtime/services/audio_aux.h"

static void put32(uint8_t *p,uint32_t v){memcpy(p,&v,4);}
static void put64(uint8_t *p,uint64_t v){memcpy(p,&v,8);}
static int32_t sample(const uint32_t *p,unsigned i){int32_t v;memcpy(&v,p+32+i,4);return v;}
int main(void) {
    enum { CAP=12,N=3 };
    uint32_t send[32+CAP+1]={0},receive[32+CAP+1]={0};
    send[32+CAP]=receive[32+CAP]=0xdeadbeef;
    send[0]=0x1234;receive[0]=0x5678;send[18]=111;receive[18]=222;
    uint8_t p[108]={0};p[0]=0;p[1]=1;p[24]=2;p[25]=3;put32(p+48,2);put32(p+56,CAP);
    put64(p+64,(uintptr_t)send);put64(p+80,(uintptr_t)receive);
    // The redundant storage-address fields are not used to locate ring data.
    put64(p+72,1);put64(p+88,1);
    AudioAux a={0};audio_aux_update(&a,p);assert(a.valid);
    AudioAuxPool pools[2]={{(uintptr_t)send,(32+CAP)*4},{(uintptr_t)receive,(32+CAP)*4}};
    int32_t returns[CAP]={-16384,32768,65536,8192,0,-32768,16384,8192,0,-8192,-16384,-32768};
    memcpy(receive+32,returns,sizeof returns);
    float b[13]={.25f+1/65536.f,.5f,-.75f,1,2,-2,0,0,0,0,0,0,12345};
    assert(audio_aux_process(&a,b,4,N,true,pools,2));
    assert(sample(send,0)==8192 && sample(send,1)==16384 && sample(send,2)==-24576);
    assert(sample(send,3)==32768 && sample(send,4)==65536 && sample(send,5)==-65536);
    assert(b[0]==.25f && b[6]==-.5f && b[7]==1 && b[8]==2 && b[9]==.25f && b[11]==-1);
    assert(send[17]==6 && receive[16]==6 && b[12]==12345);
    b[0]=.75f;assert(audio_aux_process(&a,b,4,N,true,pools,2));
    assert(sample(send,6)==24576 && send[17]==0 && receive[16]==0);
    assert(b[6]==.5f && b[7]==.25f && b[11]==-1);
    // The last channel can wrap inside its block; one offset update covers every channel.
    send[17]=receive[16]=8; b[0]=.125f;b[1]=.25f;b[2]=.5f;b[3]=1;b[4]=2;b[5]=3;
    assert(audio_aux_process(&a,b,4,N,true,pools,2));
    assert(sample(send,8)==4096 && sample(send,11)==32768 && sample(send,0)==65536 && sample(send,1)==98304);
    assert(send[17]==2 && receive[16]==2 && b[6]==0 && b[7]==-.25f && b[8]==-.5f);
    assert(b[9]==-1 && b[10]==-.5f && b[11]==1);
    assert(send[32+CAP]==0xdeadbeef && receive[32+CAP]==0xdeadbeef);
    // Bypass resets DSP offsets and total counts, without touching CPU state or loss counters.
    send[16]=3;send[19]=99;receive[17]=5;receive[19]=88;
    assert(audio_aux_process(&a,b,4,N,false,pools,2));
    assert(send[16]==0 && send[17]==0 && send[19]==0 && receive[16]==0 && receive[17]==0 && receive[19]==0);
    assert(send[0]==0x1234 && receive[0]==0x5678 && send[18]==111 && receive[18]==222);
    assert(b[6]==b[0] && b[9]==b[3]);
    p[24]=1;p[25]=0;audio_aux_update(&a,p);b[0]=3;b[1]=9;
    assert(audio_aux_process(&a,b,2,1,false,pools,2));assert(b[0]==9 && b[1]==3);
    // Detached/short ranges, bad indices, bad offsets and overflow never dereference invalid addresses.
    float before[13];memcpy(before,b,sizeof b);pools[1].size--;
    assert(!audio_aux_process(&a,b,2,1,true,pools,2));assert(!memcmp(before,b,sizeof b));pools[1].size++;
    send[17]=CAP;assert(!audio_aux_process(&a,b,2,1,true,pools,2));send[17]=0;
    p[24]=24;audio_aux_update(&a,p);assert(!audio_aux_process(&a,b,2,1,true,pools,2));
    assert(!audio_aux_mapped(UINT64_MAX-3,8,pools,2));
    assert(audio_aux_pcm(NAN)==0 && audio_aux_pcm(INFINITY)==0 && audio_aux_pcm(1e20f)==INT32_MAX);
    assert(audio_aux_pcm(-1e20f)==INT32_MIN);
    AudioAuxPool pool={0};uint8_t packet[32]={0};put64(packet,1234);put64(packet+8,64);put32(packet+16,4);
    audio_aux_pool_update(&pool,packet);assert(pool.address==1234 && pool.size==64);
    memset(packet,0,16);put32(packet+16,5);audio_aux_pool_update(&pool,packet);assert(pool.size==64);
    put32(packet+16,2);audio_aux_pool_update(&pool,packet);assert(!pool.size);
    puts("PASS: guest auxiliary PCM exchange, planar ring wrap, frame offsets, bypass, pool lifetime and bounds");
}
