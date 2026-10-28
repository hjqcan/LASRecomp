// The settings menu's sounds (see menu_sfx.h). The game plays them as sequence sounds of its sound archive: a sound
// names a sequence and its start, the sequence picks an instrument and plays one note, the instrument's bank names the
// wave (DSP ADPCM or PCM in a wave archive). That chain is followed here once per sound, the wave decoded and resampled
// to 48 kHz stereo with the sound's, note's and instrument's volume in it, and kept; playing mixes it on a stream of
// our own, beside the game's audio and before the game is up.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "rt.h"
#include "settings.h"
#include "menu_sfx.h"
#include "title_profile.h"

extern const char *g_romfs_dir;

static const char *const g_names[MENU_SFX_N] = {"SE_SY_CURSOL_MOVE", "SE_SY_MENU_LR_MOVE", "SE_SY_DECIDE",
                                                "SE_SY_BEEP", "SE_SY_CANCEL", "SE_SY_MENU_OPEN", "SE_SY_MENU_CLOSE"};

#define RATE 48000

typedef struct {
    float *pcm;  // stereo, RATE
    uint32_t frames;
} Sfx;

static Sfx g_sfx[MENU_SFX_N];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_state;  // 0 not loaded, 1 loaded, 2 failed for good
static uint64_t g_retry_ns;  // (the file missing: looked for again after this)
static char g_game_dir[1024];

// ---- reading the archive (little endian; every read checked against what was read)
typedef struct { const uint8_t *p; uint32_t n; } Buf;
static bool in(Buf b, int64_t o, uint32_t len) { return o >= 0 && (uint64_t)o + len <= b.n; }
static uint32_t u32(Buf b, int64_t o) { return in(b, o, 4) ? b.p[o] | b.p[o + 1] << 8 | b.p[o + 2] << 16 | (uint32_t)b.p[o + 3] << 24 : 0; }
static uint16_t u16(Buf b, int64_t o) { return in(b, o, 2) ? (uint16_t)(b.p[o] | b.p[o + 1] << 8) : 0; }
static uint8_t u8(Buf b, int64_t o) { return in(b, o, 1) ? b.p[o] : 0; }

static uint8_t *read_at(FILE *f, uint64_t off, uint32_t len) {
    if (!len || len > (64u << 20)) return NULL;
    uint8_t *p = malloc(len);
#ifdef _WIN32
    int bad = _fseeki64(f, (long long)off, SEEK_SET);
#else
    int bad = fseeko(f, (off_t)off, SEEK_SET);
#endif
    if (p && (bad || fread(p, 1, len, f) != len)) free(p), p = NULL;
    return p;
}

// A block of a NintendoWare file (FSAR, FSEQ, FBNK, FWAR, FWAV: the same header): its offset (-1: none)
static int64_t block(Buf b, uint16_t id) {
    uint16_t n = u16(b, 0x10);
    for (int i = 0; i < n && i < 16; i++)
        if (u16(b, 0x14 + i * 12) == id) return u32(b, 0x14 + i * 12 + 4);
    return -1;
}
// A reference (u16 type, u16, s32 offset from base) among n in a row at o, by type: where it points (-1: none)
static int64_t ref_by_type(Buf b, int64_t o, int n, uint16_t type, int64_t base) {
    for (int i = 0; i < n; i++)
        if (u16(b, o + i * 8) == type) return base + (int32_t)u32(b, o + i * 8 + 4);
    return -1;
}
// The i-th entry of a reference table (u32 count, then references from the table's start); -1: none
static int64_t table_ref(Buf b, int64_t t, uint32_t i) {
    if (t < 0 || i >= u32(b, t) || !in(b, t + 4 + i * 8, 8) || !u16(b, t + 4 + i * 8)) return -1;
    return t + (int32_t)u32(b, t + 4 + i * 8 + 4);
}

