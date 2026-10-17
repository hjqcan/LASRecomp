// audren effect 5, following Ryujinx Reverb3dState/Command and DelayLine3d at
// dde9bb5c69d2e1a70df82af8accd3d01fb94b78d. Copyright (c) Ryujinx Team and
// Contributors. MIT license: third_party/release-licenses/Ryujinx-LICENSE.txt.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    uint8_t input[6], output[6];
    uint16_t channels_max, channels;
    uint32_t reserved, sample_rate;
    float room_hf, hf_reference, decay_time, hf_decay_ratio, room_gain, reflections_gain,
          reverb_gain, diffusion, reflection_delay, reverb_delay, density, dry_gain;
} AudioReverb3dParam;
_Static_assert(sizeof(AudioReverb3dParam) == 72, "audren reverb3d parameter layout");

typedef struct {
    float *data;
    uint32_t max, count, read, write;
    float decay;
} AudioReverb3dLine;

typedef struct {
    AudioReverb3dParam p;
    AudioReverb3dLine fdn[4], decay1[4], decay2[4], pre, center;
    float direct[4], current[4], output[4], previous[4];
    float early_gain, late_gain, pre_previous, pre_target, pre_value;
    uint32_t reflection, early[20];
    bool ready;
} AudioReverb3d;

static const float ar3_fdn_min[4] = {5, 6, 13, 14};
static const float ar3_fdn_max[4] = {45.704f, 82.782f, 149.94f, 271.58f};
static const float ar3_decay1[4] = {17, 13, 9, 7}, ar3_decay2[4] = {19, 11, 10, 6};
static const float ar3_early_time[20] = {.017136f,.059154f,.16173f,.39019f,.42526f,.45541f,.68974f,.74591f,.83384f,.8595f,
    0,.075024f,.16879f,.2999f,.33744f,.3719f,.59901f,.71674f,.81786f,.85166f};
static const float ar3_early_gain[20] = {.67096f,.61027f,1,.35680f,.68361f,.65978f,.51939f,.24712f,.45945f,.45021f,
    .64196f,.54879f,.92925f,.38270f,.72867f,.69794f,.5464f,.24563f,.45214f,.44042f};

static inline uint32_t ar3_samples(uint32_t rate, float ms) { return (uint32_t)nearbyintf(rate * ms); }
static inline void ar3_line_set(AudioReverb3dLine *l, uint32_t count) {
    if (count <= l->max) { l->count = count; l->write = (l->read + count) % (l->max + 1); }
}
static inline bool ar3_line_init(AudioReverb3dLine *l, uint32_t rate, float ms) {
    l->max = ar3_samples(rate, ms);
    l->data = calloc(l->max + 1, sizeof(float));
    ar3_line_set(l, l->max);
    return l->data != NULL;
}
static inline float ar3_line_update(AudioReverb3dLine *l, float value) {
    l->data[l->write++] = value;
    float out = l->data[l->read++];
    if (l->write >= l->max) l->write = 0;
    if (l->read >= l->max) l->read = 0;
    return out;
}
static inline float ar3_tap(AudioReverb3dLine *l, uint32_t count) {
    int index = (int)l->write - (int)count - 1;
    if (index < 0) index += (int)l->max + 1;
    return l->data[index];
}
static inline float ar3_decay_update(AudioReverb3dLine *l, float value) {
    float filtered = value - l->decay * l->data[l->read];
    return ar3_line_update(l, filtered) + filtered * l->decay;
}
static inline float ar3_pow10(float x) { return x >= 0 ? 1 : x <= -5.3f ? 0 : powf(10, x); }
static inline float ar3_radians(float degrees) { return degrees * 3.14159265358979323846f / 180.f; }

static inline void audio_reverb3d_free(AudioReverb3d *r) {
    for (int i = 0; i < 4; i++) { free(r->fdn[i].data); free(r->decay1[i].data); free(r->decay2[i].data); }
    free(r->pre.data); free(r->center.data);
    memset(r, 0, sizeof *r);
}

