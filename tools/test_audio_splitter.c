#include <assert.h>
#include <stdio.h>
#include "../runtime/services/audio_splitter.h"

#ifdef TEST_AUDIO_RENDERER
#include "../runtime/hle.h"
#include "../runtime/ipc.h"
#undef SERVICE
#define SERVICE(name, fn) static void fn(Service *s, IpcReq *req, IpcResp *resp)
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/services/audio.c"
Settings g_settings = {0};
#endif

static void u32(uint8_t *p, uint32_t x) { memcpy(p, &x, 4); }
static void f32(uint8_t *p, float x) { memcpy(p, &x, 4); }
#ifdef TEST_AUDIO_RENDERER
static void u64(uint8_t *p, uint64_t x) { memcpy(p, &x, 8); }
#endif
static size_t state(uint8_t *p, uint32_t id, const uint32_t *dest, uint32_t n) {
    u32(p, 0x49444e53); u32(p + 4, id); u32(p + 8, 48000); u32(p + 12, n);
    if (n) memcpy(p + 16, dest, n * 4);
    return 28 + n * 4;
}
static void destination(uint8_t *p, uint32_t id, int mix, float l, float r) {
    u32(p, 0x44444e53); u32(p + 4, id); f32(p + 8, l); f32(p + 12, r);
    u32(p + 104, mix); p[108] = 1;
}
static size_t routing_packet(uint8_t *p) {
    memset(p, 0, 512); u32(p, 0x48444e53); u32(p + 4, 2); u32(p + 8, 3);
    uint32_t ids[] = {0, 1}, next[] = {2};
    size_t at = 32;
    at += state(p + at, 0, ids, 2);
    at += state(p + at, 1, next, 1);
    destination(p + at, 0, 2, .25f, 0); at += 0x70;
    destination(p + at, 1, 0, 0, .5f); at += 0x70;
    destination(p + at, 2, 1, .5f, 0); at += 0x70;
    return (at + 15) & ~(size_t)15;
}

#ifdef TEST_AUDIO_RENDERER
static void mixer_test(const uint8_t *packet, size_t size) {
    Renderer r = {.sample_rate=48000, .sample_count=4, .voices=1, .nmix=3,
                  .sink_used=1, .sink_inputs=2, .sink_input={0,1}};
    Voice voice = {.in_use=1, .is_new=1, .chans=1, .fmt=2, .rate=48000, .pitch=1,
                   .volume=1, .wb_count=1, .mix_id=INT32_MAX, .splitter_id=0};
    int16_t pcm[64]; for (unsigned i=0; i<64; i++) pcm[i]=8192;
    voice.wb[0]=(WaveBuf){.addr=(uintptr_t)pcm, .size=sizeof pcm, .end=64, .loop=1, .loop_count=-1};
    ChanState cs={0}; ChanRes cr={0}; Mix mixes[3]={0}; float samples[3][24*4]={0};
    r.voice=&voice; r.chst=&cs; r.chres=&cr; r.mix=mixes;
    for (int i=0; i<3; i++) mixes[i]=(Mix){.used=1,.volume=1,.bufs=2,.dest=INT32_MAX,.dest_splitter=UINT32_MAX,.buf=samples[i]};
    mixes[1].dest=0; mixes[1].vol[0]=1;
    mixes[2].dest_splitter=1;
    assert(audio_split_init(&r.routing,2,3)); assert(audio_split_update(&r.routing,packet,size));
    float stereo[8]; render_frame(&r,stereo); render_frame(&r,stereo);
    for (int i=0; i<4; i++) {
        assert(fabsf(stereo[i*2]-.03125f)<1e-6f);
        assert(fabsf(stereo[i*2+1]-.125f)<1e-6f);
    }
    // Direct voice routing remains intact when no splitter is selected.
    voice.mix_id=0; cr.mix[0]=cr.prev[0]=1; cr.mix[1]=cr.prev[1]=.5f;
    render_frame(&r,stereo);
    for (int i=0; i<4; i++) {
        assert(fabsf(stereo[i*2]-.25f)<1e-6f);
        assert(fabsf(stereo[i*2+1]-.125f)<1e-6f);
    }
    audio_split_free(&r.routing);
    // Stereo uses alternate destination entries, not both channels in every
    // destination. Distinct channel amplitudes make accidental fan-out visible.
    Voice pair[2]={voice,{0}}; ChanState states[2]={0}; ChanRes resources[2]={0};
    int16_t interleaved[128];
    for (unsigned i=0; i<64; i++) { interleaved[i*2]=8192; interleaved[i*2+1]=16384; }
    r.voices=2; r.voice=pair; r.chst=states; r.chres=resources;
    pair[0].is_new=1; pair[0].chans=2; pair[0].chan_ids[1]=1; pair[0].mix_id=INT32_MAX;
    pair[0].wb[0]=(WaveBuf){.addr=(uintptr_t)interleaved,.size=sizeof interleaved,.end=64,.loop=1,.loop_count=-1};
    mixes[1].used=mixes[2].used=0;
    assert(audio_split_init(&r.routing,2,3)); assert(audio_split_update(&r.routing,packet,size));
    r.routing.destination[0].mix_id=0;
    r.routing.destination[0].volume[0]=r.routing.destination[0].previous[0]=1;
    r.routing.destination[1].volume[1]=r.routing.destination[1].previous[1]=1;
    render_frame(&r,stereo); render_frame(&r,stereo);
    for (int i=0; i<4; i++) {
        assert(fabsf(stereo[i*2]-.25f)<1e-6f);
        assert(fabsf(stereo[i*2+1]-.5f)<1e-6f);
    }
    audio_split_free(&r.routing);
    puts("PASS: PCM voice fan-out, reversed submix ordering, splitter to submix to final mix, direct-route parity");
    puts("PASS: stereo channel destination stride");
}