typedef struct {
    FILE *f;
    uint64_t file_body;  // (the FILE block's data)
    Buf info;            // the INFO block, from its data
    int64_t sounds, banks, warcs, files;  // tables, in info
} Archive;

// A file of the archive (by file id): its offset in the archive and size (false: not in it - a group's own file)
static bool arc_file(Archive *a, uint32_t id, uint64_t *off, uint32_t *size) {
    int64_t e = table_ref(a->info, a->files, id);
    if (e < 0) return false;
    int64_t loc = e + (int32_t)u32(a->info, e + 4);  // (the internal file's location)
    if (u16(a->info, loc) != 0x1F00) return false;
    int32_t o = (int32_t)u32(a->info, loc + 4);
    *size = u32(a->info, loc + 8);
    if (o < 0 || !*size) return false;
    *off = a->file_body + (uint32_t)o;
    return true;
}

// MIDI-style variable length number
static uint32_t varlen(Buf b, int64_t *o) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t c = u8(b, (*o)++);
        v = v << 7 | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    return v;
}

// The sequence from start: the program and the first note (only what the menu's sounds use: program, note, waits,
// jumps; anything else ends the search)
static bool seq_first_note(Buf s, int64_t data, uint32_t start, int *prg, int *key, int *vel) {
    int64_t o = data + start;
    *prg = 0;
    for (int steps = 0; steps < 64 && in(s, o, 1); steps++) {
        uint8_t c = u8(s, o++);
        if (c < 0x80) { *key = c, *vel = u8(s, o); return true; }
        if (c == 0x80) varlen(s, &o);                // wait
        else if (c == 0x81) *prg = (int)varlen(s, &o);  // program
        else if (c == 0x89 || c == 0x8A)            // jump, call
            o = data + (u8(s, o) << 16 | u8(s, o + 1) << 8 | u8(s, o + 2));
        else if (c >= 0xB0 && c <= 0xDF) o++;       // (one-byte settings: volume, pan, ...)
        else return false;
    }
    return false;
}

// A region table (direct / range / index) at o in a bank: the entry for key (-1: none)
static int64_t region(Buf b, int64_t o, int key) {
    uint16_t type = u16(b, o);
    int64_t t = o + (int32_t)u32(b, o + 4);
    if (type == 0x6000) return u16(b, t) ? t + (int32_t)u32(b, t + 4) : -1;
    if (type == 0x6001) {  // u32 count, the keys' upper ends (padded to 4), references
        uint32_t n = u32(b, t);
        if (n > 128) return -1;
        int64_t refs = t + 4 + ((n + 3) & ~3u);
        for (uint32_t i = 0; i < n; i++)
            if (key <= u8(b, t + 4 + i)) return u16(b, refs + i * 8) ? t + (int32_t)u32(b, refs + i * 8 + 4) : -1;
        return -1;
    }
    if (type == 0x6002) {  // u8 min, u8 max, references
        int lo = u8(b, t), hi = u8(b, t + 1);
        if (key < lo || key > hi) return -1;
        int64_t r = t + 4 + (key - lo) * 8;
        return u16(b, r) ? t + (int32_t)u32(b, r + 4) : -1;
    }
    return -1;
}

// DSP ADPCM (8-byte frames: header + 14 samples) / PCM16 / PCM8 into floats
static void decode(Buf w, int64_t data, int enc, const int16_t *coefs, int16_t h1, int16_t h2, uint32_t n, float *out,
                   int stride) {
    for (uint32_t s = 0; s < n; s++) {
        int v = 0;
        if (enc == 2) {
            int64_t fr = data + (int64_t)(s / 14) * 8;
            uint8_t ps = u8(w, fr), byte = u8(w, fr + 1 + (s % 14) / 2);
            int nib = (s % 14) & 1 ? (int8_t)(byte << 4) >> 4 : (int8_t)byte >> 4;
            int ci = (ps >> 4) & 7;
            v = ((nib * (2048 << (ps & 15))) + coefs[ci * 2] * h1 + coefs[ci * 2 + 1] * h2 + 1024) >> 11;
            v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
            h2 = h1, h1 = (int16_t)v;
        } else if (enc == 1) v = (int16_t)u16(w, data + s * 2);
        else v = (int8_t)u8(w, data + s) * 256;
        out[(size_t)s * stride] = v / 32768.0f;
    }
}

