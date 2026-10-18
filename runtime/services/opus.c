// hwopus: packet decoding on the host through libopus. No console DSP firmware is used.
// Wire layouts: switchbrew/libnx hwopus.c and https://switchbrew.org/wiki/Audio_services#hwopus.
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <opus/opus.h>
#include <opus/opus_multistream.h>

#include "../hle.h"
#include "../ipc.h"

// Use ordinary runtime argument/resource errors for rejected requests. Hardware-specific
// codec error numbering and the DSP's exact workspace size are not modelled here.
#define OPUS_BAD_ARGUMENT 0xE801u
#define OPUS_NO_MEMORY 0xD001u
#define OPUS_UNSUPPORTED 0xF601u

typedef struct {
    int rate, channels, streams, coupled, large;
    bool multi;
    uint8_t mapping[256];
} OpusParams;

typedef struct OpusSession {
    Service *owner;
    struct OpusSession *next;
    pthread_mutex_t lock;
    OpusParams params;
    void *decoder, *trial;
    int16_t *pcm;
    int decoder_size, max_samples, refs;
} OpusSession;

static pthread_mutex_t opus_list_lock = PTHREAD_MUTEX_INITIALIZER;
static OpusSession *opus_sessions;

static uint32_t read_u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t read_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static bool opus_span(const IpcBuf *b, uint64_t bytes) {
    return b && b->addr && b->size >= bytes && b->addr <= UINTPTR_MAX - bytes;
}

static bool opus_params(IpcReq *req, OpusParams *p) {
    memset(p, 0, sizeof *p);
    p->multi = req->cmd == 2 || req->cmd == 3 || req->cmd == 6 || req->cmd == 7 || req->cmd == 9;
    bool extended = req->cmd >= 4;
    const uint8_t *data = req->data;
    if (p->multi) {
        IpcBuf *b = in_buf(req, 0);
        if (!opus_span(b, extended ? 0x118 : 0x110)) return false;
        data = (const uint8_t *)(uintptr_t)b->addr;
    } else if (!data || req->data_size < (extended ? 16u : 8u)) return false;
    p->rate = (int32_t)read_u32(data);
    p->channels = (int32_t)read_u32(data + 4);
    if (p->rate != 8000 && p->rate != 12000 && p->rate != 16000 && p->rate != 24000 && p->rate != 48000) return false;
    if (p->channels < 1 || p->channels > (p->multi ? 255 : 2)) return false;
    if (p->multi) {
        p->streams = (int32_t)read_u32(data + 8);
        p->coupled = (int32_t)read_u32(data + 12);
        memcpy(p->mapping, data + 16, 256);
        if (p->streams < 1 || p->streams > 255 || p->coupled < 0 || p->coupled > p->streams ||
            p->streams + p->coupled > 255) return false;
        for (int i = 0; i < p->channels; ++i)
            if (p->mapping[i] != 255 && p->mapping[i] >= p->streams + p->coupled) return false;
        p->large = extended ? (int32_t)read_u32(data + 0x110) : 0;
    } else p->large = extended ? (int32_t)read_u32(data + 8) : 0;
    return p->large == 0 || p->large == 1;
}

static int opus_state_size(const OpusParams *p) {
    return p->multi ? opus_multistream_decoder_get_size(p->streams, p->coupled) : opus_decoder_get_size(p->channels);
}
static uint32_t opus_work_size(const OpusParams *p) {
    int n = opus_state_size(p);
    if (n <= 0) return 0;
    // A conservative host workspace contract, shared by query and open. The opaque
    // transfer memory remains guest-owned; decoder state and scratch output live on the host.
    uint64_t bytes = 2ull * n + (uint64_t)p->rate * (p->large ? 120 : 40) / 1000 * p->channels * 2 + 4096;
    return bytes <= UINT32_MAX - 63 ? (uint32_t)((bytes + 63) & ~63ull) : 0;
}

static void opus_release(OpusSession *d) {
    if (__atomic_sub_fetch(&d->refs, 1, __ATOMIC_ACQ_REL)) return;
    pthread_mutex_destroy(&d->lock);
    free(d->pcm); free(d->trial); free(d->decoder); free(d);
}