static inline bool audio_reverb3d_update(AudioReverb3d *r, const uint8_t *data, bool reset, bool new_mapping) {
    AudioReverb3dParam p;
    memcpy(&p, data, sizeof p);
    unsigned ch = p.channels;
    bool valid = (ch == 1 || ch == 2 || ch == 4 || ch == 6) && ch <= p.channels_max &&
        (p.channels_max == 1 || p.channels_max == 2 || p.channels_max == 4 || p.channels_max == 6) &&
        p.sample_rate >= 1000 && p.sample_rate <= 192000;
    for (int i = 0; i < 12; i++) { float f; memcpy(&f, data + 24 + i * 4, 4); valid &= isfinite(f); }
    valid &= p.decay_time > 0 && p.hf_decay_ratio > 0 && p.hf_reference > 0 && p.hf_reference <= p.sample_rate / 2 &&
        p.diffusion >= 0 && p.diffusion <= 100 && p.density >= 0 && p.density <= 100 &&
        p.reflection_delay >= 0 && p.reverb_delay >= 0 && p.reflection_delay + p.reverb_delay <= .4f;
    if (!valid) { audio_reverb3d_free(r); return false; }
    // This algorithm uses the legacy FL,FR,BL,BR,FC,LFE order internally.
    if (new_mapping && ch == 6) {
        static const uint8_t order[6] = {0,1,4,5,2,3};
        for (int c = 0; c < 6; c++) { p.input[c] = data[order[c]]; p.output[c] = data[6 + order[c]]; }
    }
    reset |= data[72] == 0 || !r->ready || p.sample_rate != r->p.sample_rate || ch != r->p.channels;
    if (!reset && !memcmp(&p, &r->p, sizeof p)) return true;
    uint32_t rate = p.sample_rate / 1000;
    if (reset) {
        audio_reverb3d_free(r);
        bool allocated = ar3_line_init(&r->pre, rate, 400) && ar3_line_init(&r->center, rate, 5);
        for (int i = 0; i < 4 && allocated; i++)
            allocated = ar3_line_init(&r->fdn[i], rate, ar3_fdn_max[i]) &&
                ar3_line_init(&r->decay1[i], rate, ar3_decay1[i]) && ar3_line_init(&r->decay2[i], rate, ar3_decay2[i]);
        if (!allocated) { audio_reverb3d_free(r); return false; }
        r->ready = true;
    }
    r->p = p;
    memset(r->previous, 0, sizeof r->previous); r->pre_value = 0;
    r->early_gain = ar3_pow10(fminf(p.room_gain + p.reflections_gain, 5000) / 2000);
    r->late_gain = ar3_pow10(fminf(p.room_gain + p.reverb_gain, 5000) / 2000);
    float hf = ar3_pow10(p.room_hf / 2000);
    r->pre_previous = 0; r->pre_target = 1;
    if (hf < 1) {
        float a = 1 - hf, b = 2 - 2 * hf * cosf(ar3_radians(256 * p.hf_reference / p.sample_rate));
        float c = sqrtf(fmaxf(0, b * b - 4 * a * a));
        r->pre_previous = (b - c) / (2 * a); r->pre_target = 1 - r->pre_previous;
    }
    r->reflection = ar3_samples(rate, 1000 * (p.reflection_delay + p.reverb_delay));
    if (r->reflection > r->pre.max) r->reflection = r->pre.max;
    float decay = .6f * p.diffusion * .01f;
    for (int i = 0; i < 4; i++) {
        ar3_line_set(&r->fdn[i], ar3_samples(rate, ar3_fdn_min[i] + p.density / 100 * (ar3_fdn_max[i] - ar3_fdn_min[i])));
        uint32_t count = r->fdn[i].count + r->decay1[i].count + r->decay2[i].count;
        float a = -60.f * count / (p.decay_time * p.sample_rate), b = a / p.hf_decay_ratio;
        float angle = ar3_radians(128 * .5f * p.hf_reference / p.sample_rate);
        float c = cosf(angle) / sinf(angle), d = ar3_pow10((b - a) / 40);
        float e = ar3_pow10((b + a) / 40) * .7071f;
        r->direct[i] = e * (d * c + 1) / (c + d);
        r->current[i] = e * (1 - d * c) / (c + d);
        r->output[i] = (c - d) / (c + d);
        if (!isfinite(r->direct[i]) || !isfinite(r->current[i]) || !isfinite(r->output[i])) {
            audio_reverb3d_free(r); return false;
        }
        r->decay1[i].decay = decay; r->decay2[i].decay = decay * -.9f;
    }
    for (int i = 0; i < 20; i++) {
        uint32_t count = ar3_samples(rate, p.reflection_delay * 1000 + ar3_early_time[i] * 1000 * (p.reverb_delay * .9998f + .02f));
        r->early[i] = count < r->pre.max ? count : r->pre.max;
    }
    return true;
}