// One sound, by name, into g_sfx[id]; why says what failed
static bool load_one(Archive *a, Buf strg, int id, char *why, size_t wn) {
    const char *name = g_names[id];
    // its string's index (STRG: string table of (u16, u16, s32 offset, u32 size) from the table)
    int64_t st = 8 + (int32_t)u32(strg, 8 + 4);
    uint32_t nstr = u32(strg, st);
    int64_t sid = -1;
    size_t nl = strlen(name) + 1;
    for (uint32_t i = 0; i < nstr && sid < 0; i++) {
        int64_t e = st + 4 + i * 12, so = st + (int32_t)u32(strg, e + 4);
        if (u32(strg, e + 8) == nl && in(strg, so, (uint32_t)nl) && !memcmp(strg.p + so, name, nl)) sid = i;
    }
    if (sid < 0) return snprintf(why, wn, "no such sound"), false;
    // the sound with that name: u32 file, u32 player, u8 volume, ..., detail reference at 12, u32 flags at 20, then
    // its optional values (bit 0: the name)
    Buf in_ = a->info;
    int64_t snd = -1;
    for (uint32_t i = 0, n = u32(in_, a->sounds); i < n && snd < 0; i++) {
        int64_t o = table_ref(in_, a->sounds, i);
        if (o >= 0 && (u32(in_, o + 20) & 1) && (u32(in_, o + 24) & 0xFFFFFF) == (uint32_t)sid) snd = o;
    }
    if (snd < 0) return snprintf(why, wn, "no sound info"), false;
    uint32_t seq_file = u32(in_, snd);
    float vol = u8(in_, snd + 8) / 127.0f;
    if (u16(in_, snd + 12) != 0x2203) return snprintf(why, wn, "not a sequence sound"), false;
    int64_t det = snd + (int32_t)u32(in_, snd + 16);
    int64_t banks = det + (int32_t)u32(in_, det + 4);
    uint32_t start = u32(in_, det + 12) & 1 ? u32(in_, det + 16) : 0;
    if (!u32(in_, banks)) return snprintf(why, wn, "no bank"), false;
    uint32_t bank_id = u32(in_, banks + 4) & 0xFFFFFF;

    bool ok = false;
    uint8_t *seq = NULL, *bnk = NULL, *war = NULL, *wav = NULL;
    uint64_t off;
    uint32_t size;
    // the sequence: program and note
    if (!arc_file(a, seq_file, &off, &size) || !(seq = read_at(a->f, off, size))) { snprintf(why, wn, "sequence missing"); goto done; }
    Buf s = {seq, size};
    int prg, key, vel;
    int64_t sd = block(s, 0x5000);
    if (memcmp(seq, "FSEQ", 4) || sd < 0 || !seq_first_note(s, sd + 8, start, &prg, &key, &vel)) { snprintf(why, wn, "sequence unreadable"); goto done; }
    // the bank: program -> key region -> velocity region -> wave, its key, volume and pitch
    int64_t be = table_ref(in_, a->banks, bank_id);
    if (be < 0 || !arc_file(a, u32(in_, be), &off, &size) || !(bnk = read_at(a->f, off, size))) { snprintf(why, wn, "bank missing"); goto done; }
    Buf b = {bnk, size};
    int64_t bi = block(b, 0x5800);
    if (memcmp(bnk, "FBNK", 4) || bi < 0) { snprintf(why, wn, "bank unreadable"); goto done; }
    bi += 8;
    int64_t waves = ref_by_type(b, bi, 2, 0x0100, bi), insts = ref_by_type(b, bi, 2, 0x0101, bi);
    int64_t inst = table_ref(b, insts, (uint32_t)prg);
    int64_t kr = inst >= 0 ? region(b, inst, key) : -1;
    int64_t vr = kr >= 0 ? region(b, kr, vel) : -1;
    if (waves < 0 || vr < 0) { snprintf(why, wn, "no instrument %d", prg); goto done; }
    uint32_t wave_i = u32(b, vr), vflags = u32(b, vr + 4);
    int64_t opt = vr + 8;
    int orig_key = 60;
    float ivol = 1, pitch = 1;
    if (vflags & 1) orig_key = (int)u32(b, opt), opt += 4;
    if (vflags & 2) ivol = u32(b, opt) / 127.0f, opt += 4;
    if (vflags & 4) opt += 4;  // (pan: centred here)
    if (vflags & 8) { uint32_t p = u32(b, opt); memcpy(&pitch, &p, 4); opt += 4; }
    if (wave_i >= u32(b, waves)) { snprintf(why, wn, "no wave %u", wave_i); goto done; }
    uint32_t warc_id = u32(b, waves + 4 + wave_i * 8) & 0xFFFFFF, wave_idx = u32(b, waves + 8 + wave_i * 8);
    // the wave archive: only its header and the one wave
    int64_t we = table_ref(in_, a->warcs, warc_id);
    if (we < 0 || !arc_file(a, u32(in_, we), &off, &size) || size < 0x40) { snprintf(why, wn, "wave archive missing"); goto done; }
    uint64_t war_off = off;
    uint32_t war_size = size;
    if (!(war = read_at(a->f, war_off, 0x40))) { snprintf(why, wn, "wave archive unreadable"); goto done; }
    Buf wh = {war, 0x40};
    int64_t wi = block(wh, 0x6800), wf = block(wh, 0x6801);
    uint32_t wi_size = u32(wh, 0x14 + 8);  // (INFO is the first block)
    if (memcmp(war, "FWAR", 4) || wi < 0 || wf < 0 || u16(wh, 0x14) != 0x6800) { snprintf(why, wn, "wave archive unreadable"); goto done; }
    free(war);
    if (!(war = read_at(a->f, war_off + (uint64_t)wi, wi_size))) { snprintf(why, wn, "wave archive unreadable"); goto done; }
    Buf wib = {war, wi_size};
    if (wave_idx >= u32(wib, 8)) { snprintf(why, wn, "no wave %u in its archive", wave_idx); goto done; }
    int32_t wo = (int32_t)u32(wib, 12 + wave_idx * 12 + 4);
    uint32_t wsize = u32(wib, 12 + wave_idx * 12 + 8);
    if (wo < 0 || (uint64_t)wf + 8 + (uint32_t)wo + wsize > war_size || !(wav = read_at(a->f, war_off + wf + 8 + (uint32_t)wo, wsize))) {
        snprintf(why, wn, "wave unreadable");
        goto done;
    }
    // the wave: INFO (encoding, rate, length, channels), DATA
    Buf w = {wav, wsize};
    int64_t vi = block(w, 0x7000), vd = block(w, 0x7001);
    if (memcmp(wav, "FWAV", 4) || vi < 0 || vd < 0) { snprintf(why, wn, "wave unreadable"); goto done; }
    vi += 8;
    int enc = u8(w, vi);
    uint32_t rate = u32(w, vi + 4), n = u32(w, vi + 12);
    int64_t chans = vi + 0x14;
    uint32_t nch = u32(w, chans);
    if (enc > 2 || !rate || !n || !nch || nch > 8 || n > RATE * 30) { snprintf(why, wn, "wave format %d, %u Hz, %u ch", enc, rate, nch); goto done; }
    if (nch > 2) nch = 2;
    float *src = calloc((size_t)n * 2, sizeof(float));
    if (!src) { snprintf(why, wn, "out of memory"); goto done; }
    for (uint32_t c = 0; c < nch; c++) {
        int64_t ci = table_ref(w, chans, c);
        int64_t data = vd + 8 + (int32_t)u32(w, ci + 4);
        int16_t coefs[16] = {0}, h1 = 0, h2 = 0;
        if (enc == 2) {
            int64_t ai = ci + (int32_t)u32(w, ci + 12);
            for (int k = 0; k < 16; k++) coefs[k] = (int16_t)u16(w, ai + k * 2);
            h1 = (int16_t)u16(w, ai + 34), h2 = (int16_t)u16(w, ai + 36);
        }
        decode(w, data, enc, coefs, h1, h2, n, src + c, 2);
    }
    if (nch == 1)
        for (uint32_t i = 0; i < n; i++) src[i * 2 + 1] = src[i * 2];
    // to RATE at the note's pitch, with the volumes (the note's velocity squared, as the game's sequencer does)
    double step = rate * (double)pitch * pow(2.0, (key - orig_key) / 12.0) / RATE;
    if (!(step > 1.0 / 64 && step < 64) || (n - 1) / step > RATE * 30) { free(src); snprintf(why, wn, "pitch %g", step); goto done; }
    float gain = vol * ivol * (vel / 127.0f) * (vel / 127.0f);
    uint32_t frames = (uint32_t)((n - 1) / step) + 1;
    float *pcm = malloc((size_t)frames * 2 * sizeof(float));
    if (!pcm) { free(src); snprintf(why, wn, "out of memory"); goto done; }
    for (uint32_t i = 0; i < frames; i++) {
        double t = i * step;
        uint32_t j = (uint32_t)t;
        float f = (float)(t - j);
        uint32_t k = j + 1 < n ? j + 1 : j;
        for (int c = 0; c < 2; c++) pcm[i * 2 + c] = (src[j * 2 + c] * (1 - f) + src[k * 2 + c] * f) * gain;
    }
    free(src);
    while (frames > 1 && fabsf(pcm[frames * 2 - 2]) < 1e-4f && fabsf(pcm[frames * 2 - 1]) < 1e-4f) frames--;  // (silence at the end)
    g_sfx[id].pcm = pcm, g_sfx[id].frames = frames;
    LOG("[menu_sfx] %s: %s %u Hz %u ch, %.0f ms (program %d, key %d, gain %.3f)\n", name,
        enc == 2 ? "ADPCM" : enc == 1 ? "PCM16" : "PCM8", rate, nch, frames * 1000.0 / RATE, prg, key, gain);
    ok = true;
done:
    free(seq), free(bnk), free(war), free(wav);
    return ok;
}

