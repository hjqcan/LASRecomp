#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Capacity calculation follows Ryujinx AudioRenderSystem.GetWorkBufferSize
// (MIT, Ryujinx Team and Contributors; third_party/ryujinx/LICENSE.txt).
// Reference: NextendoNetwork/Ryujinx-Nextendo, 2e6ff9ee8bc0072debd6584c3521586d1d4ab5ca.
// These are the reference renderer's storage sizes, not our host C layouts.
// A fixed oversized reservation can exhaust a game's independently sized sound heap.
static bool audio_work_reserve(uint64_t *size, uint64_t count, uint64_t stride, uint64_t align) {
    if (*size > UINT64_MAX - (align - 1) || (stride && count > UINT64_MAX / stride)) return false;
    uint64_t start = (*size + align - 1) & ~(align - 1), bytes = count * stride;
    if (bytes > UINT64_MAX - start) return false;
    *size = start + bytes;
    return true;
}

static bool audio_workbuffer_size(const void *data, size_t length, uint64_t *result) {
    uint32_t p[13];
    if (length < sizeof p) return false;
    memcpy(p, data, sizeof p);
    unsigned revision = (p[12] >> 24) - '0';
    if ((p[12] & 0xffffff) != 0x564552 || revision < 1 || revision > 16) return false;
    uint64_t samples = p[1], buffers = p[2], mixes = (uint64_t)p[3] + 1;
    uint64_t voices = p[4], sinks = p[5], effects = p[6], frames = p[7];
    uint64_t splitters = p[9], destinations = p[10], size = 0;
#define RESERVE(n, stride, align) do { if (!audio_work_reserve(&size, (n), (stride), (align))) return false; } while (0)
    RESERVE(samples, (6 + buffers) * 4, 0x10);
    RESERVE(sinks + p[3], 240 * (6 + buffers) * 4, 0x10);
    RESERVE((buffers + 0x3f) & ~UINT64_C(0x3f), 4, 0x40);
    RESERVE(voices, 0x218, 0x10); // VoiceInfo, including float biquad parameters
    RESERVE(voices, 4, 0x10);
    RESERVE(voices, 0xd0, 0x10);  // VoiceChannelResource
    RESERVE(voices, 0xd8, 0x10);  // VoiceState
    RESERVE(mixes, 0x940, 0x10);
    RESERVE(mixes, effects * 4, 0x10);
    RESERVE(mixes, 4, 0x10);
    if (revision >= 2) {
        uint64_t graph = 0, edges = 0;
        if (!audio_work_reserve(&edges, mixes, mixes, 1) ||
            !audio_work_reserve(&graph, edges, 4, 1) ||
            !audio_work_reserve(&graph, mixes, 12, 1) ||
            !audio_work_reserve(&edges, 0, 1, 0x40) ||
            !audio_work_reserve(&graph, edges, 3, 1) ||
            !audio_work_reserve(&graph, 0, 1, 0x10)) return false;
        RESERVE(graph, 1, 1);
    }
    RESERVE(effects + voices * 4, 0x20, 0x10); // MemoryPoolInfo
    if (revision >= 2) {
        RESERVE(splitters, 0x20, 0x10);
        RESERVE(destinations, revision >= 12 ? 0x128 : 0xe0, 0x10);
        if (revision >= 5) RESERVE(destinations, 4, 0x10);
    }
    if (revision >= 12 && splitters && destinations) RESERVE(destinations, 4 * 0x20, 0x10);
    RESERVE(voices, 0xd8, 0x10); // DSP voice state
    if (frames) {
        uint64_t entry = revision >= 5 ? 0x18 : 0x10;
        uint64_t frame = entry * (voices + effects + mixes + sinks + 100) + (revision >= 5 ? 0x30 : 0x18);
        uint64_t performance = 0;
        if (!audio_work_reserve(&performance, frames + 1, frame, 1) ||
            !audio_work_reserve(&performance, 1, 12, 1) ||
            !audio_work_reserve(&performance, 0, 1, 0x100)) return false;
        RESERVE(performance, 1, 1);
    }
    RESERVE(0, 1, 0x1000);
#undef RESERVE
    *result = size;
    return true;
}