static void opus_close(Service *s) {
    pthread_mutex_lock(&opus_list_lock);
    OpusSession *d = s->state;
    if (d) {
        OpusSession **at = &opus_sessions;
        while (*at && *at != d) at = &(*at)->next;
        if (*at) *at = d->next;
        s->state = NULL;
    }
    pthread_mutex_unlock(&opus_list_lock);
    if (d) opus_release(d);
}

RT_ON_RESET(opus_reset) {
    // Reset hooks run after guest threads stop. Close also clears old service objects,
    // so a late close cannot free a subsequent boot's decoder.
    while (opus_sessions) opus_close(opus_sessions->owner);
}

static void opus_decode_request(OpusSession *d, IpcReq *req, IpcResp *resp) {
    uint32_t cmd = req->cmd;
    if (cmd == 1 && !d->params.multi) return;  // SetContext's input is unused by this interface.
    bool multi_command = cmd == 2 || cmd == 5 || cmd == 7 || cmd == 9;
    if ((cmd != 0 && cmd != 2 && cmd != 4 && cmd != 5 && cmd != 6 && cmd != 7 && cmd != 8 && cmd != 9) ||
        multi_command != d->params.multi) {
        resp->result = OPUS_UNSUPPORTED;
        if (getenv("LARECOMP_STRICT_IPC")) FATAL("[hwopus] unsupported decoder command %u\n", cmd);
        return;
    }
    if (cmd >= 6 && (!req->data || !req->data_size || req->data[0] > 1)) {
        resp->result = OPUS_BAD_ARGUMENT; return;
    }
    IpcBuf *input = in_buf(req, 0), *output = out_buf(req, 0);
    if (!opus_span(input, 8) || !opus_span(output, 2u * d->params.channels)) {
        resp->result = OPUS_BAD_ARGUMENT; return;
    }
    const uint8_t *packet = (const uint8_t *)(uintptr_t)input->addr;
    uint32_t bytes = read_be32(packet), final_range = read_be32(packet + 4);
    if (!bytes || bytes > INT_MAX - 8 || !opus_span(input, 8ull + bytes)) {
        resp->result = OPUS_BAD_ARGUMENT; return;
    }
    uint64_t room = output->size / (2u * d->params.channels);
    int samples = (int)(room < (uint64_t)d->max_samples ? room : (uint64_t)d->max_samples);
    if (!opus_span(output, (uint64_t)samples * d->params.channels * 2)) {
        resp->result = OPUS_BAD_ARGUMENT; return;
    }
    uint64_t start = plat_now_ns();
    pthread_mutex_lock(&d->lock);
    // libopus states are contiguous and position independent. Decode a copy so bad
    // packets, range mismatches and undersized destinations leave history/output intact.
    memcpy(d->trial, d->decoder, d->decoder_size);
    int result;
    if (cmd >= 6 && req->data[0]) {
        result = d->params.multi ? opus_multistream_decoder_ctl(d->trial, OPUS_RESET_STATE) :
                                  opus_decoder_ctl(d->trial, OPUS_RESET_STATE);
        if (result != OPUS_OK) goto done;
    }
    result = d->params.multi ? opus_multistream_decode(d->trial, packet + 8, bytes, d->pcm, samples, 0) :
                              opus_decode(d->trial, packet + 8, bytes, d->pcm, samples, 0);
    if (result > 0 && final_range) {
        opus_uint32 actual = 0;
        int status = d->params.multi ? opus_multistream_decoder_ctl(d->trial, OPUS_GET_FINAL_RANGE(&actual)) :
                                      opus_decoder_ctl(d->trial, OPUS_GET_FINAL_RANGE(&actual));
        if (status != OPUS_OK || actual != final_range) result = OPUS_INVALID_PACKET;
    }
    if (result > 0) {
        memcpy(d->decoder, d->trial, d->decoder_size);
        memcpy((void *)(uintptr_t)output->addr, d->pcm, (size_t)result * d->params.channels * 2);
    }
done:
    pthread_mutex_unlock(&d->lock);
    if (result <= 0) { resp->result = OPUS_BAD_ARGUMENT; return; }
    OUT(uint32_t, 0) = bytes + 8;
    OUT(uint32_t, 4) = (uint32_t)result;
    if (cmd >= 4) {
        OUT(uint64_t, 8) = plat_now_ns() - start;
        resp_size(resp, 16);
    } else resp_size(resp, 8);
}