// The archive's sounds, once (g_lock held). 0 loaded (some at least), 1 the file isn't there, 2 unreadable.
static int load_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    int ret = 2;
    uint8_t hdr[0x40], *strg_p = NULL, *info_p = NULL;
    Archive a = {f};
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, "FSAR", 4) || hdr[4] != 0xFF || hdr[5] != 0xFE) goto out;
    Buf h = {hdr, sizeof hdr};
    int64_t so = block(h, 0x2000), io = block(h, 0x2001), fo = block(h, 0x2002);
    uint32_t ssz = 0, isz = 0;
    for (int i = 0; i < 3; i++) {
        uint16_t t = u16(h, 0x14 + i * 12);
        if (t == 0x2000) ssz = u32(h, 0x14 + i * 12 + 8);
        if (t == 0x2001) isz = u32(h, 0x14 + i * 12 + 8);
    }
    if (so < 0 || io < 0 || fo < 0 || !(strg_p = read_at(f, (uint64_t)so, ssz)) || !(info_p = read_at(f, (uint64_t)io, isz))) goto out;
    Buf strg = {strg_p, ssz};
    a.info = (Buf){info_p + 8, isz - 8};
    a.file_body = (uint64_t)fo + 8;
    a.sounds = ref_by_type(a.info, 0, 8, 0x2100, 0), a.banks = ref_by_type(a.info, 0, 8, 0x2101, 0);
    a.warcs = ref_by_type(a.info, 0, 8, 0x2103, 0), a.files = ref_by_type(a.info, 0, 8, 0x2106, 0);
    if (memcmp(strg_p, "STRG", 4) || memcmp(info_p, "INFO", 4) || a.sounds < 0 || a.banks < 0 || a.warcs < 0 || a.files < 0) goto out;
    int got = 0;
    for (int i = 0; i < MENU_SFX_N; i++) {
        char why[96];
        if (load_one(&a, strg, i, why, sizeof why)) got++;
        else LOG("[menu_sfx] %s: %s\n", g_names[i], why);
    }
    ret = got ? 0 : 2;
