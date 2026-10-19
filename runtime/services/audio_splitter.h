#ifndef LAS_AUDIO_SPLITTER_H
#define LAS_AUDIO_SPLITTER_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "audio_biquad.h"

// SNDH, variable SNDI records, then SNDD destinations: 0x70 through REV11,
// 0x90 with Q14 filters from REV12, 0xA8 with float filters from REV15.
// Updates contain only changed routing objects.
// Each destination supplies one input channel to up to 24 mix buffers.
typedef struct {
    uint32_t rate, count;
    uint32_t *ids;
} AudioSplitter;
typedef struct {
    int32_t mix_id;
    float volume[24], previous[24];
    bool used, rendered;
    bool filter_enabled[2];
    float coefficients[2][5], filter_state[2][4];
} AudioSplitDestination;
typedef struct {
    uint32_t count, destination_count;
    AudioSplitter *splitter;
    AudioSplitDestination *destination;
} AudioSplitters;

static uint32_t audio_split_u32(const uint8_t *p) {
    uint32_t v; memcpy(&v, p, sizeof v); return v;
}

static void audio_split_free(AudioSplitters *s) {
    for (uint32_t i = 0; i < s->count; i++) free(s->splitter[i].ids);
    free(s->splitter); free(s->destination);
    memset(s, 0, sizeof *s);
}

static bool audio_split_init(AudioSplitters *s, uint32_t count, uint32_t destinations) {
    if (count > 4096 || destinations > 4096) return false;
    *s = (AudioSplitters){0};
    s->splitter = calloc(count ? count : 1, sizeof *s->splitter);
    s->destination = calloc(destinations ? destinations : 1, sizeof *s->destination);
    if (!s->splitter || !s->destination) { audio_split_free(s); return false; }
    s->count = count; s->destination_count = destinations;
    return true;
}

static bool audio_split_update_revision(AudioSplitters *s, const uint8_t *p, size_t size, unsigned revision) {
    if (!size) return true;
    if (size < 32 || audio_split_u32(p) != 0x48444e53) return false;
    uint32_t ns = audio_split_u32(p + 4), nd = audio_split_u32(p + 8);
    if (ns > s->count || nd > s->destination_count) return false;
    if (!ns && !nd) return size == 32;
    // Validate all records and reserve replacement lists before changing state.
    AudioSplitter *pending = calloc(s->count ? s->count : 1, sizeof *pending);
    uint8_t *changed = calloc(s->count ? s->count : 1, 1);
    bool ok = pending && changed;
    size_t at = 32;
    for (uint32_t i = 0; ok && i < ns; i++) {
        if (size - at < 28 || audio_split_u32(p + at) != 0x49444e53) { ok = false; break; }
        uint32_t id = audio_split_u32(p + at + 4), count = audio_split_u32(p + at + 12);
        if (id >= s->count || changed[id] || count > s->destination_count || count > (size - at - 28) / 4) {
            ok = false; break;
        }
        changed[id] = 1;
        AudioSplitter *next = &pending[id];
        next->rate = audio_split_u32(p + at + 8); next->count = count;
        if (count) {
            next->ids = malloc(count * sizeof *next->ids);
            if (!next->ids) { ok = false; break; }
            for (uint32_t j = 0; j < count; j++) {
                next->ids[j] = audio_split_u32(p + at + 16 + j * 4);
                if (next->ids[j] >= s->destination_count) ok = false;
            }
        }
        at += 28 + count * 4; // includes twelve reserved bytes after the ids
    }
    size_t dest_at = at;
    size_t stride=revision>=15?0xa8:revision>=12?0x90:0x70;
    unsigned used_at=revision>=15?156:revision>=12?132:108;
    if (at > size || nd > (size - at) / stride) ok = false;
    for (uint32_t i = 0; ok && i < nd; i++, at += stride) {
        if (audio_split_u32(p + at) != 0x44444e53 || audio_split_u32(p + at + 4) >= s->destination_count) {
            ok = false; break;
        }
        for (unsigned j = 0; j < 24; j++) {
            float v; memcpy(&v, p + at + 8 + j * 4, 4);
            if (!isfinite(v)) ok = false;
        }
        if (revision>=12) for (unsigned f=0;f<2;f++) {
            const uint8_t *filter=p+at+108+f*(revision>=15?24:12);
            float coefficients[5];
            if (!audio_biquad_coefficients(coefficients, filter+(revision>=15?4:2), revision>=15)) ok=false;
        }
    }
    if (ok && ((at + 15) & ~(size_t)15) != size) ok = false;
    if (ok) {
        for (uint32_t i = 0; i < s->count; i++) if (changed[i]) {
            free(s->splitter[i].ids); s->splitter[i] = pending[i]; pending[i].ids = NULL;
        }
        for (uint32_t i = 0; i < nd; i++) {
            const uint8_t *q = p + dest_at + i * stride;
            AudioSplitDestination *d = &s->destination[audio_split_u32(q + 4)];
            memcpy(&d->mix_id, q + 104, 4);
            memcpy(d->volume, q + 8, sizeof d->volume);
            bool newly_used=!d->used && q[used_at];
            bool reset_volume=revision>=13 ? q[used_at+1]!=0 : newly_used;
            if (reset_volume) { memcpy(d->previous, d->volume, sizeof d->previous); d->rendered = false; }
            for (unsigned f=0;f<2;f++) {
                const uint8_t *filter=revision>=12?q+108+f*(revision>=15?24:12):NULL;
                bool enabled=filter && filter[0];
                if (newly_used || (enabled && !d->filter_enabled[f])) memset(d->filter_state[f],0,sizeof d->filter_state[f]);
                d->filter_enabled[f]=enabled;
                if (revision>=12) audio_biquad_coefficients(d->coefficients[f],filter+(revision>=15?4:2),revision>=15);
            }
            d->used = q[used_at] != 0;
        }
    }
    if (pending) for (uint32_t i = 0; i < s->count; i++) free(pending[i].ids);
    free(pending); free(changed);
    return ok;
}