static void delay_mixer_test(void) {
    Effect fx[2] = {0};
    Renderer r = {.sample_count=4, .effects=2, .fx=fx};
    float samples[9] = {1,0,0,0,0,0,0,0,12345};
    Mix mix = {.bufs=2, .buf=samples};
    uint8_t delay[0xc0] = {3,1,1}, biquad[0xc0] = {6,1,1};
    u32(delay+4,4); u32(delay+0x18,1);  // delay follows the biquad, despite its lower slot
    AudioDelayParam p = {.channels=1,.channels_max=1,.output={1},.delay_ms=2,.delay_max=4,
                         .sample_rate=1000*16384,.in_gain=16384,.out_gain=16384};
    memcpy(delay+0x20,&p,sizeof p); delay[0x20+52]=2;
    u32(biquad+4,4); biquad[0x20+22]=1; biquad[0x20+23]=1;
    int16_t half=8192; memcpy(biquad+0x20+12,&half,2);
    effect_update(&fx[0],delay,0x38564552); effect_update(&fx[1],biquad,0x38564552);
    apply_effects(&r,0,&mix); assert(samples[0]==1 && samples[5]==0);
    apply_effects(&r,4,&mix); assert(samples[0]==.5f && samples[5]==.5f && samples[8]==12345);
    delay[1]=0; delay[2]=0; effect_update(&fx[0],delay,0x38564552);
    memset(samples,0,8*sizeof(float)); samples[0]=.5f;
    apply_effects(&r,4,&mix); assert(samples[4]==.25f);  // disabled delay still routes its input
    delay[0]=0; effect_update(&fx[0],delay,0x38564552); assert(!fx[0].delay.lines);
    // The 3D reverb shares the same ordered effect dispatch and global bypass.
    AudioReverb3dParam rv={.channels=1,.channels_max=1,.sample_rate=48000,.hf_reference=2000,
        .decay_time=3,.hf_decay_ratio=1,.reflection_delay=.05f,.density=100,.diffusion=100,.dry_gain=.5f,.output={1}};
    delay[0]=5;delay[1]=1;delay[2]=1;memcpy(delay+0x20,&rv,sizeof rv);delay[0x20+72]=2;
    effect_update(&fx[0],delay,0x38564552);g_settings.reverb=1;
    memset(samples,0,8*sizeof(float));samples[0]=1;apply_effects(&r,4,&mix);assert(samples[4]==.25f);
    g_settings.reverb=0;memset(samples,0,8*sizeof(float));samples[0]=1;
    apply_effects(&r,4,&mix);assert(samples[4]==.5f);
    delay[0]=0;effect_update(&fx[0],delay,0x38564552);assert(!fx[0].rv3.pre.data);
    uint32_t send[40]={0},receive[40]={0};receive[32]=4096;
    AudioAuxPool pools[2]={{(uintptr_t)send,sizeof send},{(uintptr_t)receive,sizeof receive}};
    r.pools=pools;r.pool_count=2;
    memset(delay,0,sizeof delay);delay[0]=2;delay[1]=1;delay[2]=1;u32(delay+4,4);u32(delay+0x18,1);
    delay[0x20+24]=1;u32(delay+0x20+48,1);u32(delay+0x20+56,8);
    u64(delay+0x20+64,(uintptr_t)send);u64(delay+0x20+80,(uintptr_t)receive);
    effect_update(&fx[0],delay,0x38564552);memset(samples,0,8*sizeof(float));samples[0]=16384;
    apply_effects(&r,4,&mix);assert(send[32]==8192 && samples[4]==4096 && !fx[0].aux_invalid);
    assert(send[17]==4 && receive[16]==4);
    delay[2]=0;effect_update(&fx[0],delay,0x38564552);r.pool_count=0;
    memset(samples,0,8*sizeof(float));samples[0]=1;apply_effects(&r,4,&mix);assert(samples[4]==.5f);
    puts("PASS: production effect dispatch, ordered biquad/delay chain, mix selection, bypass and slot retirement");
    puts("PASS: production reverb3d dispatch, global reverb setting and slot retirement");
    puts("PASS: production auxiliary-buffer dispatch, guest PCM exchange and detached bypass");
}