out:
    free(strg_p), free(info_p);
    fclose(f);
    return ret;
}

static bool no_audio(void) {
    const char *e = getenv("LARECOMP_NO_AUDIO");
    return e && strcmp(e, "0");
}

// Loaded (g_lock held): true when there are sounds to play
static bool ensure_loaded(void) {
    if (!title_is_las()) return false;
    if (g_state == 1) return true;
    if (g_state) return false;
    uint64_t now = SDL_GetTicksNS();
    if (now < g_retry_ns) return false;
    char path[1200];
    if (g_game_dir[0]) snprintf(path, sizeof path, "%s/romfs/region_common/audio/Richard.bfsar", g_game_dir);
    else if (g_romfs_dir) snprintf(path, sizeof path, "%s/region_common/audio/Richard.bfsar", g_romfs_dir);
    else return false;
    int r = load_all(path);
    static bool told;
    if (r == 1) {  // (not set up yet: the start screen may set the game files up, looked for again in a while)
        if (!told) LOG("[menu_sfx] no menu sounds: %s is missing\n", path), told = true;
        g_retry_ns = now + 3000000000ull;
        return false;
    }
    if (r == 2) LOG("[menu_sfx] no menu sounds: %s is unreadable\n", path);
    else LOG("[menu_sfx] menu sounds loaded in %.1f ms\n", (SDL_GetTicksNS() - now) / 1e6);
    g_state = r ? 2 : 1;
    return g_state == 1;
}

