#include <assert.h>
#include <stdio.h>
#include "../runtime/services/opus.c"

pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static KTransferMem transfer = {.obj.type=KOBJ_TMEM, .size=UINT32_MAX};
KObject *handle_get(uint32_t h) { return h == 0x200 ? &transfer.obj : NULL; }
static bool pause_clock, clock_paused, resume_clock;
static pthread_mutex_t clock_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t clock_cond=PTHREAD_COND_INITIALIZER;
uint64_t plat_now_ns(void) {
    static uint64_t time;
    if(__atomic_exchange_n(&pause_clock,false,__ATOMIC_ACQ_REL)) {
        pthread_mutex_lock(&clock_lock); clock_paused=true; pthread_cond_signal(&clock_cond);
        while(!resume_clock) pthread_cond_wait(&clock_cond,&clock_lock);
        pthread_mutex_unlock(&clock_lock);
    }
    return __atomic_add_fetch(&time,100,__ATOMIC_RELAXED);
}
void rt_fatal_msg(const char *fmt, ...) { (void)fmt; abort(); }
void rt_fatal_trace(void) { abort(); }
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    Service *s = calloc(1, sizeof *s); assert(s);
    s->name = name; s->handler = handler; s->state = state; return s;
}
void ipc_return_object(IpcResp *r, Service *s) { r->objects[r->nobjects++] = s; }

static void be32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static uint32_t wire[70], inline_data[8];
static IpcReq request(uint32_t cmd, int rate, int channels, bool large, bool multi) {
    memset(wire, 0, sizeof wire); memset(inline_data, 0, sizeof inline_data);
    wire[0]=rate; wire[1]=channels;
    if (multi) {
        wire[2]=4; wire[3]=2;
        memcpy(wire+4, (uint8_t[]){0,4,1,2,3,5}, 6);
        wire[68]=large;
    } else wire[2]=large;
    bool open = cmd==0 || cmd==2 || cmd==4 || cmd==6;
    IpcReq r = {.cmd=cmd, .data=multi ? (uint8_t *)inline_data : (uint8_t *)wire,
                .data_size=multi ? (open ? 4 : 0) : (cmd>=4 ? (open ? 20 : 16) : (open ? 12 : 8))};
    if (multi) { r.nx=1; r.x[0]=(IpcBuf){(uintptr_t)wire, cmd>=4 ? 0x118 : 0x110}; }
    if (open) { r.ncopy=1; r.copy_handles[0]=0x200; }
    return r;
}
static Service *open_decoder(int rate, int channels, bool large, bool multi, uint32_t query) {
    IpcReq r=request(query,rate,channels,large,multi); IpcResp out={0}; hwopus(NULL,&r,&out);
    assert(!out.result && out.data_size==4 && !out.nobjects);
    uint32_t size=read_u32(out.data); assert(size>0);
    r=request(multi ? (query==3 ? 2 : 6) : (query==1 ? 0 : 4),rate,channels,large,multi);
    uint32_t *size_field=multi ? inline_data : wire+(query==1 ? 2 : 4);
    *size_field=size;
    out=(IpcResp){0}; hwopus(NULL,&r,&out); assert(!out.result && out.nobjects==1);
    Service *s=out.objects[0]; assert(s->state);
    *size_field=size-1; out=(IpcResp){0}; hwopus(NULL,&r,&out); assert(out.result && !out.nobjects);
    *size_field=size; r.copy_handles[0]=0x999; out=(IpcResp){0}; hwopus(NULL,&r,&out); assert(out.result && !out.nobjects);
    return s;
}
static IpcResp decode(Service *s, uint32_t cmd, bool reset, uint8_t *packet, size_t bytes, void *pcm, size_t room) {
    uint8_t flag=reset;
    IpcReq r={.cmd=cmd,.data=&flag,.data_size=1,.na=1,.nb=1,
              .a={{(uintptr_t)packet,bytes}},.b={{(uintptr_t)pcm,room}}};
    if (cmd>=8) { r.nx=r.nc=1; r.x[0]=r.a[0]; r.c[0]=r.b[0]; r.a[0].size=r.b[0].size=0; }
    IpcResp out={0}; s->handler(s,&r,&out); return out;
}
static void close_decoder(Service *s) { s->on_close(s); assert(!s->state); s->on_close(s); free(s); }

typedef struct { Service *service; uint8_t *packet; size_t bytes, pcm_bytes; int16_t *expected; bool multi; } Concurrent;
static void *decode_during_close(void *arg) {
    Concurrent *c=arg;
    uint8_t *pcm=malloc(c->pcm_bytes); assert(pcm);
    IpcResp out=decode(c->service,c->multi ? 9 : 8,true,c->packet,c->bytes,pcm,c->pcm_bytes);
    assert(!out.result && !memcmp(pcm,c->expected,c->pcm_bytes));
    out=decode(c->service,c->multi ? 9 : 8,true,c->packet,c->bytes,pcm,c->pcm_bytes);
    assert(out.result);
    free(pcm); return NULL;
}
static void *decode_concurrent(void *arg) {
    Concurrent *c=arg;
    uint8_t *pcm=malloc(c->pcm_bytes); assert(pcm);
    for(int i=0;i<64;++i) {
        IpcResp out=decode(c->service,c->multi ? 9 : 8,true,c->packet,c->bytes,pcm,c->pcm_bytes);
        assert(!out.result && !memcmp(pcm,c->expected,c->pcm_bytes));
    }
    free(pcm); return NULL;
}

