#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../runtime/services/audio_biquad.h"

int main(void) {
    AudioBiquad f={0}; uint8_t p[24]={0};
    p[6]=1; p[22]=1; p[23]=1;
    // y[n] = .5*x[n] + .5*y[n-1]. Its impulse response continues across packets.
    int16_t k[5]={8192,0,0,8192,0}; memcpy(p+12,k,sizeof k);
    audio_biquad_update(&f,p,true);
    float samples[9]={1,0,0,0,0,0,0,0,12345};
    audio_biquad_process(&f,samples,2,4,true);
    float expected=.5f;
    for(int i=0;i<4;i++,expected*=.5f) assert(fabsf(samples[4+i]-expected)<1e-6f);
    memset(samples,0,8*sizeof(float)); audio_biquad_update(&f,p,false);
    audio_biquad_process(&f,samples,2,4,true);
    for(int i=0;i<4;i++,expected*=.5f) assert(fabsf(samples[4+i]-expected)<1e-6f);
    assert(samples[8]==12345);
    // Disabled effects still copy their input to a distinct output.
    samples[0]=17; audio_biquad_process(&f,samples,2,4,false); assert(samples[4]==17);
    audio_biquad_update(&f,p,true); memset(samples,0,8*sizeof(float));
    audio_biquad_process(&f,samples,2,4,true); assert(samples[4]==0);
    // Signed coefficients, in-place filtering, bounds, and unused channels.
    k[0]=-8192; k[3]=0; memcpy(p+12,k,sizeof k); p[6]=0;
    audio_biquad_update(&f,p,true); samples[0]=4;
    audio_biquad_process(&f,samples,2,4,true); assert(samples[0]==-2);
    p[6]=2; audio_biquad_update(&f,p,true); samples[0]=10;
    audio_biquad_process(&f,samples,2,4,true); assert(samples[0]==10 && samples[8]==12345);
    p[22]=7; audio_biquad_update(&f,p,true); assert(f.channels==0);
    puts("PASS: effect biquad impulse response, packet continuity, bypass, reset, signed coefficients and bounds");
}