static bool audio_split_update(AudioSplitters *s, const uint8_t *p, size_t size) {
    return audio_split_update_revision(s,p,size,11);
}

static AudioSplitDestination *audio_split_get(AudioSplitters *s, uint32_t id, uint32_t index) {
    if (id >= s->count || index >= s->splitter[id].count) return NULL;
    return &s->destination[s->splitter[id].ids[index]];
}

// Voice channels select destination indices channel, channel + channel_count,
// ...; submix channels select destination_index % source_buffer_count.
static void audio_split_mix(AudioSplitDestination *d, float *out, uint32_t buffers, uint32_t n,
                            const float *in, float before, float gain, bool ramp) {
    if (!d || !d->used || !out || !n) return;
    if (buffers > 24) buffers = 24;
    if (d->filter_enabled[0] || d->filter_enabled[1]) {
        // Filter once before fan-out. Advancing the same filter again for each
        // output channel would give those channels different samples and history.
        float start[24], step[24];
        for (uint32_t b=0;b<buffers;b++) {
            start[b]=ramp?before*d->previous[b]:gain*d->volume[b];
            step[b]=(gain*d->volume[b]-start[b])/n;
        }
        for (uint32_t i=0;i<n;i++) {
            float value=in[i];
            for (unsigned f=0;f<2;f++) if (d->filter_enabled[f]) {
                const float *k=d->coefficients[f];float *state=d->filter_state[f];
                float result=value*k[0]+state[0]*k[1]+state[1]*k[2]+state[2]*k[3]+state[3]*k[4];
                state[1]=state[0];state[0]=value;state[3]=state[2];state[2]=result;
                value=result;
            }
            for (uint32_t b=0;b<buffers;b++) out[b*n+i]+=value*(start[b]+step[b]*i);
        }
        if (ramp) d->rendered=true;
        return;
    }
    for (uint32_t b = 0; b < buffers; b++) {
        float g0 = ramp ? before * d->previous[b] : gain * d->volume[b];
        float g1 = gain * d->volume[b], step = (g1 - g0) / n;
        if (g0 == 0 && g1 == 0) continue;
        for (uint32_t i = 0; i < n; i++) out[b * n + i] += in[i] * (g0 + step * i);
    }
    if (ramp) d->rendered = true;
}

static void audio_split_finish(AudioSplitters *s) {
    for (uint32_t i = 0; i < s->destination_count; i++) {
        AudioSplitDestination *d = &s->destination[i];
        if (d->used && d->rendered) memcpy(d->previous, d->volume, sizeof d->previous);
        d->rendered = false;
    }
}

#endif