static void run_audio(int rate, int channels, bool multi, uint32_t query, bool large) {
    Service *s=open_decoder(rate,channels,large,multi,query);
    int err, n=rate/50, max=rate*120/1000;
    uint8_t mapping[]={0,4,1,2,3,5};
    void *enc=multi ? (void *)opus_multistream_encoder_create(rate,channels,4,2,mapping,OPUS_APPLICATION_AUDIO,&err) :
                      (void *)opus_encoder_create(rate,channels,OPUS_APPLICATION_AUDIO,&err);
    assert(enc && err==OPUS_OK);
    void *ref=multi ? (void *)opus_multistream_decoder_create(rate,channels,4,2,mapping,&err) :
                      (void *)opus_decoder_create(rate,channels,&err);
    assert(ref && err==OPUS_OK);
    int16_t *input=calloc((size_t)max*channels,2), *expected=calloc((size_t)max*channels,2);
    size_t pcm_size=(size_t)max*channels*2;
    uint8_t *output=malloc(pcm_size+16), *packet=malloc(65536); assert(input&&expected&&output&&packet);
    for (int seq=0;seq<6;++seq) {
        for(int i=0;i<n;++i) for(int ch=0;ch<channels;++ch)
            input[i*channels+ch]=(int16_t)(((i+seq*n)*(137+ch*31))%24000-12000);
        int bytes=multi ? opus_multistream_encode(enc,input,n,packet+8,65528) : opus_encode(enc,input,n,packet+8,65528);
        assert(bytes>0);
        opus_uint32 range=0;
        err=multi ? opus_multistream_encoder_ctl(enc,OPUS_GET_FINAL_RANGE(&range)) : opus_encoder_ctl(enc,OPUS_GET_FINAL_RANGE(&range));
        assert(err==OPUS_OK); be32(packet,bytes); be32(packet+4,range);
        uint32_t cmd=multi ? (uint32_t[]){2,5,7,9,9,9}[seq] : (uint32_t[]){0,4,6,8,8,8}[seq];
        bool reset=seq==4;
        memset(output,0xa5,pcm_size+16);
        // Both failures must preserve the complete output and the running decoder history.
        IpcResp out=decode(s,cmd,reset,packet,bytes+7,output+7,pcm_size);
        assert(out.result && !out.data_size);
        out=decode(s,cmd,reset,packet,bytes+8,output+7,(size_t)(n-1)*channels*2);
        assert(out.result && !out.data_size);
        be32(packet+4,range ? range^1 : 1);
        out=decode(s,cmd,reset,packet,bytes+8,output+7,pcm_size); assert(out.result && !out.data_size);
        for(size_t i=0;i<pcm_size+16;++i) assert(output[i]==0xa5);
        be32(packet+4,range);
        if(reset) { err=multi ? opus_multistream_decoder_ctl(ref,OPUS_RESET_STATE) : opus_decoder_ctl(ref,OPUS_RESET_STATE); assert(!err); }
        int got=multi ? opus_multistream_decode(ref,packet+8,bytes,expected,max,0) : opus_decode(ref,packet+8,bytes,expected,max,0);
        assert(got==n);
        out=decode(s,cmd,reset,packet,bytes+8,output+7,pcm_size);
        assert(!out.result && out.data_size==(cmd>=4 ? 16u : 8u));
        assert(read_u32(out.data)==(uint32_t)bytes+8 && read_u32(out.data+4)==(uint32_t)n);
        assert(!memcmp(output+7,expected,(size_t)n*channels*2));
        for(int i=0;i<7;++i) assert(output[i]==0xa5);
        for(size_t i=7+(size_t)n*channels*2;i<pcm_size+16;++i) assert(output[i]==0xa5);
    }
    // Cloned sessions can call the same decoder concurrently; each reset packet has a known independent output.
    if(rate==48000) {
        err=multi ? opus_multistream_decoder_ctl(ref,OPUS_RESET_STATE) : opus_decoder_ctl(ref,OPUS_RESET_STATE); assert(!err);
        int bytes=(int)read_be32(packet);
        int got=multi ? opus_multistream_decode(ref,packet+8,bytes,expected,max,0) : opus_decode(ref,packet+8,bytes,expected,max,0);
        assert(got==n);
        Concurrent c={s,packet,(size_t)bytes+8,(size_t)n*channels*2,expected,multi};
        pthread_t threads[4];
        for(int i=0;i<4;++i) assert(!pthread_create(&threads[i],NULL,decode_concurrent,&c));
        for(int i=0;i<4;++i) assert(!pthread_join(threads[i],NULL));
    }
    // 120 ms packets need the extended workspace, and are rejected by a legacy 40 ms decoder.
    int long_n=rate*120/1000;
    for(int i=0;i<long_n*channels;++i) input[i]=(int16_t)(i*17);
    int bytes=multi ? opus_multistream_encode(enc,input,long_n,packet+8,65528) : opus_encode(enc,input,long_n,packet+8,65528);
    assert(bytes>0); be32(packet,bytes); be32(packet+4,0);
    IpcResp out=decode(s,multi ? 9 : 8,true,packet,bytes+8,output+7,pcm_size);
    assert(large ? (!out.result && read_u32(out.data+4)==(uint32_t)long_n) : out.result!=0);
    if(large && rate==48000) {
        // Hold a real request inside its clock call, close the service, then let that request finish.
        err=multi ? opus_multistream_decoder_ctl(ref,OPUS_RESET_STATE) : opus_decoder_ctl(ref,OPUS_RESET_STATE); assert(!err);
        int got=multi ? opus_multistream_decode(ref,packet+8,bytes,expected,max,0) : opus_decode(ref,packet+8,bytes,expected,max,0);
        assert(got==long_n);
        Concurrent c={s,packet,(size_t)bytes+8,(size_t)long_n*channels*2,expected,multi};
        clock_paused=resume_clock=false;
        __atomic_store_n(&pause_clock,true,__ATOMIC_RELEASE);
        pthread_t thread; assert(!pthread_create(&thread,NULL,decode_during_close,&c));
        pthread_mutex_lock(&clock_lock);
        while(!clock_paused) pthread_cond_wait(&clock_cond,&clock_lock);
        s->on_close(s); assert(!s->state);
        resume_clock=true; pthread_cond_signal(&clock_cond); pthread_mutex_unlock(&clock_lock);
        assert(!pthread_join(thread,NULL));
    }
    close_decoder(s);
    if(multi) { opus_multistream_encoder_destroy(enc); opus_multistream_decoder_destroy(ref); }
    else { opus_encoder_destroy(enc); opus_decoder_destroy(ref); }
    free(input);free(expected);free(output);free(packet);
}

