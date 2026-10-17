#include <assert.h>
#include <stdio.h>
#include "../runtime/services/audio_reverb3d.h"
#include "audio_reverb3d_reference.inc"

static AudioReverb3dParam parameters(unsigned channels) {
    AudioReverb3dParam p = {.channels=channels,.channels_max=channels,.sample_rate=48000,.room_hf=-2500,
        .hf_reference=2000,.decay_time=3,.hf_decay_ratio=1,.room_gain=-1600,.reflections_gain=-800,
        .reverb_gain=-1000,.diffusion=100,.reflection_delay=.05f,.density=100,.dry_gain=.125f};
    for(unsigned c=0;c<channels;c++){p.input[c]=c;p.output[c]=channels+c;}
    return p;
}
static bool update(AudioReverb3d *r, AudioReverb3dParam p, uint8_t status, bool modern) {
    uint8_t bytes[76]={0}; memcpy(bytes,&p,sizeof p); bytes[72]=status;
    return audio_reverb3d_update(r,bytes,false,modern);
}
static void impulse(unsigned channels, bool modern, const char *reference_dir) {
    AudioReverb3d r={0}; AudioReverb3dParam p=parameters(channels);
    FILE *reference=NULL;
    if(reference_dir) {
        // Compare REV11 remapping only for 5.1. The reference's fixed six-element
        // index array also remaps inactive quad channels; the ABI change is 5.1-only.
        char path[1024]; snprintf(path,sizeof path,"%s/rv3-%u-%u.bin",reference_dir,channels,channels==6&&modern);
        reference=fopen(path,"rb"); assert(reference);
    }
    float buffers[12*240], max_difference=0, max_value=0;
    double tail_energy=0;
    for(int block=0;block<200;block++) {
        memset(buffers,0,sizeof buffers);
        if(block==0||block==100) for(unsigned c=0;c<channels;c++)buffers[c*240]=(c+1)*.125f;
        if(block==100){p.density=30;p.diffusion=40;p.hf_decay_ratio=.5f;}
        assert(update(&r,p,block==0?0:block==100?1:2,modern));
        audio_reverb3d_process(&r,buffers,channels*2,240,true);
        for(unsigned c=0;c<channels;c++)for(int s=0;s<240;s++) {
            float v=buffers[(channels+c)*240+s]; assert(isfinite(v));
            if(fabsf(v)>max_value)max_value=fabsf(v);
            if(block>0)tail_energy+=(double)v*v;
            if(block==0&&s==0)assert(fabsf(v-(c+1)*.015625f)<1e-6f);
            if(reference) {
                float expected; assert(fread(&expected,sizeof expected,1,reference)==1);
                float delta=fabsf(v-expected); if(delta>max_difference)max_difference=delta;
                if(delta>2e-6f+fabsf(expected)*.001f) {
                    fprintf(stderr,"reverb3d mismatch ch%u mapping%d block%d channel%u sample%d: %g != %g\n",
                        channels,modern,block,c,s,v,expected); abort();
                }
            }
        }
        for(size_t i=0;i<sizeof rv3_reference/sizeof *rv3_reference;i++) {
            if(rv3_reference[i].channels!=channels || rv3_reference[i].modern!=(channels==6&&modern) ||
               rv3_reference[i].sample/240!=(unsigned)block)continue;
            float actual=buffers[(channels+rv3_reference[i].channel)*240+rv3_reference[i].sample%240];
            assert(fabsf(actual-rv3_reference[i].expected)<1e-7f+fabsf(rv3_reference[i].expected)*1e-4f);
        }
    }
    assert(tail_energy>1e-8 && tail_energy<100 && max_value<2);
    if(reference){assert(fgetc(reference)==EOF);fclose(reference);}
    printf("PASS: reverb3d %u channels mapping %u, energy %.9g",channels,modern,tail_energy);
    if(reference)printf(", reference error %.9g",max_difference);
    putchar('\n');
    // Bypassing does not advance the delay lines; exchanged buffers retain all inputs.
    uint32_t position=r.pre.read; memset(buffers,0,sizeof buffers);
    for(unsigned c=0;c<channels;c++)buffers[c*4]=(float)c+1;
    audio_reverb3d_process(&r,buffers,channels*2,4,false); assert(r.pre.read==position);
    for(unsigned c=0;c<channels;c++)assert(buffers[(channels+c)*4]==(float)c+1);
    audio_reverb3d_free(&r); assert(!r.ready && !r.pre.data); audio_reverb3d_free(&r);
}

int main(int argc,char **argv) {
    const unsigned channels[]={1,2,4,6};
    for(unsigned i=0;i<4;i++)for(int modern=0;modern<2;modern++)impulse(channels[i],modern,argc>1?argv[1]:NULL);
    AudioReverb3d r={0}; AudioReverb3dParam p=parameters(2);
    assert(update(&r,p,0,false)); float b[9]={1,2,3,4,5,6,7,8,12345};
    p.output[1]=4; assert(update(&r,p,2,false)); audio_reverb3d_process(&r,b,4,2,true);
    assert(b[0]==1 && b[8]==12345);
    p=parameters(2); p.decay_time=0; assert(!update(&r,p,0,false)); assert(!r.ready);
    p=parameters(2); p.room_hf=NAN; assert(!update(&r,p,0,false));
    p=parameters(2); p.decay_time=1e-38f; assert(!update(&r,p,0,false));
    p=parameters(2); p.reflection_delay=1; assert(!update(&r,p,0,false));
    p=parameters(2); p.channels=7; assert(!update(&r,p,0,false));
    p=parameters(2); p.input[0]=0;p.input[1]=1;p.output[0]=1;p.output[1]=0;
    assert(update(&r,p,0,false)); b[0]=3;b[1]=9;audio_reverb3d_process(&r,b,2,1,false);
    assert(b[0]==9 && b[1]==3);audio_reverb3d_free(&r);
    puts("PASS: reverb3d tails, updates, bypass, reset, bounds and invalid descriptors");
}
