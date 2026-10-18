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
    // REV15 effect parameters carry unquantized float coefficients and move the
    // channel/status bytes past the wider coefficient block.
    uint8_t wide[40]={0}; wide[6]=1;wide[36]=wide[37]=1;
    float fk[5]={.123456f,0,0,.5f,0};memcpy(wide+16,fk,sizeof fk);
    assert(audio_biquad_update_format(&f,wide,true,true));
    memset(samples,0,8*sizeof(float));samples[0]=1;
    audio_biquad_process(&f,samples,2,4,true);expected=fk[0];
    for(int i=0;i<4;i++,expected*=.5f)assert(fabsf(samples[4+i]-expected)<1e-7f);
    AudioBiquad before=f;fk[0]=NAN;memcpy(wide+16,fk,sizeof fk);
    assert(!audio_biquad_update_format(&f,wide,false,true) && !memcmp(&f,&before,sizeof f));
    assert(samples[8]==12345);
    puts("PASS: effect biquad impulse response, packet continuity, bypass, reset, signed coefficients and bounds");
}
