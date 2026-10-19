// audren auxiliary-buffer effect (2). ABI/ring protocol follows Ryujinx
// AuxiliaryBufferParameter/Header/Command at dde9bb5c69d2e1a70df82af8accd3d01fb94b78d.
// Copyright (c) Ryujinx Team and Contributors. MIT license; see
// third_party/release-licenses/Ryujinx-LICENSE.txt.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <limits.h>

typedef struct { uint64_t address, size; } AudioAuxPool;
static inline void audio_aux_pool_update(AudioAuxPool *pool, const uint8_t *data) {
    uint32_t state; memcpy(&state, data + 16, 4);
    if (state == 4) { memcpy(&pool->address, data, 8); memcpy(&pool->size, data + 8, 8); }
    else if (state == 0 || state == 2 || state == 3) memset(pool, 0, sizeof *pool);
}
typedef struct {
    uint8_t input[24], output[24];
    uint32_t channels, capacity;
    uint64_t send, receive;
    bool valid;
} AudioAux;

static inline void audio_aux_update(AudioAux *a, const uint8_t *data) {
    memcpy(a->input, data, 24); memcpy(a->output, data + 24, 24);
    memcpy(&a->channels, data + 48, 4); memcpy(&a->capacity, data + 56, 4);
    memcpy(&a->send, data + 64, 8); memcpy(&a->receive, data + 80, 8);
    a->valid = a->channels && a->channels <= 24 && a->capacity && a->send && a->receive &&
        !(a->send & 3) && !(a->receive & 3);
}

static inline bool audio_aux_mapped(uint64_t address, uint64_t size, const AudioAuxPool *pools, uint32_t count) {
    if (!address || size > UINT64_MAX - address) return false;
    for (uint32_t i = 0; i < count; i++)
        if (address >= pools[i].address && address - pools[i].address <= pools[i].size &&
            size <= pools[i].size - (address - pools[i].address)) return true;
    return false;
}
static inline uint32_t audio_aux_load(uint8_t *header, unsigned offset) {
    return __atomic_load_n((uint32_t *)(header + 0x40 + offset), __ATOMIC_ACQUIRE);
}
static inline void audio_aux_store(uint8_t *header, unsigned offset, uint32_t value) {
    __atomic_store_n((uint32_t *)(header + 0x40 + offset), value, __ATOMIC_RELEASE);
}
static inline int32_t audio_aux_pcm(float sample) {
    // The mixer holds PCM magnitudes in floats; only the final device sink
    // normalizes by 32768. Guest auxiliary rings use the same units as int32.
    float value = sample;
    if (!isfinite(value)) return 0;
    if (value >= 2147483648.f) return INT32_MAX;
    if (value <= -2147483648.f) return INT32_MIN;
    return (int32_t)value;
}

static inline bool audio_aux_process(AudioAux *a, float *buffers, uint32_t buffer_count, uint32_t samples,
                                      bool enabled, const AudioAuxPool *pools, uint32_t pool_count) {
    if (!a->valid) return false;
    for (uint32_t c = 0; c < a->channels; c++)
        if (a->input[c] >= buffer_count || a->output[c] >= buffer_count) return false;
    uint64_t size = 0x80ull + (uint64_t)a->capacity * 4, total = (uint64_t)a->channels * samples;
    bool mapped = audio_aux_mapped(a->send, size, pools, pool_count) && audio_aux_mapped(a->receive, size, pools, pool_count);
    uint8_t *send = (uint8_t *)(uintptr_t)a->send, *receive = (uint8_t *)(uintptr_t)a->receive;
    if (!enabled) {
        if (mapped) {
            // CPU headers and the lost-sample counters are owned separately.
            audio_aux_store(send, 0, 0); audio_aux_store(send, 4, 0); audio_aux_store(send, 12, 0);
            audio_aux_store(receive, 0, 0); audio_aux_store(receive, 4, 0); audio_aux_store(receive, 12, 0);
        }
        for (uint32_t s = 0; s < samples; s++) {
            float input[24];
            for (uint32_t c = 0; c < a->channels; c++) input[c] = buffers[(size_t)a->input[c] * samples + s];
            for (uint32_t c = 0; c < a->channels; c++) buffers[(size_t)a->output[c] * samples + s] = input[c];
        }
        return true;
    }
    if (!mapped || total > a->capacity) return false;
    uint32_t write = audio_aux_load(send, 4), read = audio_aux_load(receive, 0);
    if (write >= a->capacity || read >= a->capacity) return false;
    // All channels are one frame in the planar ring. Publish its new offsets once
    // the complete frame is transferred, so the game never sees a partial frame.
    for (uint32_t c = 0; c < a->channels; c++) {
        uint32_t at = (uint32_t)((write + (uint64_t)c * samples) % a->capacity);
        float *input = buffers + (size_t)a->input[c] * samples;
        for (uint32_t s = 0; s < samples; s++) {
            int32_t value = audio_aux_pcm(input[s]);
            memcpy(send + 0x80 + (size_t)at * 4, &value, 4);
            input[s] = (float)value;
            if (++at == a->capacity) at = 0;
        }
    }
    for (uint32_t c = 0; c < a->channels; c++) {
        uint32_t at = (uint32_t)((read + (uint64_t)c * samples) % a->capacity);
        float *output = buffers + (size_t)a->output[c] * samples;
        for (uint32_t s = 0; s < samples; s++) {
            int32_t value; memcpy(&value, receive + 0x80 + (size_t)at * 4, 4);
            output[s] = (float)value;
            if (++at == a->capacity) at = 0;
        }
    }
    audio_aux_store(send, 4, (uint32_t)((write + total) % a->capacity));
    audio_aux_store(receive, 0, (uint32_t)((read + total) % a->capacity));
    return true;
}
