// Audio-renderer effect 6: six channel indices, Q14 or REV15 float coefficients.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

typedef struct {
    uint8_t input[6], output[6], channels;
    float coefficients[5], state[6][2];
} AudioBiquad;

static inline bool audio_biquad_coefficients(float out[5], const uint8_t *data, bool floating) {
    for (unsigned i=0;i<5;i++) {
        if (floating) memcpy(out+i, data+i*4, 4);
        else { int16_t value; memcpy(&value, data+i*2, 2); out[i]=value/16384.f; }
        if (!isfinite(out[i])) return false;
    }
    return true;
}

static inline bool audio_biquad_update_format(AudioBiquad *filter, const uint8_t *data, bool reset, bool floating) {
    float coefficients[5];
    if (!audio_biquad_coefficients(coefficients, data+(floating?16:12), floating)) return false;
    unsigned channels=floating?36:22, status=channels+1;
    if (reset || data[status] == 0) memset(filter->state, 0, sizeof filter->state);
    memcpy(filter->input, data, 6); memcpy(filter->output, data + 6, 6);
    filter->channels = data[channels] <= 6 ? data[channels] : 0;
    memcpy(filter->coefficients, coefficients, sizeof coefficients);
    return true;
}

static inline void audio_biquad_update(AudioBiquad *filter, const uint8_t *data, bool reset) {
    (void)audio_biquad_update_format(filter, data, reset, false);
}

static inline void audio_biquad_process(AudioBiquad *filter, float *buffers,
                                        uint32_t buffer_count, uint32_t samples, bool enabled) {
    for (unsigned c=0;c<filter->channels;c++) {
        if (filter->input[c] >= buffer_count || filter->output[c] >= buffer_count) continue;
        float *input=buffers + filter->input[c]*samples, *output=buffers + filter->output[c]*samples;
        if (!enabled) {
            if (input != output) memcpy(output,input,samples*sizeof(float));
            continue;
        }
        const float *k=filter->coefficients;
        float z1=filter->state[c][0], z2=filter->state[c][1];
        for (uint32_t i=0;i<samples;i++) {
            float x=input[i], y=k[0]*x+z1;
            z1=k[1]*x+k[3]*y+z2; z2=k[2]*x+k[4]*y;
            output[i]=y;
        }
        filter->state[c][0]=z1; filter->state[c][1]=z2;
    }
}
