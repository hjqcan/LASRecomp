// Exercise extractor source changes and decoder teardown with real FFmpeg allocations and synthetic PCM.
// Retained buffer references verify that frames and queued output release their payloads on reboot too.
// No game files or GPU setup are needed. The caller supplies an empty scratch directory.
#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include "../runtime/hle.h"

static unsigned packets;
static unsigned scalers, rgba_buffers;
static void *tracked_rgba;
static bool fail_packet;
static AVPacket *packet_alloc(void) {
    if (fail_packet) return NULL;
    AVPacket *p = av_packet_alloc();
    if (p) packets++;
    return p;
}
static void packet_free(AVPacket **p) {
    if (*p) { assert(packets); packets--; }
    av_packet_free(p);
}
static void scaler_free(struct SwsContext *s) { if (s) { assert(scalers); scalers--; } sws_freeContext(s); }
static void host_free(void *p) { if (p && p == tracked_rgba) { rgba_buffers--; tracked_rgba = NULL; } free(p); }
#define av_packet_alloc packet_alloc
#define av_packet_free packet_free
#define sws_freeContext scaler_free
#define free host_free
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#undef RT_ON_RESET
#define RT_ON_RESET(fn) static void fn(void)
#include "../runtime/hle/movie.c"

const char *g_romfs_dir;
Settings g_settings = {.movie_audio = true};
volatile int g_inject_plus;
uint64_t input_buttons_now(void) { return 0; }
uint64_t guest_sym(const char *name) { return 0; }
void guest_call(Cpu *c, uint64_t address) { assert(!"No decoder events in the fixture"); }

static void decoder_cleanup(bool reboot) {
    Decoder d = {0};
    pthread_mutex_init(&d.lock, NULL);
    d.ctx = avcodec_alloc_context3(NULL);
    d.frame = av_frame_alloc();
    d.frame->buf[0] = av_buffer_alloc(32);
    AVBufferRef *frame = av_buffer_ref(d.frame->buf[0]);
    d.pkt = packet_alloc();
    assert(d.ctx && d.frame && frame && d.pkt && av_new_packet(d.pkt, 32) == 0);
    AVBufferRef *packet = av_buffer_ref(d.pkt->buf), *pending[3];
    d.npending = 3; d.nready = 2;
    for (int i = 0; i < d.npending; i++) {
        d.pending[i] = av_frame_alloc();
        d.pending[i]->buf[0] = av_buffer_alloc(32);
        pending[i] = av_buffer_ref(d.pending[i]->buf[0]);
        assert(pending[i]);
    }
    d.sws = sws_getContext(16, 16, AV_PIX_FMT_NV12, 16, 16, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    assert(d.sws); scalers++;
    tracked_rgba = d.rgba = malloc(16 * 16 * 4); assert(d.rgba); rgba_buffers++;
    movie_track(1, &d, true);
    if (reboot) movie_reset();
    else {
        uint64_t object[2] = {0, (uintptr_t)&d};
        Cpu c = {0}; c.x[0] = (uintptr_t)object;
        dec_dtor(&c);
        c.x[0] = (uintptr_t)object; dec_dtor(&c); // the second virtual destructor is harmless
    }
    assert(!d.ctx && !d.frame && !d.pkt && !d.npending && !d.nready && !d.sws && !d.rgba);
    assert(!packets && !scalers && !rgba_buffers);
    assert(av_buffer_get_ref_count(frame) == 1 && av_buffer_get_ref_count(packet) == 1);
    av_buffer_unref(&frame); av_buffer_unref(&packet);
    for (int i = 0; i < 3; i++) {
        assert(!d.pending[i] && av_buffer_get_ref_count(pending[i]) == 1);
        av_buffer_unref(&pending[i]);
    }
    for (int i = 0; i < MAX_MOVIE_OBJS; i++) assert(!g_movie_objs[1][i]);
    pthread_mutex_destroy(&d.lock);
}

static void open_source(Cpu *c, uint64_t object[2], const char *path) {
    c->x[0] = (uintptr_t)object;
    c->x[1] = 0;
    c->x[2] = (uintptr_t)path;
    ex_set_data_source(c);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    g_romfs_dir = argv[1];
    char path[1024];
    snprintf(path, sizeof path, "%s/extractor.wav", g_romfs_dir);
    uint32_t wave[15] = {0x46464952, 52, 0x45564157, 0x20746d66, 16, 0x00010001, 48000, 96000,
                         0x00100002, 0x61746164, 16, 0x00020001, 0x00040003, 0x00060005, 0x00080007};
    FILE *file = fopen(path, "wbx");
    assert(file && fwrite(wave, 1, sizeof wave, file) == sizeof wave);
    fclose(file);

    Extractor e = {0};
    uint64_t object[2] = {0, (uintptr_t)&e};
    Cpu c = {0};
    open_source(&c, object, "rom:/extractor.wav");
    assert(!c.x[0] && packets == 1 && e.fmt && e.pkt);
    movie_track(0, &e, true);
    c.x[0] = (uintptr_t)object;
    ex_dtor(&c);
    assert(!e.fmt && !e.pkt && packets == 0);

    for (int i = 0; i < 32; i++) {
        open_source(&c, object, "rom:/extractor.wav");
        assert(!c.x[0] && packets == 1 && e.ntracks == 1 && !e.selected && !e.have_pkt && !e.eof);
        c.x[0] = (uintptr_t)object; c.x[1] = 0;
        ex_select_track(&c); ex_fill(&e);
        assert(e.have_pkt && e.pkt->size == 16 && e.pkt->data[0] == 1);
    }
    open_source(&c, object, "rom:/missing.wav");
    assert(c.x[0] && !e.fmt && !e.pkt && !e.ntracks && !e.selected && packets == 0);
    ex_fill(&e);
    assert(e.eof && !e.have_pkt);
    open_source(&c, object, NULL);
    assert(c.x[0] && !e.fmt && !e.pkt);
    fail_packet = true;
    open_source(&c, object, "rom:/extractor.wav");
    assert(c.x[0] && !e.fmt && !e.pkt && packets == 0);
    fail_packet = false;
    open_source(&c, object, "rom:/extractor.wav");
    assert(!c.x[0] && packets == 1);
    movie_track(0, &e, true);
    movie_reset();
    assert(!e.fmt && !e.pkt && packets == 0);
    assert(remove(path) == 0);
    for (int i = 0; i < 32; i++) { decoder_cleanup(false); decoder_cleanup(true); }
    movie_reset();
    puts("PASS: extractor source changes/failures and decoder destruction/reboot release packets, frames, queued output, scaler and RGBA");
}