int main(void) {
    int rates[]={8000,12000,16000,24000,48000};
    for(int i=0;i<5;++i) for(int channels=1;channels<=2;++channels)
        run_audio(rates[i],channels,false,(uint32_t[]){1,5,8}[i%3],i%3!=0);
    run_audio(48000,6,true,3,false);
    run_audio(48000,6,true,7,true);
    run_audio(12000,6,true,9,true);
    for(int cmd=0;cmd<=9;++cmd) {
        IpcReq r={.cmd=cmd}; IpcResp out={0}; hwopus(NULL,&r,&out); assert(out.result && !out.nobjects);
    }
    for(int which=0;which<5;++which) {
        IpcReq r=request(8,48000,2,true,false);
        if(which==0) wire[0]=44100;
        if(which==1) wire[1]=0;
        if(which==2) wire[1]=3;
        if(which==3) wire[2]=2;
        if(which==4) r.data_size=15;
        IpcResp out={0}; hwopus(NULL,&r,&out); assert(out.result && !out.data_size);
    }
    for(int which=0;which<7;++which) {
        IpcReq r=request(9,48000,6,true,true);
        if(which==0) wire[1]=256;
        if(which==1) wire[2]=0;
        if(which==2) wire[3]=5;
        if(which==3) ((uint8_t *)(wire+4))[0]=6;
        if(which==4) wire[68]=2;
        if(which==5) r.x[0].size=0x117;
        if(which==6) r.x[0].addr=UINT64_MAX-10;
        IpcResp out={0}; hwopus(NULL,&r,&out); assert(out.result && !out.data_size);
    }
    for(int which=0;which<4;++which) {
        IpcReq r=request(4,48000,2,true,false); wire[4]=0x100000;
        if(which==0) r.ncopy=0;
        if(which==1) r.ncopy=2;
        if(which==2) transfer.size=0x1000;
        if(which==3) transfer.obj.type=KOBJ_SHMEM;
        IpcResp out={0}; hwopus(NULL,&r,&out); assert(out.result && !out.nobjects);
        transfer.size=UINT32_MAX; transfer.obj.type=KOBJ_TMEM;
    }
    Service *a=open_decoder(48000,1,false,false,1), *b=open_decoder(48000,2,true,false,8);
    close_decoder(a);
    opus_reset(); assert(!b->state); close_decoder(b); opus_reset();
    Service *fresh=open_decoder(48000,2,false,false,1); close_decoder(fresh);
    puts("PASS: hwopus manager ABIs, rates, mono/stereo/multistream PCM, 120 ms frames, streaming/reset, ranges, output guards, concurrency and reboot cleanup");
}