static inline void audio_reverb3d_process(AudioReverb3d *r, float *buffers, uint32_t buffer_count,
                                          uint32_t samples, bool enabled) {
    if (!r->ready) return;
    unsigned ch = r->p.channels;
    for (unsigned c = 0; c < ch; c++) if (r->p.input[c] >= buffer_count || r->p.output[c] >= buffer_count) return;
    static const uint8_t early_stereo[20] = {0,0,0,1,1,1,1,0,0,0,1,1,1,0,0,0,0,1,1,1};
    static const uint8_t early_quad[20] = {0,0,0,1,1,1,1,2,2,2,1,1,1,0,0,0,0,3,3,3};
    for (uint32_t s = 0; s < samples; s++) {
        float input[6], out[6] = {0}, fo[4], fb[4], values[4], pre = 0;
        for (unsigned c = 0; c < ch; c++) { input[c] = buffers[(size_t)r->p.input[c] * samples + s]; pre += input[c]; }
        if (!enabled) {
            for (unsigned c = 0; c < ch; c++) buffers[(size_t)r->p.output[c] * samples + s] = input[c];
            continue;
        }
        float late = ar3_tap(&r->pre, r->reflection);
        for (int i = 0; i < 20; i++) {
            unsigned c = ch == 1 ? 0 : ch == 2 ? early_stereo[i] : ch == 6 && i == 0 ? 4 : early_quad[i];
            float v = ar3_tap(&r->pre, r->early[i]) * ar3_early_gain[i];
            out[c] += v; if (ch == 6) out[5] += v;
        }
        for (unsigned c = 0; c < ch; c++) out[c] *= r->early_gain;
        r->pre_value = pre * r->pre_target + r->pre_value * r->pre_previous;
        ar3_line_update(&r->pre, r->pre_value);
        for (int i = 0; i < 4; i++) {
            float fdn = r->fdn[i].data[r->fdn[i].read];
            fo[i] = fdn * r->direct[i] + r->previous[i];
            r->previous[i] = fdn * r->current[i] + fo[i] * r->output[i];
        }
        fb[0] = fo[2] + fo[1]; fb[1] = -fo[0] - fo[3]; fb[2] = fo[0] - fo[3]; fb[3] = fo[1] - fo[2];
        for (int i = 0; i < 4; i++) {
            values[i] = ar3_decay_update(&r->decay2[i], ar3_decay_update(&r->decay1[i], late * r->late_gain + fb[i]));
            ar3_line_update(&r->fdn[i], values[i]);
        }
        for (unsigned c = 0; c < ch; c++) {
            float v = out[c] + input[c] * r->p.dry_gain;
            if (ch == 6 && c == 4) {
                v += ar3_line_update(&r->center, (values[2] - values[3]) * .5f);
                buffers[(size_t)r->p.output[c] * samples + s] += v;
            } else {
                v += values[c == 5 ? 3 : c]; if (ch == 1) v += values[1];
                buffers[(size_t)r->p.output[c] * samples + s] = v;
            }
        }
    }
}