// ---- playing: up to a few at once, mixed on our stream (its callback)
#define VOICES 8
typedef struct { int id; uint32_t pos; uint64_t start; } Voice;
static Voice g_voice[VOICES];
static int g_nvoice;
static SDL_AudioStream *g_stream;
static bool g_stream_failed;
static uint64_t g_idle_since;
// (the device's pause / resume: never taken by the callback, so SDL's own stream lock and g_lock can't cross)
static pthread_mutex_t g_dev_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_paused = true;  // (g_dev_lock; a new stream starts paused)

static void SDLCALL feed(void *ud, SDL_AudioStream *st, int need, int total) {
    (void)ud, (void)total;
    float buf[512 * 2];
    static float env = -1;  // (LARECOMP_AUDIO_GAIN, as the game's audio)
    if (env < 0) env = getenv("LARECOMP_AUDIO_GAIN") ? (float)atof(getenv("LARECOMP_AUDIO_GAIN")) : 1.0f;
    float gain = g_settings.volume * env;
    while (need > 0) {
        int n = need / 8 < 512 ? need / 8 : 512;
        if (n <= 0) n = 1;
        memset(buf, 0, sizeof buf);
        pthread_mutex_lock(&g_lock);
        for (int v = 0; v < g_nvoice;) {
            Voice *vo = &g_voice[v];
            const Sfx *s = &g_sfx[vo->id];
            uint32_t left = s->frames - vo->pos, k = left < (uint32_t)n ? left : (uint32_t)n;
            for (uint32_t i = 0; i < k * 2; i++) buf[i] += s->pcm[vo->pos * 2 + i] * gain;
            vo->pos += k;
            if (vo->pos >= s->frames) g_voice[v] = g_voice[--g_nvoice];
            else v++;
        }
        if (!g_nvoice && !g_idle_since) g_idle_since = SDL_GetTicksNS();
        pthread_mutex_unlock(&g_lock);
        SDL_PutAudioStreamData(st, buf, n * 8);
        need -= n * 8;
    }
}