static void auxiliary_sink_level_test(void) {
    enum { N=4, CAP=2*N };
    Renderer r={.sample_rate=48000,.sample_count=N,.voices=1,.nmix=1,.effects=1,
                .sink_used=1,.sink_inputs=2,.sink_input={0,1}};
    Voice voice={.in_use=1,.is_new=1,.chans=1,.fmt=2,.rate=48000,.pitch=1,
                 .volume=1,.wb_count=1,.mix_id=0,.splitter_id=UINT32_MAX};
    int16_t pcm[64];for(unsigned i=0;i<64;i++)pcm[i]=8192;
    voice.wb[0]=(WaveBuf){.addr=(uintptr_t)pcm,.size=sizeof pcm,.end=64,.loop=1,.loop_count=-1};
    ChanState cs={0};ChanRes cr={0};cr.mix[0]=cr.prev[0]=cr.mix[1]=cr.prev[1]=1;
    float samples[24*N]={0},stereo[2*N];
    Mix mix={.used=1,.volume=1,.bufs=2,.dest=INT32_MAX,.dest_splitter=UINT32_MAX,.buf=samples};
    Effect fx={0};uint8_t effect[0xc0]={2,1,0};
    uint32_t send[32+CAP]={0},receive[32+CAP]={0};
    AudioAuxPool pools[2]={{(uintptr_t)send,sizeof send},{(uintptr_t)receive,sizeof receive}};
    effect[0x20]=0;effect[0x21]=1;effect[0x20+24]=0;effect[0x20+25]=1;
    u32(effect+0x20+48,2);u32(effect+0x20+56,CAP);
    u64(effect+0x20+64,(uintptr_t)send);u64(effect+0x20+80,(uintptr_t)receive);
    effect_update(&fx,effect,0x38564552);
    r.voice=&voice;r.chst=&cs;r.chres=&cr;r.mix=&mix;r.fx=&fx;r.pools=pools;r.pool_count=2;
    render_frame(&r,stereo);render_frame(&r,stereo); // warm the resampler with the effect bypassed
    for(unsigned i=0;i<2*N;i++)assert(stereo[i]==.25f);
    fx.enabled=1;
    render_frame(&r,stereo);
    // The guest sees signed PCM magnitudes, not normalized floats or PCM multiplied again.
    for(unsigned i=0;i<CAP;i++)assert((int32_t)send[32+i]==8192);
    // Simulate the game's auxiliary processing, including its 0.71875 limiter.
    for(unsigned i=0;i<CAP;i++) {
        int32_t value=(int32_t)send[32+i];
        if(value>23552)value=23552;if(value<-23552)value=-23552;
        memcpy(receive+32+i,&value,4);
    }
    render_frame(&r,stereo);
    for(unsigned i=0;i<2*N;i++)assert(stereo[i]==.25f);
    for(unsigned i=0;i<CAP;i++)receive[32+i]=23552;
    render_frame(&r,stereo);
    for(unsigned i=0;i<2*N;i++)assert(stereo[i]==.71875f);
    puts("PASS: decoded PCM -> auxiliary guest ring -> final SDL float sink keeps full audible amplitude");
}
#endif

