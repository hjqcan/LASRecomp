// Legacy audio-renderer effect 6: six channel indices and signed Q14 coefficients.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    uint8_t input[6], output[6], channels;
    float coefficients[5], state[6][2];
} AudioBiquad;

static inline void audio_biquad_update(AudioBiquad *filter, const uint8_t *data, bool reset) {
    if (reset || data[23] == 0) memset(filter->state, 0, sizeof filter->state);
    memcpy(filter->input, data, 6); memcpy(filter->output, data + 6, 6);
    filter->channels = data[22] <= 6 ? data[22] : 0;
    for (int i=0;i<5;i++) {
        int16_t value; memcpy(&value, data + 12 + i*2, 2);
        filter->coefficients[i] = value / 16384.f;
    }
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