void menu_sfx_set_game_dir(const char *dir) {
    pthread_mutex_lock(&g_lock);
    if (strcmp(g_game_dir, dir ? dir : "")) {
        g_nvoice = 0;
        for (int i = 0; i < MENU_SFX_N; i++) { free(g_sfx[i].pcm); g_sfx[i] = (Sfx){0}; }
        g_state = 0;
        g_retry_ns = 0;
        g_idle_since = SDL_GetTicksNS();
    }
    snprintf(g_game_dir, sizeof g_game_dir, "%s", dir ? dir : "");
    pthread_mutex_unlock(&g_lock);
}

void menu_sfx_load_now(void) {
    pthread_mutex_lock(&g_lock);
    ensure_loaded();
    pthread_mutex_unlock(&g_lock);
}

void menu_sfx_play(int id) {
    static int off = -1;
    if (off < 0) off = no_audio();
    if (id < 0 || id >= MENU_SFX_N || off) return;
    pthread_mutex_lock(&g_lock);
    if (!ensure_loaded() || !g_sfx[id].pcm) { pthread_mutex_unlock(&g_lock); return; }
    if (!g_stream && !g_stream_failed) {
        g_stream_failed = true;
        SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, RATE};
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) LOG("[menu_sfx] SDL audio init failed: %s\n", SDL_GetError());
        else if (!(g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL)))
            LOG("[menu_sfx] SDL audio open failed: %s\n", SDL_GetError());
        else g_stream_failed = false;
    }
    if (!g_stream) { pthread_mutex_unlock(&g_lock); return; }
    // one press, one sound: a more important sound just before drops this one; this drops less important ones
    uint64_t now = SDL_GetTicksNS(), recent = 150000000ull;
    bool drop = false;
    for (int v = 0; v < g_nvoice; v++)
        if (now - g_voice[v].start < recent && g_voice[v].id > id) drop = true;
    if (!drop) {
        for (int v = 0; v < g_nvoice;)
            if (now - g_voice[v].start < recent && g_voice[v].id < id) g_voice[v] = g_voice[--g_nvoice];
            else v++;
        if (g_nvoice == VOICES) {  // (the oldest makes room)
            int old = 0;
            for (int v = 1; v < g_nvoice; v++)
                if (g_voice[v].start < g_voice[old].start) old = v;
            g_voice[old] = g_voice[--g_nvoice];
        }
        g_voice[g_nvoice++] = (Voice){id, 0, now};
        g_idle_since = 0;
    }
    pthread_mutex_unlock(&g_lock);
    if (drop) return;
    pthread_mutex_lock(&g_dev_lock);
    if (g_paused) g_paused = false, SDL_ResumeAudioStreamDevice(g_stream);
    pthread_mutex_unlock(&g_dev_lock);
}

// Nothing playing for a while: the stream's device pauses (an idle start screen keeps no audio running)
void menu_sfx_idle_check(void) {
    pthread_mutex_lock(&g_dev_lock);
    pthread_mutex_lock(&g_lock);
    bool pause = g_stream && !g_paused && !g_nvoice && g_idle_since && SDL_GetTicksNS() - g_idle_since > 2000000000ull;
    pthread_mutex_unlock(&g_lock);
    if (pause) g_paused = true, SDL_PauseAudioStreamDevice(g_stream);
    pthread_mutex_unlock(&g_dev_lock);
}
