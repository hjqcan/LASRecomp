// Legacy audren effect 3. Delay equations and channel mapping follow Ryujinx
// DelayCommand/DelayState at dde9bb5c69d2e1a70df82af8accd3d01fb94b78d.
// Copyright (c) Ryujinx Team and Contributors. MIT license; see
// third_party/release-licenses/Ryujinx-LICENSE.txt.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t input[6], output[6];
    uint16_t channels_max, channels;
    uint32_t delay_max, delay_ms, sample_rate, in_gain, feedback, out_gain, dry_gain, spread, low_pass;
} AudioDelayParam;
_Static_assert(sizeof(AudioDelayParam) == 52, "audren delay parameter layout");

typedef struct {
    AudioDelayParam p;
    float *lines;
    uint32_t length, cursor;
    float low_pass_z[6];
    bool ready;
} AudioDelay;

static inline void audio_delay_free(AudioDelay *d) {
    free(d->lines);
    memset(d, 0, sizeof *d);
}

static inline bool audio_delay_channels(unsigned n) { return n == 1 || n == 2 || n == 4 || n == 6; }
static inline float audio_delay_q14(uint32_t n) { return n / 16384.f; }

static inline bool audio_delay_update(AudioDelay *d, const uint8_t *data, bool reset, bool new_mapping) {
    AudioDelayParam p;
    memcpy(&p, data, sizeof p);
    uint32_t rate = (p.sample_rate >> 14) / 1000;  // samples per millisecond
    // Reject malformed descriptors before allocating or addressing any channel.
    if (!audio_delay_channels(p.channels) || !audio_delay_channels(p.channels_max) ||
        p.channels > p.channels_max || !rate || rate > 192 || p.delay_max > 60000 || p.delay_ms > p.delay_max) {
        audio_delay_free(d);
        return false;
    }
    if (!new_mapping && p.channels == 6) {
        static const uint8_t order[6] = {0, 1, 4, 5, 2, 3};
        for (int c = 0; c < 6; c++) { p.input[c] = data[order[c]]; p.output[c] = data[6 + order[c]]; }
    }
    reset |= data[52] == 0 || !d->ready || p.channels != d->p.channels || p.sample_rate != d->p.sample_rate ||
             p.delay_max != d->p.delay_max || p.delay_ms != d->p.delay_ms;
    if (reset) {
        // The DSP delay line wraps at count - 1; zero/one-sample delays retain a single cell.
        uint32_t count = rate * p.delay_ms, length = count > 1 ? count - 1 : 1;
        float *lines = calloc((size_t)p.channels * length, sizeof(float));
        if (!lines) { audio_delay_free(d); return false; }
        audio_delay_free(d);
        d->lines = lines; d->length = length; d->ready = true;
    }
    d->p = p;
    return true;
}

static inline void audio_delay_process(AudioDelay *d, float *buffers, uint32_t buffer_count,
                                        uint32_t samples, bool enabled) {
    if (!d->ready) return;
    const AudioDelayParam *p = &d->p;
    unsigned ch = p->channels;
    for (unsigned c = 0; c < ch; c++)
        if (p->input[c] >= buffer_count || p->output[c] >= buffer_count) return;
    float gain = audio_delay_q14(p->feedback), spread = audio_delay_q14(p->spread);
    float base = (1 - spread) * gain * .98f, cross = spread * gain * .98f / (ch >= 4 ? 2 : 1);
    float feedback[6][6] = {{0}};
    for (unsigned c = 0; c < ch; c++) feedback[c][c] = base;
    if (ch == 1) feedback[0][0] = gain;
    else if (ch == 2) feedback[0][1] = feedback[1][0] = cross;
    else if (ch == 4) {
        feedback[0][1] = feedback[1][0] = feedback[0][2] = feedback[2][0] = cross;
        feedback[1][3] = feedback[3][1] = feedback[2][3] = feedback[3][2] = cross;
    } else {
        feedback[0][2] = feedback[2][0] = feedback[0][4] = feedback[4][0] = cross;
        feedback[1][2] = feedback[2][1] = feedback[1][5] = feedback[5][1] = cross;
        feedback[4][5] = feedback[5][4] = cross;
        feedback[3][3] = gain;  // LFE feeds only itself, without the .98 attenuation
    }
    float in_gain = audio_delay_q14(p->in_gain), dry_gain = audio_delay_q14(p->dry_gain);
    float out_gain = audio_delay_q14(p->out_gain), low_pass = .95f * audio_delay_q14(p->low_pass);
    for (uint32_t s = 0; s < samples; s++) {
        float input[6], delayed[6];
        // Read all channels first: input/output indices may alias or exchange channels.
        for (unsigned c = 0; c < ch; c++) {
            input[c] = buffers[(size_t)p->input[c] * samples + s] * 64;
            delayed[c] = d->lines[(size_t)c * d->length + d->cursor];
        }
        for (unsigned c = 0; c < ch; c++) {
            float output = input[c];
            if (enabled) {
                float next = input[c] * in_gain;
                for (unsigned k = 0; k < ch; k++) next += feedback[c][k] * delayed[k];
                d->low_pass_z[c] = low_pass * d->low_pass_z[c] + (1 - low_pass) * next;
                d->lines[(size_t)c * d->length + d->cursor] = d->low_pass_z[c];
                output = input[c] * dry_gain + delayed[c] * out_gain;
            }
            buffers[(size_t)p->output[c] * samples + s] = output / 64;
        }
        if (enabled && ++d->cursor >= d->length) d->cursor = 0;
    }
}