static void opus_decoder_service(Service *s, IpcReq *req, IpcResp *resp) {
    // A clone can close its final handle while another thread's request is active.
    // Keep this decoder alive until every already-started request finishes.
    pthread_mutex_lock(&opus_list_lock);
    OpusSession *d = s->state;
    if (d) __atomic_add_fetch(&d->refs, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&opus_list_lock);
    if (!d) { resp->result = OPUS_BAD_ARGUMENT; return; }
    opus_decode_request(d, req, resp);
    opus_release(d);
}

SERVICE("hwopus", hwopus) {
    (void)s;
    if (req->cmd > 9) {
        resp->result = OPUS_UNSUPPORTED;
        if (getenv("LARECOMP_STRICT_IPC")) FATAL("[hwopus] unsupported manager command %u\n", req->cmd);
        return;
    }
    OpusParams params;
    if (!opus_params(req, &params)) { resp->result = OPUS_BAD_ARGUMENT; return; }
    uint32_t required = opus_work_size(&params);
    if (!required) { resp->result = OPUS_BAD_ARGUMENT; return; }
    bool open = req->cmd == 0 || req->cmd == 2 || req->cmd == 4 || req->cmd == 6;
    if (!open) {
        OUT(uint32_t, 0) = required; resp_size(resp, 4);
        return;
    }
    uint32_t offset = params.multi ? 0 : req->cmd == 4 ? 16 : 8;
    if (!req->data || req->data_size < offset + 4 || req->ncopy != 1) {
        resp->result = OPUS_BAD_ARGUMENT; return;
    }
    uint32_t work_size = read_u32(req->data + offset);
    pthread_mutex_lock(&g_klock);
    KObject *object = req->copy_handles[0] == 0xffff8000u ? NULL : handle_get(req->copy_handles[0]);
    bool transfer_ok = object && object->type == KOBJ_TMEM && ((KTransferMem *)object)->size >= work_size;
    pthread_mutex_unlock(&g_klock);
    if (!transfer_ok || work_size < required) { resp->result = OPUS_BAD_ARGUMENT; return; }
    OpusSession *d = calloc(1, sizeof *d);
    if (!d) { resp->result = OPUS_NO_MEMORY; return; }
    d->params = params;
    d->decoder_size = opus_state_size(&params);
    d->max_samples = params.rate * (params.large ? 120 : 40) / 1000;
    d->decoder = malloc(d->decoder_size);
    d->trial = malloc(d->decoder_size);
    d->pcm = malloc((size_t)d->max_samples * params.channels * 2);
    int status = OPUS_ALLOC_FAIL;
    if (d->decoder && d->trial && d->pcm)
        status = params.multi ? opus_multistream_decoder_init(d->decoder, params.rate, params.channels,
                                    params.streams, params.coupled, params.mapping) :
                                opus_decoder_init(d->decoder, params.rate, params.channels);
    if (status != OPUS_OK) {
        free(d->pcm); free(d->trial); free(d->decoder); free(d);
        resp->result = status == OPUS_ALLOC_FAIL ? OPUS_NO_MEMORY : OPUS_BAD_ARGUMENT;
        return;
    }
    pthread_mutex_init(&d->lock, NULL);
    d->refs = 1;
    Service *decoder = service_new("IHardwareOpusDecoder", opus_decoder_service, d);
    decoder->on_close = opus_close;
    d->owner = decoder;
    pthread_mutex_lock(&opus_list_lock);
    d->next = opus_sessions; opus_sessions = d;
    pthread_mutex_unlock(&opus_list_lock);
    ipc_return_object(resp, decoder);
    LOG("[hwopus] opened %d Hz, %d ch, %s, %d ms frames\n", params.rate, params.channels,
        params.multi ? "multistream" : "single stream", params.large ? 120 : 40);
}