int main(int argc, char **argv) {
    AudioSplitters s={0}; assert(audio_split_init(&s,2,3));
    uint8_t packet[512]; size_t size=routing_packet(packet);
    assert(audio_split_update(&s,packet,size));
    assert(s.splitter[0].count==2 && s.splitter[1].count==1);
    assert(audio_split_get(&s,0,1)->mix_id==0 && !audio_split_get(&s,2,0));
    // Every truncation rejects the packet without changing the active routes.
    for (size_t n=1; n<size; n++) assert(!audio_split_update(&s,packet,n));
    assert(s.splitter[0].ids[1]==1 && s.destination[0].volume[0]==.25f);
    uint8_t update[144]={0}; u32(update,0x48444e53); u32(update+8,1);
    destination(update+32,0,2,.5f,0); assert(audio_split_update(&s,update,sizeof update));
    float in[4]={4,4,4,4}, out[9]={0}; out[8]=123;
    audio_split_mix(&s.destination[0],out,2,4,in,1,1,true);
    for(int i=0;i<4;i++) assert(fabsf(out[i]-(1+.25f*i))<1e-6f);
    assert(out[8]==123); audio_split_finish(&s); assert(s.destination[0].previous[0]==.5f);
    u32(update+36,3); assert(!audio_split_update(&s,update,sizeof update));
    u32(update+36,0); f32(update+40,NAN); assert(!audio_split_update(&s,update,sizeof update));
    assert(s.destination[0].volume[0]==.5f);
    uint8_t empty[32]={0}; u32(empty,0x48444e53);
    assert(audio_split_update(&s,empty,sizeof empty)); assert(s.splitter[0].count==2);
    uint8_t clear[64]={0}; u32(clear,0x48444e53); u32(clear+4,1); state(clear+32,0,NULL,0);
    assert(audio_split_update(&s,clear,sizeof clear)); assert(!audio_split_get(&s,0,0));
    audio_split_free(&s);
#ifdef TEST_AUDIO_RENDERER
    mixer_test(packet,size);
    auxiliary_sink_level_test();
    delay_mixer_test();
#endif
    // Optional local captures exercise the real ABI without checking game data
    // into the repository. The first capture must include initial state.
    if (argc>1) {
        assert(audio_split_init(&s,96,384));
        for(int i=1;i<argc;i++) {
            FILE *f=fopen(argv[i],"rb"); assert(f); fseek(f,0,SEEK_END); long n=ftell(f); rewind(f);
            uint8_t *data=malloc(n); assert(data && fread(data,1,n,f)==(size_t)n); fclose(f);
            assert(n>=64); size_t at=64;
            for(int j=1;j<=5;j++) at+=audio_split_u32(data+j*4);
            size_t length=audio_split_u32(data+9*4); assert(at+length<=(size_t)n);
            assert(audio_split_update(&s,data+at,length)); free(data);
        }
        audio_split_free(&s);
        printf("PASS: %d captured renderer update packets\n",argc-1);
    }
    puts("PASS: partial routing updates, padded records, retained state, volume ramp, invalid ids/NaN/truncations and bounds");
}
