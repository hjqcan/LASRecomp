#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../runtime/services/audio_delay.h"

static void near(float a, float b) { assert(fabsf(a - b) < 1e-5f); }
static void update(AudioDelay *d, AudioDelayParam p, bool reset, bool new_mapping) {
    uint8_t data[56] = {0}; memcpy(data, &p, sizeof p); data[52] = 2;
    assert(audio_delay_update(d, data, reset, new_mapping));
}
static AudioDelayParam parameters(unsigned channels) {
    AudioDelayParam p = {0};
    p.channels = p.channels_max = channels;
    p.delay_max = 4000; p.delay_ms = 4; p.sample_rate = 1000 * 16384;
    p.in_gain = p.out_gain = 16384;
    for (unsigned c = 0; c < channels; c++) p.input[c] = p.output[c] = c;
    return p;
}

int main(void) {
    AudioDelay d = {0};
    AudioDelayParam p = parameters(1);
    p.feedback = 8192;
    update(&d, p, true, false);
    float b[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    audio_delay_process(&d, b, 1, 2, true); near(b[0], 0); near(b[1], 0);
    update(&d, p, false, false);  // receiving a packet must not clear the tail
    memset(b, 0, sizeof b);
    audio_delay_process(&d, b, 1, 5, true);
    near(b[0], 0); near(b[1], 1); near(b[4], .5f);

    // Bypass preserves the ring and copies the input, including exchanged channel indices.
    uint32_t cursor = d.cursor;
    b[0] = 7; audio_delay_process(&d, b, 1, 1, false); near(b[0], 7); assert(d.cursor == cursor);
    p = parameters(2); p.output[0] = 1; p.output[1] = 0;
    update(&d, p, true, false); b[0] = 3; b[1] = 9;
    audio_delay_process(&d, b, 2, 1, false); near(b[0], 9); near(b[1], 3);

    // Stereo cross-feedback moves the second echo to the other channel.
    p = parameters(2); p.delay_ms = 2; p.feedback = 8192; p.spread = 16384;
    update(&d, p, true, false); memset(b, 0, sizeof b); b[0] = 1;
    audio_delay_process(&d, b, 2, 4, true);
    near(b[1], 1); near(b[2], 0); near(b[6], .49f);
    near(b[3], .49f * .49f);

    // Quad feedback reaches the two adjacent channels, not the opposite one.
    p = parameters(4); p.delay_ms = 2; p.feedback = p.spread = 16384;
    update(&d, p, true, false);
    float quad[12] = {1}; audio_delay_process(&d, quad, 4, 3, true);
    near(quad[1], 1); near(quad[5], .49f); near(quad[8], .49f); near(quad[11], 0);

    // REV10 and earlier use FL,FR,BL,BR,FC,LFE; LFE feedback is independent.
    p = parameters(6); p.delay_ms = 2; p.feedback = 8192; p.spread = 16384;
    update(&d, p, true, false);
    assert(d.p.input[2] == 4 && d.p.input[3] == 5);
    float surround[18] = {0}; surround[15] = 1;
    audio_delay_process(&d, surround, 6, 3, true);
    near(surround[16], 1); near(surround[17], .5f);
    for (int i = 0; i < 15; i++) near(surround[i], 0);
    update(&d, p, true, true); assert(d.p.input[2] == 2 && d.p.input[3] == 3);

    // Low-pass state carries into the next sample before entering the delay line.
    p = parameters(1); p.delay_ms = 2; p.low_pass = 16384;
    update(&d, p, true, false); memset(b, 0, sizeof b); b[0] = 1;
    audio_delay_process(&d, b, 1, 4, true);
    near(b[1], .05f); near(b[2], .0475f); near(b[3], .045125f);

    // The game's 48 kHz / 400 ms mono descriptor: timed echoes, exact Q14 gains.
    p = parameters(1); p.sample_rate = 48000 * 16384; p.delay_ms = 400;
    p.in_gain = 9830; p.feedback = 6553; p.spread = 13107;
    update(&d, p, true, false);
    float *long_block = calloc(40000, sizeof(float)); assert(long_block);
    long_block[0] = 1; audio_delay_process(&d, long_block, 1, 40000, true);
    near(long_block[19199], 9830 / 16384.f);
    near(long_block[38398], (9830 / 16384.f) * (6553 / 16384.f));
    for (int i = 0; i < 40000; i++) if (i != 19199 && i != 38398) near(long_block[i], 0);
    free(long_block);

    // Bad buffer indices and descriptors must not write beyond the supplied buffers.
    p.output[0] = 2; update(&d, p, true, false); b[0] = 11; b[1] = 12345;
    audio_delay_process(&d, b, 1, 1, true); near(b[0], 11); near(b[1], 12345);
    uint8_t bad[56] = {0}; p.channels = 7; memcpy(bad, &p, sizeof p);
    assert(!audio_delay_update(&d, bad, false, false)); assert(!d.ready && !d.lines);
    p = parameters(1); p.delay_max = UINT32_MAX; memcpy(bad, &p, sizeof p);
    assert(!audio_delay_update(&d, bad, true, false));
    p = parameters(1); p.delay_ms = 0; update(&d, p, true, false); assert(d.length == 1);
    audio_delay_free(&d); audio_delay_free(&d);
    puts("PASS: delay timing, packet continuity, bypass, stereo/quad/5.1 routing, low-pass, game parameters and bounds");
}
