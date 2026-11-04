// Frame interpolation: images between the game's own frames (Video tab "Frame interpolation", off by default).
//
// The game steps its world once per frame, so smoother motion than its frame rate needs images it never made. Each
// frame's command lists are run a second time (more for more images) with the uniforms of each draw blended between
// the previous frame and this one - matrices, colours, animation - as RT64 does for N64 games. The backend's present
// (render thread) shows the images on the display's own vsyncs, a fixed latency behind the game.
//
// Draws are matched across frames by programs + draw arguments + target size (the game's render-target objects and
// dynamic buffer addresses change every frame; this matched 96-100% of draws in Mabe Village, Tail Cave, a door
// transition, a pit); several draws of one key (the same mesh for different objects: Cuccos, chain links) are paired
// by the nearest per-object uniforms, not their order, which the game's depth sort changes between frames
// (match_all). Every uniform buffer of a matched draw is paired by position and blended element-wise; integers
// (stored in float slots they read as denormals) never are, nor whole numbers that changed by one or more (a texture's
// frame, a mode: this frame's), and a texture translation that wrapped (0.98 -> 0.01) blends the short way round. An element moving further than the cut distance (+5% of
// its size: projection terms are large) is a jump - it shows its new value at once (a camera's settings switching as
// Link enters another part of the overworld: one value of its buffer). A jump in a shared block (the camera, a pass:
// a camera cut), jumps in most of the buffers that changed (a warp), a level change, too few matches or a texture
// going away during the frame make the whole frame a cut: no in-between images, the frame shows as it is. The
// shadow pass's light block and its fragment-stage copies snap together, as one light.
//
//   LARECOMP_INTERP=on|measure|off   override the setting (measure: log and match only, no extra images)
//   LARECOMP_INTERP_CUT=<d>          the jump distance (default 4 world units)
//   LARECOMP_INTERP_STAGES=<m>       which stages' uniforms are blended: 1 vertex, 2 fragment, 3 both (default 3: with
//                                    the vertex stage's only, the shadow maps - drawn again with blended light matrices -
//                                    were read with the game frame's matrices in the fragment stage: Mabe Village's
//                                    cast shadows faded / dithered in the in-betweens)
//   LARECOMP_INTERP_LOG=<s>          stats line period in seconds (default 2; 0: none)
//   LARECOMP_INTERP_LOG_CUTS=1       log each frame cut and why
//   LARECOMP_INTERP_SNAP=elem        a jumping buffer snaps per element (default: the whole buffer shows this frame)
//   LARECOMP_INTERP_TOGGLE=0         blend values that alternate every frame too (default: they keep this frame's value)
//   LARECOMP_INTERP_ROT=0            rotations blend element-wise (default: along the arc, rot_plan)
//   LARECOMP_INTERP_ROT_SWITCH=<deg> a turn past this in one frame switches half-way (default 100)
//   LARECOMP_INTERP_HALFWAY=0        switched values (whole numbers, discontinuities) show this frame's at once
//   LARECOMP_INTERP_UNIFY=0          draws reading the same uniforms decide apart (default: one decision per buffer)
//   LARECOMP_INTERP_ORDER_CHECK=0    pairs by occurrence (more than 64 draws of a key) unchecked
//   LARECOMP_INTERP_GEO_COPY=0       replays read the per-frame geometry in place (renderer.m iv_geometry)
//   research: LARECOMP_INTERP_LOG_SPLITS=1 logs each buffer decided apart (before LARECOMP_INTERP_UNIFY joins them)
//   research: LARECOMP_INTERP_LOG_MOVES=<d> [_FROM=<frame>] logs uniform buffers whose elements moved more than d in one
//   frame; LARECOMP_INTERP_DUMP=<frame>:<path> writes both frames' draws and uniform copies as text at that frame;
//   LARECOMP_INTERP_LOG_KEYS=1 (renderer.m) logs each draw key's components once
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "../rt.h"
#include "../platform.h"
#include "../settings.h"
#include "interp.h"

bool g_interp_log, g_interp_replaying;
uint64_t g_interp_present_frame;

int interp_wanted(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("LARECOMP_INTERP");
        env = !e || !*e ? -1 : !strcmp(e, "measure") ? INTERP_MEASURE
            : (!strcmp(e, "0") || !strcmp(e, "off")) ? INTERP_OFF : INTERP_ON;
        if (env >= 0) LOG("[interp] LARECOMP_INTERP=%s\n", e);
    }
    if (env >= 0) return env;
#ifdef __APPLE__
    return g_settings.frame_interp ? INTERP_ON : INTERP_OFF;
#else
    return INTERP_OFF;  // (Metal only so far)
#endif
}

typedef struct {
    uint8_t stage, slot, snap;  // snap: elements past the jump distance show the new value (a jump, not motion)
    uint32_t off, size;
    uint32_t used;  // the bytes the shader reads: compared and blended; the rest (unwritten leftovers) are this frame's
    uint64_t addr;  // the guest address of the buffer (research: is it an object's identity across frames?)
    int64_t prev_off, blend;
    int64_t prev2_off;  // the same buffer two frames back (-1: none): an element back at that value toggles, not moves
    float maxd;  // the largest element change from the previous frame (-1: not matched): how fast it was moving
    uint32_t plan_first, plan_n; uint8_t plan_switch;  // its rotation blocks (interp_prepare's plans)
    int32_t dup;    // an earlier-decided copy with the same bytes in all three frames (frame_end): its blend is this one's
    uint8_t still;  // unchanged since the previous frame (interp_prepare's first image): shown as it is, never blended
} CbRec;
typedef struct {
    uint64_t key; uint32_t occ, first_cb, ncb;
    uint8_t depth_only;  // drawn into a depth-only target (a shadow pass: its shared blocks are the light's)
    uint64_t obj;   // the object: the guest addresses of its per-object uniform buffers (0: none - UI); frame_end
    uint32_t oocc;  // occurrence of (obj, key) in the frame
    int32_t pair;   // the draw's partner in the frame before (-1 none; -2 UI, not blended); frame_end
} DrawRec;
typedef struct { uint64_t addr, size, off; } StoRec;  // a storage range read through gmem: its bytes at the time, in `bytes`
typedef struct {
    StoRec *sto; uint32_t nsto, cap_sto;
    struct { uint64_t addr, size; } *dout; uint32_t ndout, cap_dout;  // the frame's dispatch outputs
    DrawRec *draws; uint32_t ndraws, cap_draws;
    CbRec *cbs; uint32_t ncbs, cap_cbs;
    uint8_t *bytes; uint64_t nbytes, cap_bytes;
    uint64_t *handles; uint32_t nhandles, cap_handles;
    bool overflow, destroyed;
    uint64_t free_gen;  // interp_free_begin's count when the frame's first list ran
} Frame;

static Frame fr[3];
static Frame *cur = &fr[0], *prev = &fr[1], *prev2 = &fr[2];  // (prev2: the frame before prev - object identity, below)
static bool prev_valid, frame_open, force_cut, can_blend;
static _Atomic uint64_t called[256];  // the game thread's present call time per frame (interp_stamp)
static uint8_t *blend_bytes; static uint64_t blend_n, blend_cap;

#define GROW(p, n, cap, init) do { if ((n) >= (cap)) { (cap) = (cap) ? (cap) * 2 : (init); (p) = realloc((p), sizeof *(p) * (cap)); } } while (0)

// occurrence counting / matching: open addressing on (key, occ)
#define HBITS 15
static uint64_t occ_keys[1 << HBITS]; static uint32_t occ_cnt[1 << HBITS];
static uint64_t m_keys[1 << HBITS]; static uint32_t m_occ[1 << HBITS]; static int32_t m_idx[1 << HBITS];

static uint32_t next_occ(uint64_t key) {
    uint32_t h = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> (64 - HBITS));
    for (int i = 0; i < (1 << HBITS); i++, h = (h + 1) & ((1 << HBITS) - 1)) {
        if (occ_keys[h] == key) return occ_cnt[h]++;
        if (!occ_keys[h]) { occ_keys[h] = key; occ_cnt[h] = 1; return 0; }
    }
    return 0;
}
// Object identity (Dusklight keys interpolation by the address of an object's own matrix storage). The game double-
// buffers each object's uniform blocks by frame parity: a draw's per-object buffers (vertex stage, slots 2 and up) sit
// at the same guest addresses as that object's draw two frames earlier, never the frame before (measured in Mabe
// Village: 49 of 49 static objects' addresses equal to N-2's, 0 to N-1's). So the draw of frame N finds its object in
// frame N-2 by address, and that draw's partner in N-1 - paired the frame before - is its own partner now. Objects
// without one in N-2 (just appeared, or a cut frame between) fall back to the nearest per-object uniforms (match_all).
static uint64_t i_keys[1 << HBITS]; static uint32_t i_occ[1 << HBITS]; static int32_t i_idx[1 << HBITS];
static uint64_t ihash(uint64_t obj, uint64_t key, uint32_t occ) {
    return (obj * 0xff51afd7ed558ccdull ^ key ^ (uint64_t)occ * 0xc4ceb9fe1a85ec53ull) * 0x9E3779B97F4A7C15ull;
}
static void index_prev2(void) {
    memset(i_idx, 0xff, sizeof i_idx);
    for (uint32_t j = 0; j < prev2->ndraws && j < (1u << HBITS) / 2; j++) {
        DrawRec *d = &prev2->draws[j];
        if (!d->obj) continue;
        uint32_t h = (uint32_t)(ihash(d->obj, d->key, d->oocc) >> (64 - HBITS));
        while (i_idx[h] >= 0) h = (h + 1) & ((1 << HBITS) - 1);
        i_keys[h] = ihash(d->obj, d->key, d->oocc), i_occ[h] = d->oocc, i_idx[h] = (int32_t)j;
    }
}
static int32_t lookup_prev2(uint64_t obj, uint64_t key, uint32_t oocc) {
    uint64_t ik = ihash(obj, key, oocc);
    uint32_t h = (uint32_t)(ik >> (64 - HBITS));
    for (int i = 0; i < (1 << HBITS); i++, h = (h + 1) & ((1 << HBITS) - 1)) {
        if (i_idx[h] < 0) return -1;
        if (i_keys[h] == ik && i_occ[h] == oocc) return i_idx[h];
    }
    return -1;
}
// The objects of this frame's draws and their occurrence per (obj, key): from the logged buffers' addresses.
static uint64_t o_keys[1 << HBITS]; static uint32_t o_cnt[1 << HBITS];
static void assign_objects(Frame *F) {
    memset(o_keys, 0, sizeof o_keys);
    for (uint32_t j = 0; j < F->ndraws; j++) {
        DrawRec *d = &F->draws[j];
        uint64_t obj = 0;
        for (uint32_t i = 0; i < d->ncb; i++) {
            CbRec *c = &F->cbs[d->first_cb + i];
            if (c->stage == 0 && c->slot >= 2 && c->size >= 48) obj = (obj ^ c->addr ^ (uint64_t)c->slot << 56) * 0x9E3779B97F4A7C15ull;
        }
        d->obj = obj;
        d->oocc = 0;
        if (!obj) continue;
        uint64_t ok = ihash(obj, d->key, 0) | 1;
        uint32_t h = (uint32_t)(ok >> (64 - HBITS));
        for (int i = 0; i < (1 << HBITS); i++, h = (h + 1) & ((1 << HBITS) - 1)) {
            if (o_keys[h] == ok) { d->oocc = o_cnt[h]++; break; }
            if (!o_keys[h]) { o_keys[h] = ok; o_cnt[h] = 1; break; }
        }
    }
}

static uint32_t mhash(uint64_t key, uint32_t occ) {
    return (uint32_t)((key ^ (uint64_t)occ * 0xff51afd7ed558ccdull) * 0x9E3779B97F4A7C15ull >> (64 - HBITS));
}
static int32_t lookup_prev(uint64_t key, uint32_t occ) {
    uint32_t h = mhash(key, occ);
    for (int i = 0; i < (1 << HBITS); i++, h = (h + 1) & ((1 << HBITS) - 1)) {
        if (m_idx[h] < 0) return -1;
        if (m_keys[h] == key && m_occ[h] == occ) return m_idx[h];
    }
    return -1;
}
static void index_prev(void) {
    memset(m_idx, 0xff, sizeof m_idx);
    for (uint32_t j = 0; j < prev->ndraws && j < (1u << HBITS) / 2; j++) {
        uint32_t h = mhash(prev->draws[j].key, prev->draws[j].occ);
        while (m_idx[h] >= 0) h = (h + 1) & ((1 << HBITS) - 1);
        m_keys[h] = prev->draws[j].key, m_occ[h] = prev->draws[j].occ, m_idx[h] = (int32_t)j;
    }
}

// Several draws of one key in a frame are the same mesh drawn for different objects (two Cuccos, BowWow's chain links,
// grass tufts) - or the same object drawn in several passes. The game's order among them changes from frame to frame
// (it sorts by depth: as the camera moves, two objects swap), so pairing them by occurrence blends one object's uniforms
// with another's: the in-between shows the object half-way between the two, and its shadow with it (Mabe Village,
// running east past the well: a Cucco on the path that was in the yard, BowWow gone, a dark wedge of shadow on the
// grass). Each is paired with the draw of the previous frame whose per-object uniforms are nearest to its own instead.
static uint8_t *p_used; static uint32_t p_used_cap;
static bool blendable(float a, float b);
static float cut_threshold(void);
static bool history_reset;  // a forced cut (a level change, a resize): the object identity chain starts over
// How far apart two draws' objects are: the per-object uniforms (vertex stage, slots 2 and up: the model matrix, the
// bones). The shared buffers (slots 0, 1: the pass, the camera) say nothing about the object and their unwritten tails
// hold leftovers that differ from frame to frame (HUD quads: -3e38, 255, 1268 in slot 0's last rows).
static float draw_distance(const DrawRec *d, const DrawRec *p) {
    float dist = 0;
    for (uint32_t i = 0; i < d->ncb && i < p->ncb; i++) {
        const CbRec *cc = &cur->cbs[d->first_cb + i], *pc = &prev->cbs[p->first_cb + i];
        if (cc->stage != pc->stage || cc->slot != pc->slot || cc->size != pc->size) return 1e30f;
        if (cc->stage != 0 || cc->slot < 2) continue;
        const float *a = (const float *)(prev->bytes + pc->off), *b = (const float *)(cur->bytes + cc->off);
        if (!memcmp(a, b, cc->used)) continue;
        for (uint32_t q = 0; q < cc->used / 4; q++)
            if (blendable(a[q], b[q])) dist += fabsf(b[q] - a[q]);
            else if (a[q] != b[q]) dist += 1;  // (an integer / flag changed: another object)
    }
    return dist;
}
// The largest per-object element change between two draws: past the cut distance they are not one object that moved
// but one that left and one that appeared (Dusklight, RT64: no match - drawn as it is, never blended with another).
static float draw_maxdiff(const DrawRec *d, const DrawRec *p) {
    float m = 0;
    for (uint32_t i = 0; i < d->ncb && i < p->ncb; i++) {
        const CbRec *cc = &cur->cbs[d->first_cb + i], *pc = &prev->cbs[p->first_cb + i];
        if (cc->stage != 0 || cc->slot < 2 || pc->stage != cc->stage || pc->slot != cc->slot || pc->size != cc->size) continue;
        const float *a = (const float *)(prev->bytes + pc->off), *b = (const float *)(cur->bytes + cc->off);
        if (!memcmp(a, b, cc->used)) continue;
        for (uint32_t q = 0; q < cc->used / 4; q++)
            if (blendable(a[q], b[q])) { float dd = fabsf(b[q] - a[q]) - 0.05f * fmaxf(fabsf(a[q]), fabsf(b[q])); if (dd > m) m = dd; }
    }
    return m;
}
static uint32_t object_bytes(const DrawRec *d) {  // the per-object uniforms a draw has (none: HUD quads, decals)
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->ncb; i++) {
        const CbRec *cc = &cur->cbs[d->first_cb + i];
        if (cc->stage == 0 && cc->slot >= 2) n += cc->size;
    }
    return n;
}

// The pairing of a key's draws: every (this frame, previous frame) pair of the key by distance, nearest first, each draw
// once (greedy in that order: the draws that didn't change pair first; one whose content jumped takes what is left,
// its own, instead of taking another's and starting a chain of wrong pairs). Equal distances - the same object drawn
// in two passes (depth pre-pass and colour pass: identical per-object data, different per-pass buffers) - keep the
// occurrence order: crossing those blended one pass's camera / light buffer with the other's (whole scene flat-lit).
typedef struct { float d; uint16_t c, p, docc; } Pair;
static int pair_cmp(const void *a, const void *b) {
    const Pair *x = a, *y = b;
    if (x->d != y->d) return x->d < y->d ? -1 : 1;
    return x->docc < y->docc ? -1 : x->docc > y->docc;
}
static uint64_t st_order_pairs, s_order_pairs;
static int order_check = -1;  // LARECOMP_INTERP_ORDER_CHECK=0: occurrence pairs unchecked (research)
static int32_t *p_match;  // per draw of this frame: its previous-frame draw (-1: none; -2: UI; -3: just appeared - not blended)
static uint64_t st_id_pairs, st_content_pairs, st_new_objects, s_id_pairs, s_content_pairs, s_new_objects, s_class_snaps, s_toggled, s_integral, s_wraps, s_discont, s_split, s_reused, s_onoff;  // (stats)
static uint32_t *p_order; static uint32_t p_order_cap;
static int order_cmp(const void *a, const void *b) {
    const DrawRec *x = &cur->draws[*(const uint32_t *)a], *y = &cur->draws[*(const uint32_t *)b];
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->occ < y->occ ? -1 : x->occ > y->occ;
}
#define GROUP_MAX 64
static void match_all(void) {
    if (cur->ndraws > p_order_cap) {
        p_order_cap = cur->cap_draws;
        p_order = realloc(p_order, sizeof *p_order * p_order_cap);
        p_match = realloc(p_match, sizeof *p_match * p_order_cap);
    }
    if (prev->ndraws > p_used_cap) { p_used_cap = prev->cap_draws; p_used = realloc(p_used, p_used_cap); }
    memset(p_used, 0, prev->ndraws);
    for (uint32_t j = 0; j < cur->ndraws; j++) p_order[j] = j, p_match[j] = -1;
    // by identity: the object's draw in prev2 (same per-object buffer addresses) and that draw's partner in prev
    st_id_pairs = st_content_pairs = st_new_objects = st_order_pairs = 0;
    if (order_check < 0) { const char *e = getenv("LARECOMP_INTERP_ORDER_CHECK"); order_check = !e || atoi(e); }
    if (prev2->ndraws && !prev2->overflow && !history_reset) {
        index_prev2();
        static int32_t *inv; static uint32_t inv_cap;  // prev2's draw -> prev's draw paired with it
        if (prev2->ndraws > inv_cap) { inv_cap = prev2->cap_draws; inv = realloc(inv, sizeof *inv * inv_cap); }
        for (uint32_t k = 0; k < prev2->ndraws; k++) inv[k] = -1;
        for (uint32_t k = 0; k < prev->ndraws; k++) if (prev->draws[k].pair >= 0 && (uint32_t)prev->draws[k].pair < prev2->ndraws) inv[prev->draws[k].pair] = (int32_t)k;
        for (uint32_t j = 0; j < cur->ndraws; j++) {
            DrawRec *d = &cur->draws[j];
            if (!d->obj) continue;
            int32_t y = lookup_prev2(d->obj, d->key, d->oocc);
            if (y < 0) continue;
            int32_t x = inv[y];  // prev's draw paired with y
            // (same pass of the object: its occurrence within (obj, key) - a pre-pass / colour-pass crossing made once
            // by a tie would otherwise be carried along the parity chain for ever: flat-lit images every other frame)
            if (x < 0 || p_used[x] || prev->draws[x].key != d->key || prev->draws[x].oocc != d->oocc) continue;
            p_match[j] = x;
            p_used[x] = 1;
            st_id_pairs++;
        }
    }
    qsort(p_order, cur->ndraws, sizeof *p_order, order_cmp);
    static Pair pairs[GROUP_MAX * GROUP_MAX];
    for (uint32_t g = 0; g < cur->ndraws;) {
        uint64_t key = cur->draws[p_order[g]].key;
        uint32_t ge = g + 1;
        while (ge < cur->ndraws && cur->draws[p_order[ge]].key == key) ge++;
        uint32_t ncur = ge - g;
        int32_t cand[GROUP_MAX];
        uint32_t nprev = 0;
        for (; nprev < GROUP_MAX; nprev++) {
            int32_t pj = lookup_prev(key, nprev);
            if (pj < 0) break;
            cand[nprev] = pj;
        }
        // Draws without per-object uniforms (vertex stage, slots 2 and up) are the HUD's and the menus' quads (and a
        // few static full-screen passes): they are not blended at all - an in-between shows them as this frame draws
        // them. Their shared buffers end in unwritten leftovers that differ every frame, so no distance tells them
        // apart, and their order shifts whenever one is added (the Items menu's cursor moving a slot: every later quad
        // paired with its neighbour, the whole grid scrambled for an image). The HUD moving at the game's own 60 Hz is
        // not what the in-betweens are for.
        uint32_t left = 0;  // (draws of the group not paired by identity)
        for (uint32_t k = g; k < ge; k++) left += p_match[p_order[k]] == -1;
        if (object_bytes(&cur->draws[p_order[g]]) < 48) {
            for (uint32_t k = g; k < ge; k++) p_match[p_order[k]] = -2;
        } else if (!left) {
        } else if (nprev == 1 && ncur == 1) {
            if (!p_used[cand[0]]) {
                if (draw_maxdiff(&cur->draws[p_order[g]], &prev->draws[cand[0]]) > cut_threshold()) p_match[p_order[g]] = -3, st_new_objects++;
                else p_match[p_order[g]] = cand[0], p_used[cand[0]] = 1, st_content_pairs++;
            }
        }
        else if (nprev >= GROUP_MAX || ncur > GROUP_MAX) {  // (too many: by occurrence, as before)
            for (uint32_t k = g; k < ge; k++) {
                if (p_match[p_order[k]] != -1) continue;
                int32_t pj = lookup_prev(key, cur->draws[p_order[k]].occ);
                if (pj < 0 || p_used[pj]) continue;
                st_order_pairs++;
                // (the same occurrence is another object when a tile came into view before it: kept at this frame -
                // blended, a ground tile's depth pre-pass was drawn half-way to another tile's place while its
                // colour pass, paired by identity, was not: black bands where the depth-equal test failed)
                if (order_check && draw_maxdiff(&cur->draws[p_order[k]], &prev->draws[pj]) > cut_threshold()) { p_match[p_order[k]] = -3; st_new_objects++; continue; }
                p_match[p_order[k]] = pj, p_used[pj] = 1;
            }
        } else if (nprev) {
            uint32_t np = 0;
            for (uint32_t c = 0; c < ncur; c++)
                for (uint32_t q = 0; q < nprev; q++)
                {   // ties: the same pass of the object first (occurrence within obj + key), then the nearest occurrence
                    const DrawRec *dc = &cur->draws[p_order[g + c]], *dp = &prev->draws[cand[q]];
                    uint32_t dpass = dc->oocc > dp->oocc ? dc->oocc - dp->oocc : dp->oocc - dc->oocc;
                    pairs[np++] = (Pair){draw_distance(dc, dp), (uint16_t)c, (uint16_t)q,
                                         (uint16_t)((dpass > 255 ? 255 : dpass) << 8 | (c > q ? c - q : q - c) % 256)};
                }
            qsort(pairs, np, sizeof *pairs, pair_cmp);
            for (uint32_t i = 0; i < np; i++) {
                uint32_t j = p_order[g + pairs[i].c];
                int32_t pj = cand[pairs[i].p];
                if (p_match[j] != -1 || p_used[pj] || pairs[i].d >= 1e30f) continue;
                if (draw_maxdiff(&cur->draws[j], &prev->draws[pj]) > cut_threshold()) { p_match[j] = -3; st_new_objects++; continue; }  // (another object)
                p_match[j] = pj;
                p_used[pj] = 1;
                st_content_pairs++;
            }
        }
        g = ge;
    }
}

static void frame_reset(Frame *f) {
    f->ndraws = f->ncbs = f->nhandles = f->nsto = f->ndout = 0;
    f->nbytes = 0;
    f->overflow = f->destroyed = false;
}

// ---- objects freed by the game's threads
static pthread_mutex_t free_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t free_gen;
// (stats: the frees by kind, the pools' bytes, and the frames that lost their in-betweens to one - by the kinds freed
// since the frame began)
static uint64_t s_frees[INTERP_FREE_KINDS], s_free_pool_bytes, s_free_cuts[INTERP_FREE_KINDS];
static _Atomic uint32_t free_kinds;
void interp_free_begin(int kind, uint64_t size) {
    if (pthread_mutex_trylock(&free_lock) != 0) {  // (a replay runs: the wait isn't the game's work - frame pacing)
        extern __thread uint64_t g_blocked_ns;
        uint64_t t0 = plat_now_ns();
        pthread_mutex_lock(&free_lock);
        g_blocked_ns += plat_now_ns() - t0;
    }
    atomic_fetch_add(&free_gen, 1);
    s_frees[kind]++;
    if (kind == INTERP_FREE_POOL) s_free_pool_bytes += size;
    atomic_fetch_or(&free_kinds, 1u << kind);
}
void interp_free_end(void) { pthread_mutex_unlock(&free_lock); }
bool interp_replay_lock(void) {
    pthread_mutex_lock(&free_lock);
    if (atomic_load(&free_gen) == cur->free_gen) return true;
    uint32_t k = atomic_load(&free_kinds);
    for (int i = 0; i < INTERP_FREE_KINDS; i++) s_free_cuts[i] += (k >> i) & 1;
    pthread_mutex_unlock(&free_lock);
    return false;
}
void interp_replay_unlock(void) { pthread_mutex_unlock(&free_lock); }

bool interp_list_submitted(uint64_t handle) {
    if (!g_interp_log || g_interp_replaying) return false;
    bool first = !frame_open;
    if (first) {
        frame_open = true;
        memset(occ_keys, 0, sizeof occ_keys);
        cur->free_gen = atomic_load(&free_gen);
        atomic_store(&free_kinds, 0);
    }
    GROW(cur->handles, cur->nhandles, cur->cap_handles, 64);
    cur->handles[cur->nhandles++] = handle;
    return first;
}

void interp_texture_destroyed(void) { if (frame_open) cur->destroyed = true; }

// ---- replay cursor
static uint32_t rp_draw, rp_cb;
static int32_t rp_cur = -1;  // the replayed draw's record (-1: not found: guest memory as it is)
static uint64_t st_replay_misses, st_replay_draws, st_replay_cb_miss;

void interp_draw(uint64_t key, bool depth_only) {
    if (g_interp_replaying) {
        st_replay_draws++;
        rp_cur = -1;
        for (uint32_t j = rp_draw; j < cur->ndraws && j < rp_draw + 8; j++)
            if (cur->draws[j].key == key) { rp_cur = (int32_t)j; rp_draw = j + 1; rp_cb = cur->draws[j].first_cb; break; }
        if (rp_cur < 0) st_replay_misses++;
        return;
    }
    if (!frame_open) return;  // (a list run outside a frame: nothing to track)
    GROW(cur->draws, cur->ndraws, cur->cap_draws, 1024);
    DrawRec *d = &cur->draws[cur->ndraws++];
    d->key = key;
    d->occ = next_occ(key);
    d->depth_only = depth_only;
    d->first_cb = cur->ncbs;
    d->ncb = 0;
}

static int32_t last_cb = -1;  // the copy interp_cbuf logged last (-1: none)
int32_t interp_last_cb(void) { return last_cb; }
const void *interp_tape_cb(int32_t i) {
    if (i < 0 || (uint32_t)i >= cur->ncbs || !can_blend) return NULL;
    const CbRec *c = &cur->cbs[i];
    return c->blend >= 0 ? blend_bytes + c->blend : NULL;
}

const void *interp_cbuf(int stage, int k, int slot, const void *src, uint64_t size, uint64_t used) {
    (void)k;
    last_cb = -1;
    if (g_interp_replaying) {
        if (rp_cur < 0) return src;
        DrawRec *d = &cur->draws[rp_cur];
        if (rp_cb >= d->first_cb + d->ncb) { st_replay_cb_miss++; return src; }
        CbRec *c = &cur->cbs[rp_cb];
        if (c->stage != stage || c->slot != slot || c->size != size) { st_replay_cb_miss++; return src; }
        rp_cb++;
        return c->blend >= 0 ? blend_bytes + c->blend : cur->bytes + c->off;
    }
    if (!frame_open || !cur->ndraws) return src;
    DrawRec *d = &cur->draws[cur->ndraws - 1];
    if (cur->nbytes + size > (256u << 20)) { cur->overflow = true; return src; }
    while (cur->nbytes + size > cur->cap_bytes) {
        cur->cap_bytes = cur->cap_bytes ? cur->cap_bytes * 2 : (8u << 20);
        cur->bytes = realloc(cur->bytes, cur->cap_bytes);
    }
    GROW(cur->cbs, cur->ncbs, cur->cap_cbs, 4096);
    CbRec *c = &cur->cbs[cur->ncbs++];
    c->stage = (uint8_t)stage, c->slot = (uint8_t)slot, c->off = (uint32_t)cur->nbytes, c->size = (uint32_t)size;
    c->used = stage >= 2 ? 0 : (uint32_t)(used && used < size ? used : size);  // (stage 2, 3: a texture entry, 4, 5: the storage addresses bound - never compared)
    c->addr = (uint64_t)(uintptr_t)src;
    c->prev_off = c->blend = c->prev2_off = -1;
    c->snap = 0, c->maxd = -1, c->dup = -1, c->still = 0;
    memcpy(cur->bytes + cur->nbytes, src, size);
    cur->nbytes += (size + 15) & ~15ull;
    d->ncb++;
    last_cb = (int32_t)cur->ncbs - 1;
    return src;
}

// Storage read through gmem (the light lists, a per-frame table): its bytes at execution time, once per range per
// frame (64 KB at most each), and the frame's dispatch outputs. Replays: interp_replays_allowed compares.
void interp_storage(uint64_t addr, uint64_t size) {
    if (g_interp_replaying || !frame_open || !addr || !size) return;
    if (size > 0x10000) size = 0x10000;
    for (uint32_t i = 0; i < cur->nsto; i++) if (cur->sto[i].addr == addr) return;
    if (cur->nbytes + size > (256u << 20)) { cur->overflow = true; return; }
    while (cur->nbytes + size > cur->cap_bytes) { cur->cap_bytes = cur->cap_bytes ? cur->cap_bytes * 2 : (8u << 20); cur->bytes = realloc(cur->bytes, cur->cap_bytes); }
    GROW(cur->sto, cur->nsto, cur->cap_sto, 64);
    cur->sto[cur->nsto++] = (StoRec){addr, size, cur->nbytes};
    memcpy(cur->bytes + cur->nbytes, (const void *)(uintptr_t)addr, size);
    cur->nbytes += (size + 15) & ~15ull;
}
void interp_dispatch_output(uint64_t addr, uint64_t size) {
    if (g_interp_replaying || !frame_open || !addr) return;
    GROW(cur->dout, cur->ndout, cur->cap_dout, 64);
    cur->dout[cur->ndout++].addr = addr, cur->dout[cur->ndout - 1].size = size;
}
static uint64_t s_sto_checks, s_sto_bytes;  // (stats)

// Texture-pool entries: logged like uniform blocks (stage 2 + the stage, slot i, no bytes compared: used = 0) and
// read back from the log by a replay, in the draw's bind order after its uniform blocks.
const void *interp_texentry(int stage, int i, const void *entry, uint32_t size) {
    const void *r = interp_cbuf(2 + stage, 0, i, entry, size, 0);
    return r;
}

// ---- blending
static bool blendable(float a, float b) {
    if (!isfinite(a) || !isfinite(b) || fabsf(a) > 1e6f || fabsf(b) > 1e6f) return false;
    if ((a != 0 && fabsf(a) < 1e-30f) || (b != 0 && fabsf(b) < 1e-30f)) return false;  // (integers: denormals)
    return true;
}
static float cut_threshold(void) {
    static float c = -1;
    if (c < 0) { const char *e = getenv("LARECOMP_INTERP_CUT"); c = e ? (float)atof(e) : 4.0f; }
    return c;
}
static int blend_stages(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("LARECOMP_INTERP_STAGES"); m = e ? atoi(e) : 3; }
    return m;
}

// ---- stats (per log period)
static uint64_t s_frames, s_draws, s_matched, s_changed, s_cut_cbs, s_blended_cbs, s_cuts[9], s_frame_no;
static uint64_t s_shots, s_mids, s_late, s_skipped, s_gave_way, s_no_gpu;
static double s_replay_cpu, s_replay_cpu_max, s_prep, s_L, s_replay_gpu, s_compose_gpu;
static _Atomic uint64_t s_gpu_n, s_compose_n;
static uint64_t s_replays;
static const char *const cut_name[9] = {"level/forced", "no previous frame", "few matches", "jumps", "object freed", "loading/hitch", "camera jump", "game ahead", "storage rewritten"};

static double img_replay_ms = -1, img_compose_ms = -1;  // (moving averages: an in-between's GPU time)
void interp_replay_gpu(double ms) {  // (completion handlers)
    atomic_fetch_add(&s_gpu_n, 1);
    s_replay_gpu += ms;
    img_replay_ms = img_replay_ms < 0 ? ms : img_replay_ms + (ms - img_replay_ms) * 0.1;
}
void interp_compose_gpu(double ms) {
    atomic_fetch_add(&s_compose_n, 1);
    s_compose_gpu += ms;
    img_compose_ms = img_compose_ms < 0 ? ms : img_compose_ms + (ms - img_compose_ms) * 0.1;
}
double interp_image_gpu_ms(void) { return img_replay_ms < 0 ? -1 : img_replay_ms + (img_compose_ms > 0 ? img_compose_ms : 0); }


static void log_cut(int why, const char *more) {
    s_cuts[why]++;
    static int on = -1;
    if (on < 0) on = getenv("LARECOMP_INTERP_LOG_CUTS") != NULL;
    extern const char *game_level_name(void);
    if (on) LOG("[interp-cut] f%llu %s: %s%s\n", (unsigned long long)s_frame_no, game_level_name(), cut_name[why], more ? more : "");
}

void interp_force_cut(void) { force_cut = true; history_reset = true; }

// Dusklight starts a new presentation epoch - no blending, history cleared - on a native gap over 250 ms
// (src/dusk/game_clock.cpp): its sim never runs while it presents, so that is its only race. Ours runs ahead on its
// own thread: the game's uniform blocks are double-buffered by frame parity, so once it is two frames past the frame
// being presented it rewrites that frame's memory, and a replay's vertex data, index data and texture uploads (read
// live, unlike the logged uniforms) tear. Also a present the render thread reached over 100 ms late: a load, a hitch.
bool interp_replays_allowed(uint64_t frame, uint64_t game_frame, uint64_t now) {
    if (game_frame >= frame + 2) { log_cut(7, NULL); return false; }
    uint64_t c = atomic_load(&called[frame & 255]);
    if (c && now > c && now - c > 100000000ull) { log_cut(5, " (present reached late)"); return false; }
    // Storage the frame's gmem draws read, rewritten by the game since (its CPU-written tables; a dispatch's output
    // is left to GPU order): the replays would read the newer data - the village's lighting one image different.
    for (uint32_t i = 0; i < cur->nsto; i++) {
        StoRec *r = &cur->sto[i];
        bool gpu = false;
        for (uint32_t k = 0; k < cur->ndout && !gpu; k++) gpu = r->addr < cur->dout[k].addr + cur->dout[k].size && cur->dout[k].addr < r->addr + r->size;
        if (gpu) continue;
        s_sto_checks++, s_sto_bytes += r->size;
        if (memcmp(cur->bytes + r->off, (const void *)(uintptr_t)r->addr, r->size) != 0) {
            char why[80];
            snprintf(why, sizeof why, " (%llx, %llu bytes)", (unsigned long long)r->addr, (unsigned long long)r->size);
            log_cut(8, why);
            return false;
        }
    }
    return true;
}

// LARECOMP_INTERP_DUMP=<frame>:<path> (research): both frames' draws and uniform copies (with their guest addresses) as
// text at present frames frame .. frame + 2 (<path>.0 .. .2)
static void dump_frames(void) {
    static long df = -1; static char dpath[256];
    if (df < 0) { const char *e = getenv("LARECOMP_INTERP_DUMP"); df = 0; if (e) sscanf(e, "%ld:%255s", &df, dpath); }
    if (df && (long)g_interp_present_frame >= df && (long)g_interp_present_frame < df + 3) {  // (3 frames: N-1 .. N+2)
        char path[300];
        snprintf(path, sizeof path, "%s.%ld", dpath, (long)g_interp_present_frame - df);
        FILE *f = fopen(path, "w");
        for (int which = 0; f && which < 2; which++) {
            Frame *F = which ? cur : prev;
            fprintf(f, "F %s %u\n", which ? "cur" : "prev", F->ndraws);
            for (uint32_t j = 0; j < F->ndraws; j++) {
                DrawRec *d = &F->draws[j];
                fprintf(f, "D %u %016llx %u %u\n", j, (unsigned long long)d->key, d->occ, d->ncb);
                for (uint32_t i = 0; i < d->ncb; i++) {
                    CbRec *c = &F->cbs[d->first_cb + i];
                    fprintf(f, "C %d %d %u %llx", c->stage, c->slot, c->size, (unsigned long long)c->addr);
                    const uint32_t *w = (const uint32_t *)(F->bytes + c->off);
                    for (uint32_t q = 0; q < c->size / 4; q++) fprintf(f, " %08x", w[q]);
                    fprintf(f, "\n");
                }
            }
        }
        if (f) fclose(f);
    }
}

bool interp_frame_end(bool log_next) {
    double t0 = plat_now_ns() / 1e6;
    s_frame_no++;
    bool was_open = frame_open;
    frame_open = false;
    can_blend = false;
    if (!g_interp_log) { g_interp_log = log_next; return false; }
    g_interp_log = log_next;
    for (uint32_t j = 0; j < cur->ndraws; j++) cur->draws[j].pair = -1;  // (set by match_all when the frame is paired)
    if (!was_open || !cur->ndraws || cur->overflow) return false;
    s_frames++;
    if (force_cut) { force_cut = false; log_cut(0, NULL); return false; }
    if (!prev_valid || prev->overflow) { log_cut(1, NULL); return false; }
    index_prev();
    assign_objects(cur);
    dump_frames();
    match_all();
    for (uint32_t j = 0; j < cur->ndraws; j++) cur->draws[j].pair = p_match[j] >= 0 ? p_match[j] : -1;
    history_reset = false;  // (this frame paired without the old chain: from here on it is the chain)
    s_id_pairs += st_id_pairs, s_order_pairs += st_order_pairs, s_content_pairs += st_content_pairs, s_new_objects += st_new_objects;
    float cut = cut_threshold();
    int stages = blend_stages();
    uint64_t matched = 0, changed = 0, cuts = 0;
    for (uint32_t j = 0; j < cur->ndraws; j++) {
        DrawRec *d = &cur->draws[j];
        int32_t pj = p_match[j];
        if (pj == -2 || pj == -3) { matched++; continue; }  // (UI, or an object that just appeared: shown as it is)
        if (pj < 0) continue;
        matched++;
        DrawRec *p = &prev->draws[pj];
        for (uint32_t i = 0; i < d->ncb && i < p->ncb; i++) {
            CbRec *cc = &cur->cbs[d->first_cb + i], *pc = &prev->cbs[p->first_cb + i];
            if (cc->stage != pc->stage || cc->slot != pc->slot || cc->size != pc->size) continue;
            if (cc->stage > 1 || !(stages & (cc->stage == 0 ? 1 : 2))) continue;  // (texture entries are never blended)
            const float *a = (const float *)(prev->bytes + pc->off), *b = (const float *)(cur->bytes + cc->off);
            if (!memcmp(a, b, cc->used)) { cc->maxd = 0; continue; }
            changed++;
            uint32_t nf = cc->used / 4;
            // A jump: an element moved further than the jump distance in one frame - and much further than the buffer
            // moved the frame before. Steady fast motion (a dungeon's room-to-room scroll: its camera moves far each
            // frame, every frame) blends; deciding it per frame by distance alone made such a scroll alternate
            // between blended and snapped in-betweens: flicker.
            float maxd = 0;
            uint32_t q = 0, jq = 0;
            bool far = false;
            for (; q < nf; q++) {
                if (!blendable(a[q], b[q])) continue;
                float d = fabsf(b[q] - a[q]);
                if (d > maxd) maxd = d;
                if (!far && d > cut + 0.05f * fmaxf(fabsf(a[q]), fabsf(b[q]))) far = true, jq = q + 1;
            }
            cc->maxd = maxd;
            {   // LARECOMP_INTERP_LOG_MOVES=<d>: buffers whose elements moved more than d in one frame (research:
                // draws matched to another object of the same mesh)
                static float lm = -2; static int nlm; static long lmf;  // LARECOMP_INTERP_LOG_MOVES_FROM=<frame>
                if (lm < -1) { const char *e = getenv("LARECOMP_INTERP_LOG_MOVES"); lm = e ? (float)atof(e) : -1;
                               e = getenv("LARECOMP_INTERP_LOG_MOVES_FROM"); lmf = e ? atol(e) : 0; }
                if (lm >= 0 && maxd > lm && (long)s_frame_no >= lmf && nlm++ < 40000) {
                    uint32_t mq = 0; for (uint32_t z = 0; z < nf; z++) if (blendable(a[z], b[z]) && fabsf(b[z] - a[z]) == maxd) { mq = z; break; }
                    LOG("[interp-move] f%llu draw %u occ %u key %016llx stage %d slot %d size %u ncb %u: maxd %g at elem %u (%g -> %g) far %d\n",
                        (unsigned long long)s_frame_no, j, d->occ, (unsigned long long)d->key, cc->stage, cc->slot, cc->size, d->ncb, maxd, mq, a[mq], b[mq], far);
                }
            }
            bool jump = far && (pc->maxd < 0 || maxd > 2.5f * pc->maxd + cut);
            q = jq;
            cc->snap = jump;
            if (jump) {
                static int nlog;  // LARECOMP_INTERP_LOG_JUMPS=<frame>: log jumping buffers from that frame on
                static long from = -1;
                if (from < 0) from = getenv("LARECOMP_INTERP_LOG_JUMPS") ? atol(getenv("LARECOMP_INTERP_LOG_JUMPS")) : 0;
                if (from && (long)s_frame_no >= from && nlog++ < 3000)
                    LOG("[interp-jump] f%llu draw %u stage %d slot %d size %u elem %u: %g -> %g\n", (unsigned long long)s_frame_no,
                        j, cc->stage, cc->slot, cc->size, q - 1, a[q - 1], b[q - 1]);
                cuts++;
            }
            cc->prev_off = pc->off;
            {   // the same buffer two frames back, through the partner's own partner (the toggle rule, interp_prepare)
                int32_t y = p->pair;
                if (y >= 0 && (uint32_t)y < prev2->ndraws && i < prev2->draws[y].ncb) {
                    CbRec *p2 = &prev2->cbs[prev2->draws[y].first_cb + i];
                    if (p2->stage == cc->stage && p2->slot == cc->slot && p2->size == cc->size) cc->prev2_off = p2->off;
                }
            }
        }
    }
    {   // One decision per light (Dusklight: the light's view and projection are recorded once and every reader -
        // the caster pass and the receivers' texture matrix - takes the same replacement, src/d/d_drawlist.cpp).
        // The light's block is the shared block (slots 0 and 1) the shadow pass binds; the ground reads a copy of it
        // in the fragment stage. Decided per buffer, one copy could snap while the other blended: the shadow map
        // drawn with one light matrix and read with another (acne, a wedge of shadow). A jump in any copy snaps
        // them all - and only them: the earlier rule took every block of the same slot and size with it, the
        // scene's camera included, so a light's jump put the camera at this frame while the objects blended: the
        // whole image shifted for one in-between (the full-screen flicker). A jump in any other shared block - the
        // camera, a pass - is a camera cut: the frame shows as it is, with no in-betweens (Dusklight's presentation
        // sync: the game marks its camera cuts and the frame is presented at step 1).
        struct { uint8_t slot; uint32_t size; bool snap; } lcls[32];
        int nl = 0;
        bool camera_jump = false;
        for (uint32_t j = 0; j < cur->ndraws; j++) {
            DrawRec *d = &cur->draws[j];
            if (!d->depth_only) continue;
            for (uint32_t i = 0; i < d->ncb; i++) {
                CbRec *cc = &cur->cbs[d->first_cb + i];
                if (cc->stage > 1 || cc->slot > 1 || cc->prev_off < 0) continue;
                int k = 0;
                for (; k < nl; k++) if (lcls[k].slot == cc->slot && lcls[k].size == cc->size) break;
                if (k == nl) { if (nl == 32) continue; lcls[nl].slot = cc->slot, lcls[nl].size = cc->size, lcls[nl].snap = false; nl++; }
                lcls[k].snap |= cc->snap;
            }
        }
        for (uint32_t j = 0; j < cur->ndraws; j++) {  // the receivers: the same block in the fragment stage
            DrawRec *d = &cur->draws[j];
            if (d->depth_only) continue;
            for (uint32_t i = 0; i < d->ncb; i++) {
                CbRec *cc = &cur->cbs[d->first_cb + i];
                if (cc->slot > 1 || cc->prev_off < 0 || cc->stage != 1) continue;
                for (int k = 0; k < nl; k++) if (lcls[k].slot == cc->slot && lcls[k].size == cc->size) { lcls[k].snap |= cc->snap; break; }
            }
        }
        for (uint32_t j = 0; j < cur->ndraws; j++) {
            DrawRec *d = &cur->draws[j];
            for (uint32_t i = 0; i < d->ncb; i++) {
                CbRec *cc = &cur->cbs[d->first_cb + i];
                if (cc->stage > 1 || cc->slot > 1 || cc->prev_off < 0) continue;
                int k = 0;
                bool light = d->depth_only || cc->stage == 1;
                if (light) { for (; k < nl; k++) if (lcls[k].slot == cc->slot && lcls[k].size == cc->size) break; light = k < nl; }
                if (light) { if (lcls[k].snap && !cc->snap) cc->snap = 1, s_class_snaps++; }
                else if (cc->snap) camera_jump = true;
            }
        }
        if (camera_jump) { log_cut(6, NULL); return false; }
    }
    {   // One decision per buffer. Draws that read the same uniforms take the same partner and rules:
        // - an object's own blocks (the vertex stage's slots 2 and up) by address: the same memory with the same
        //   contents (its depth pre-pass, colour pass, shadow);
        // - the shared blocks (slots 0 and 1: the pass's camera, the light) by contents: each pass has its own copy.
        //   The first copy in a paired draw decides, and unpaired draws (a new object, a tile of the ground that
        //   came into view while scrolling) take it too: they stay where they are in the world and the camera moves
        //   them, instead of being drawn with this frame's camera while their depth pre-pass was not.
        // Decided apart, an object's passes drew it in two places: one paired (blended camera), the other not (this
        // frame's camera: a pass whose draw was left as a new object). The colour pass's depth-equal test then failed
        // against the pre-pass's depth and the object showed the cleared target: black silhouettes, and black bands
        // with hatched triangle edges on the ground (the beach on a 5120x1440 window, walking at 30 game frames a
        // second: motion per frame past the new-object distance). LARECOMP_INTERP_UNIFY=0: off.
        static int unify = -1;
        if (unify < 0) { const char *e = getenv("LARECOMP_INTERP_UNIFY"); unify = !e || atoi(e); }
        static uint64_t u_keys[1 << HBITS]; static int32_t u_idx[1 << HBITS];
        memset(u_idx, 0xff, sizeof u_idx);
        for (int pass = 0; pass < 2; pass++)  // (0: the paired draws' shared copies first, 1: everything else)
            for (uint32_t j = 0; j < cur->ndraws; j++) {
                if (p_match[j] == -2) continue;  // (UI: never blended)
                DrawRec *d = &cur->draws[j];
                for (uint32_t q = 0; q < d->ncb; q++) {
                    CbRec *cc = &cur->cbs[d->first_cb + q];
                    if (cc->stage > 1 || !cc->used) continue;
                    bool shared = cc->slot <= 1;
                    if (pass == 0 && !(shared && p_match[j] >= 0)) continue;
                    uint64_t k;
                    if (shared) {
                        const uint64_t *w = (const uint64_t *)(cur->bytes + cc->off);
                        k = 0x9E3779B97F4A7C15ull ^ cc->used;
                        for (uint32_t z = 0; z < cc->used / 8 && z < 16; z++) k = (k ^ w[z]) * 0xff51afd7ed558ccdull, k ^= k >> 29;  // (the head: memcmp settles it)
                        k |= 1;
                    } else k = ((cc->addr * 0x9E3779B97F4A7C15ull) ^ cc->size) & ~1ull;
                    uint32_t h = (uint32_t)(k >> (64 - HBITS));
                    int32_t f = -1;
                    for (int n = 0; n < (1 << HBITS); n++, h = (h + 1) & ((1 << HBITS) - 1)) {
                        if (u_idx[h] < 0) { u_keys[h] = k; u_idx[h] = (int32_t)(d->first_cb + q); break; }
                        CbRec *c0 = &cur->cbs[u_idx[h]];
                        if (u_keys[h] == k && c0->size == cc->size && c0->used == cc->used && (shared || c0->addr == cc->addr) &&
                            !memcmp(cur->bytes + c0->off, cur->bytes + cc->off, cc->used)) { f = u_idx[h]; break; }
                    }
                    if (f < 0 || f == (int32_t)(d->first_cb + q)) continue;
                    CbRec *c0 = &cur->cbs[f];
                    bool same = (c0->prev_off < 0) == (cc->prev_off < 0) && c0->snap == cc->snap && (c0->prev2_off < 0) == (cc->prev2_off < 0) &&
                                (c0->prev_off < 0 || !memcmp(prev->bytes + c0->prev_off, prev->bytes + cc->prev_off, cc->used)) &&
                                (c0->prev2_off < 0 || !memcmp(prev2->bytes + c0->prev2_off, prev2->bytes + cc->prev2_off, cc->used));
                    // (the same reader of the same bytes in all three frames: blended once, interp_prepare)
                    bool twin = c0->stage == cc->stage && c0->slot == cc->slot && c0->size == cc->size;
                    if (same) { if (twin) cc->dup = f; continue; }
                    s_split++;
                    {   static int on = -1, nlog;  // LARECOMP_INTERP_LOG_SPLITS=1: each (research)
                        if (on < 0) on = getenv("LARECOMP_INTERP_LOG_SPLITS") != NULL;
                        if (on && nlog++ < 5000)
                            LOG("[interp-split] frame %llu draw %u (%d) stage %d slot %d size %u: first %s%s, this %s%s\n", (unsigned long long)g_interp_present_frame, j, p_match[j], cc->stage, cc->slot, cc->size,
                                c0->prev_off < 0 ? "unpaired" : "paired", c0->snap ? " snap" : "", cc->prev_off < 0 ? "unpaired" : "paired", cc->snap ? " snap" : "");
                    }
                    if (unify) { cc->prev_off = c0->prev_off, cc->prev2_off = c0->prev2_off, cc->snap = c0->snap; if (twin) cc->dup = f; }
                }
            }
    }
    s_draws += cur->ndraws, s_matched += matched, s_changed += changed, s_cut_cbs += cuts, s_blended_cbs += changed - cuts;
    s_prep += plat_now_ns() / 1e6 - t0;
    char why[96];
    if (matched < cur->ndraws * 8 / 10) {
        snprintf(why, sizeof why, " (%llu of %u draws)", (unsigned long long)matched, cur->ndraws);
        log_cut(2, why);
        return false;
    }
    if (cuts > 8 && cuts * 2 > changed) {  // (most of what moved jumped: a camera cut, a warp)
        snprintf(why, sizeof why, " (%llu of %llu changed buffers)", (unsigned long long)cuts, (unsigned long long)changed);
        log_cut(3, why);
        return false;
    }
    if (cur->destroyed) { log_cut(4, " (a texture)"); return false; }
    can_blend = true;
    return true;
}

// ---- rotations (letsgo-recomp's rule, MIT): three rows of a buffer whose 3x3 part is a rotation with a scale per axis
// in both frames - a model matrix, a bone, a camera - blend along the arc. Element by element, a turn shrinks the
// matrix by cos(angle / 2) and passes through a flat one at 180 degrees, and the per-element rules below (the
// discontinuity rule: the sine terms of a turn change sign) snapped some terms and blended others: a sheared model.
// Link turning round on the spot flickered as a sliver or a skewed model. A turn larger than
// LARECOMP_INTERP_ROT_SWITCH degrees (default 100) in one frame is no motion the in-betweens can show (a facing that
// switched, a new animation's pose): it switches half-way.
static float rot_switch_cos(void) {  // cos(the switch angle / 2): compared with the quaternions' dot product
    static float c = -2;
    if (c < -1) { const char *e = getenv("LARECOMP_INTERP_ROT_SWITCH"); c = cosf((e ? (float)atof(e) : 100.0f) * (float)M_PI / 360.0f); }
    return c;
}
static uint64_t s_rot_blocks, s_rot_switch;  // (stats)
// m (3 rows of 4: the 3x3 part) = R * S, R a rotation, S symmetric (the polar decomposition: Higham's iteration
// X <- (X + X^-T) / 2). A bone carries its parents' stretch turned with it (Link's skeleton: off-axis shear up to
// 0.2 in the hair's bones), so a test for a plain rotation with a scale per axis missed some bones of a skeleton:
// those blended element-wise and the rest along the arc, and the hair between them stretched into a long curve.
// loose: per-object buffers (a model, its bones) take any matrix whose S is no more than `loose` off its diagonal;
// elsewhere only near-plain rotations (0.02) count.
static bool rot_split(const float *m, float loose, float R[3][3], float S[3][3]) {
    double X[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) X[i][j] = m[i * 4 + j];
    double det0 = X[0][0] * (X[1][1] * X[2][2] - X[1][2] * X[2][1]) - X[0][1] * (X[1][0] * X[2][2] - X[1][2] * X[2][0]) + X[0][2] * (X[1][0] * X[2][1] - X[1][1] * X[2][0]);
    double fro = 0;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) fro += X[i][j] * X[i][j];
    if (!(det0 > 1e-3 * pow(fro / 3, 1.5)) || !isfinite(det0)) return false;  // (flat, mirrored or degenerate: element-wise)
    {   // (most are a rotation with a scale per axis: orthogonal columns, R and S at once)
        double n[3], o = 0;
        for (int j = 0; j < 3; j++) n[j] = sqrt(X[0][j] * X[0][j] + X[1][j] * X[1][j] + X[2][j] * X[2][j]);
        for (int j = 0; j < 3; j++)
            for (int k = j + 1; k < 3; k++) o = fmax(o, fabs(X[0][j] * X[0][k] + X[1][j] * X[1][k] + X[2][j] * X[2][k]) / (n[j] * n[k]));
        if (o < 1e-5) {
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) R[i][j] = (float)(X[i][j] / n[j]), S[i][j] = i == j ? (float)n[j] : 0;
            return true;
        }
    }
    for (int it = 0; it < 30; it++) {
        double c[3][3], det = X[0][0] * (X[1][1] * X[2][2] - X[1][2] * X[2][1]) - X[0][1] * (X[1][0] * X[2][2] - X[1][2] * X[2][0]) + X[0][2] * (X[1][0] * X[2][1] - X[1][1] * X[2][0]);
        c[0][0] = X[1][1] * X[2][2] - X[1][2] * X[2][1]; c[0][1] = X[1][2] * X[2][0] - X[1][0] * X[2][2]; c[0][2] = X[1][0] * X[2][1] - X[1][1] * X[2][0];
        c[1][0] = X[0][2] * X[2][1] - X[0][1] * X[2][2]; c[1][1] = X[0][0] * X[2][2] - X[0][2] * X[2][0]; c[1][2] = X[0][1] * X[2][0] - X[0][0] * X[2][1];
        c[2][0] = X[0][1] * X[1][2] - X[0][2] * X[1][1]; c[2][1] = X[0][2] * X[1][0] - X[0][0] * X[1][2]; c[2][2] = X[0][0] * X[1][1] - X[0][1] * X[1][0];
        double dmax = 0;  // (c / det = X^-T: the cofactor matrix)
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { double v = 0.5 * (X[i][j] + c[i][j] / det); dmax = fmax(dmax, fabs(v - X[i][j])); X[i][j] = v; }
        if (dmax < 1e-7) break;
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i][j] = (float)X[i][j];
    float off = 0, diag = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double v = 0;
            for (int k = 0; k < 3; k++) v += X[k][i] * m[k * 4 + j];  // S = R^T M
            S[i][j] = (float)v;
        }
    for (int i = 0; i < 3; i++) { if (!(S[i][i] > 0)) return false; diag = fmaxf(diag, S[i][i]); }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) if (i != j) off = fmaxf(off, fabsf(S[i][j]));
    return off <= loose * diag;
}
static void rot_to_quat(float R[3][3], float q[4]) {  // (w, x, y, z)
    float tr = R[0][0] + R[1][1] + R[2][2];
    if (tr > 0) { float s = sqrtf(tr + 1) * 2; q[0] = s / 4; q[1] = (R[2][1] - R[1][2]) / s; q[2] = (R[0][2] - R[2][0]) / s; q[3] = (R[1][0] - R[0][1]) / s; }
    else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) { float s = sqrtf(1 + R[0][0] - R[1][1] - R[2][2]) * 2; q[0] = (R[2][1] - R[1][2]) / s; q[1] = s / 4; q[2] = (R[0][1] + R[1][0]) / s; q[3] = (R[0][2] + R[2][0]) / s; }
    else if (R[1][1] > R[2][2]) { float s = sqrtf(1 + R[1][1] - R[0][0] - R[2][2]) * 2; q[0] = (R[0][2] - R[2][0]) / s; q[1] = (R[0][1] + R[1][0]) / s; q[2] = s / 4; q[3] = (R[1][2] + R[2][1]) / s; }
    else { float s = sqrtf(1 + R[2][2] - R[0][0] - R[1][1]) * 2; q[0] = (R[1][0] - R[0][1]) / s; q[1] = (R[0][2] + R[2][0]) / s; q[2] = (R[1][2] + R[2][1]) / s; q[3] = s / 4; }
}
static void quat_to_rot(const float q[4], float R[3][3]) {
    float w = q[0], x = q[1], y = q[2], z = q[3];
    R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y - z * w);     R[0][2] = 2 * (x * z + y * w);
    R[1][0] = 2 * (x * y + z * w);     R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z - x * w);
    R[2][0] = 2 * (x * z - y * w);     R[2][1] = 2 * (y * z + x * w);     R[2][2] = 1 - 2 * (x * x + y * y);
}
// A rotation block's plan (once per frame; interp_prepare runs for every image): what rot_apply does at each t.
typedef struct {
    uint32_t cb, row;
    uint8_t kind;  // 1: toggles (this frame's), 2: switches half-way, 3: blends along the arc
    uint8_t screw;  // the translation turns round the axis through c
    float qa[4], qb[4], dot, Sa[3][3], Sb[3][3], Ra[3][3];
    double c[3], up, u[3];
} RotPlan;
// The rotation block at rows a, b (a2: two frames back, or NULL): false when it is no rotation pair (the caller
// blends element-wise). The fourth element of each row is its translation.
static bool rot_plan(const float *a, const float *b, const float *a2, float loose, RotPlan *P) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            if (!blendable(a[i * 4 + j], b[i * 4 + j])) return false;
    float Rb[3][3];
    if (!rot_split(a, loose, P->Ra, P->Sa) || !rot_split(b, loose, Rb, P->Sb)) return false;
    bool toggle = a2;  // (back at its value of two frames ago: alternating, this frame's - the element rule's)
    for (int i = 0; i < 3 && toggle; i++)
        for (int j = 0; j < 3 && toggle; j++) toggle = a2[i * 4 + j] == b[i * 4 + j];
    if (toggle) { P->kind = 1; return true; }
    rot_to_quat(P->Ra, P->qa), rot_to_quat(Rb, P->qb);
    float dot = P->qa[0] * P->qb[0] + P->qa[1] * P->qb[1] + P->qa[2] * P->qb[2] + P->qa[3] * P->qb[3];
    if (dot < 0) { dot = -dot; for (int k = 0; k < 4; k++) P->qb[k] = -P->qb[k]; }  // (the short way round)
    P->dot = dot;
    if (dot < rot_switch_cos()) { P->kind = 2; return true; }  // a switch: the old rotation until half-way, then the new one
    P->kind = 3;
    // The translation (each row's fourth element) moves with the turn, along the arc round the turn's axis (a screw
    // motion: dual-quaternion ScLERP). A bone of a skeleton that turns about the model's centre otherwise cut across
    // the chord while its rotation took the arc: bones far from the centre (the hair, the hat's tip) were pulled
    // inwards by up to 30% at a quarter turn and the mesh between them stretched.
    P->screw = 0;
    const float *qa = P->qa, *qb = P->qb;
    double ta[3] = {a[3], a[7], a[11]}, tb[3] = {b[3], b[7], b[11]};
    double vx = qa[0] * qb[1] - qb[0] * qa[1] - (qb[2] * qa[3] - qb[3] * qa[2]),  // qd = qb * conj(qa): the turn
           vy = qa[0] * qb[2] - qb[0] * qa[2] - (qb[3] * qa[1] - qb[1] * qa[3]),
           vz = qa[0] * qb[3] - qb[0] * qa[3] - (qb[1] * qa[2] - qb[2] * qa[1]);
    double vn = sqrt(vx * vx + vy * vy + vz * vz), th = 2 * atan2(vn, (double)dot);
    if (th > 1e-3) {
        double u[3] = {vx / vn, vy / vn, vz / vn}, Rd[3][3], td[3], up = 0, tp[3], c[3], ct = 1 / tan(th / 2);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) { Rd[i][j] = 0; for (int k = 0; k < 3; k++) Rd[i][j] += (double)Rb[i][k] * P->Ra[j][k]; }
        for (int i = 0; i < 3; i++) td[i] = tb[i] - (Rd[i][0] * ta[0] + Rd[i][1] * ta[1] + Rd[i][2] * ta[2]);
        for (int i = 0; i < 3; i++) up += u[i] * td[i];
        for (int i = 0; i < 3; i++) tp[i] = td[i] - up * u[i];
        c[0] = 0.5 * (tp[0] + ct * (u[1] * tp[2] - u[2] * tp[1]));  // the axis' point: (I - Rd) c = tp
        c[1] = 0.5 * (tp[1] + ct * (u[2] * tp[0] - u[0] * tp[2]));
        c[2] = 0.5 * (tp[2] + ct * (u[0] * tp[1] - u[1] * tp[0]));
        double cn = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]), tn = sqrt(ta[0] * ta[0] + ta[1] * ta[1] + ta[2] * ta[2]) + sqrt(tb[0] * tb[0] + tb[1] * tb[1] + tb[2] * tb[2]);
        if (cn < 100 * (tn + 1)) {  // (a nearly straight path: the axis far away - linear, as it is to float precision)
            P->screw = 1, P->up = up;
            for (int k = 0; k < 3; k++) P->c[k] = c[k], P->u[k] = u[k];
        }
    }
    return true;
}
static void rot_apply(const RotPlan *P, const float *a, const float *b, float t, float *out) {
    s_rot_blocks++;
    if (P->kind == 1 || P->kind == 2) {
        if (P->kind == 2) s_rot_switch++;
        const float *src = P->kind == 2 && t < 0.5f ? a : b;
        for (int i = 0; i < 12; i++) out[i] = src[i];
        return;
    }
    float q[4], wa = 1 - t, wb = t;
    if (P->dot < 0.9995f) {
        float th = acosf(P->dot), sn = sinf(th);
        wa = sinf((1 - t) * th) / sn, wb = sinf(t * th) / sn;
    }
    float n = 0;
    for (int k = 0; k < 4; k++) q[k] = wa * P->qa[k] + wb * P->qb[k], n += q[k] * q[k];
    n = sqrtf(n);
    for (int k = 0; k < 4; k++) q[k] /= n;
    float R[3][3];
    quat_to_rot(q, R);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            float v = 0;
            for (int k = 0; k < 3; k++) v += R[i][k] * (P->Sa[k][j] + (P->Sb[k][j] - P->Sa[k][j]) * t);
            out[i * 4 + j] = v;
        }
    double ta[3] = {a[3], a[7], a[11]}, tt[3];
    for (int i = 0; i < 3; i++) tt[i] = ta[i] + ((double)b[i * 4 + 3] - ta[i]) * t;
    if (P->screw) {
        double Mt[3][3], x[3] = {ta[0] - P->c[0], ta[1] - P->c[1], ta[2] - P->c[2]};  // R(t) * Ra^T: the turn so far
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) { Mt[i][j] = 0; for (int k = 0; k < 3; k++) Mt[i][j] += (double)R[i][k] * P->Ra[j][k]; }
        for (int i = 0; i < 3; i++) tt[i] = Mt[i][0] * x[0] + Mt[i][1] * x[1] + Mt[i][2] * x[2] + P->c[i] + t * P->up * P->u[i];
    }
    for (int i = 0; i < 3; i++) out[i * 4 + 3] = (float)tt[i];
}
static RotPlan *plans; static uint32_t nplans, plans_cap;
static uint64_t frame_serial;  // (interp_frame_rotate's count)
static uint64_t plans_serial = ~0ull;

bool interp_prepare(float t) {
    if (!can_blend) return false;
    bool planned = plans_serial == frame_serial;
    if (!planned) nplans = 0;
    double t0 = plat_now_ns() / 1e6;
    blend_n = 0;
    static int snap_elem0 = -1, reuse = -1;  // LARECOMP_INTERP_REUSE=0: every copy blended (research)
    if (snap_elem0 < 0) { const char *e = getenv("LARECOMP_INTERP_SNAP"); snap_elem0 = e && !strcmp(e, "elem");
                          e = getenv("LARECOMP_INTERP_REUSE"); reuse = !e || atoi(e); }
    for (uint32_t i = 0; i < cur->ncbs; i++) {
        CbRec *cc = &cur->cbs[i];
        cc->blend = -1;
        if (cc->prev_off < 0 || (reuse && cc->dup >= 0)) continue;  // (copies take their twin's below)
        // Unchanged, or jumped (shown whole at this frame's): every rule gives this frame's values - its own copy
        // is bound as it is (the camera blocks of a scene that holds still, most objects' material blocks)
        if (!planned) cc->still = !memcmp(prev->bytes + cc->prev_off, cur->bytes + cc->off, cc->used);
        if (!reuse) cc->still = 0;
        if (reuse && (cc->still || (cc->snap && !snap_elem0))) { s_reused++; continue; }
        while (blend_n + cc->size + 16 > blend_cap) {
            blend_cap = blend_cap ? blend_cap * 2 : (4u << 20);
            blend_bytes = realloc(blend_bytes, blend_cap);
        }
        const float *a = (const float *)(prev->bytes + cc->prev_off), *b = (const float *)(cur->bytes + cc->off);
        float *out = (float *)(blend_bytes + blend_n);
        uint32_t nf = cc->used / 4;
        // A buffer that jumped shows this frame's values whole (letsgo-recomp's rule: a matrix with some elements
        // snapped and others blended shears and shrinks the model; the earlier per-element snap did that).
        // LARECOMP_INTERP_SNAP=elem: the old per-element snap (research).
        static int snap_elem = -1, toggles = -1, rot_on = -1, halfway = -1;
        if (snap_elem < 0) {
            const char *e = getenv("LARECOMP_INTERP_SNAP"); snap_elem = e && !strcmp(e, "elem");
            e = getenv("LARECOMP_INTERP_TOGGLE"); toggles = !e || atoi(e);  // =0: blend toggling values too (research)
            e = getenv("LARECOMP_INTERP_ROT"); rot_on = !e || atoi(e);  // =0: rotations blend element-wise (research)
            e = getenv("LARECOMP_INTERP_HALFWAY"); halfway = !e || atoi(e);  // =0: switched values show this frame's at once
        }
        if (cc->snap && !snap_elem) { memcpy(out, b, cc->size); cc->blend = (int64_t)blend_n; blend_n += (cc->size + 15) & ~15u; continue; }
        float cut = cut_threshold();
        // An element back at its value of two frames ago alternates (dither offsets, frame parity, a jitter) - it is
        // not moving: it keeps this frame's value instead of a midpoint neither frame shows.
        const float *a2 = cc->prev2_off >= 0 ? (const float *)(prev2->bytes + cc->prev2_off) : NULL;
        // rotation blocks first (rows of 4 elements): their 3x3 parts are done, the rest goes through the rules below
        static uint8_t *done; static uint32_t done_cap;
        if (nf > done_cap) { done_cap = nf * 2; done = realloc(done, done_cap); }
        memset(done, 0, nf);
        if (rot_on) {
            if (!planned) {  // (the frame's first image: find the blocks; the later images reuse the plans)
                float loose = cc->stage == 0 && cc->slot >= 2 ? 0.35f : 0.02f;
                cc->plan_first = nplans, cc->plan_switch = 0;
                for (uint32_t r = 0; r + 3 <= nf / 4;) {
                    const float *ra = a + r * 4, *rb = b + r * 4;
                    if (nplans == plans_cap) { plans_cap = plans_cap ? plans_cap * 2 : 4096; plans = realloc(plans, sizeof *plans * plans_cap); }
                    RotPlan *P = &plans[nplans];
                    if (!memcmp(ra, rb, 48) || !rot_plan(ra, rb, a2 ? a2 + r * 4 : NULL, loose, P)) { r++; continue; }
                    P->cb = i, P->row = r;
                    cc->plan_switch |= P->kind == 2;
                    nplans++;
                    r += 3;
                }
                cc->plan_n = nplans - cc->plan_first;
            }
            for (uint32_t k = cc->plan_first; k < cc->plan_first + cc->plan_n; k++) {
                const RotPlan *P = &plans[k];
                rot_apply(P, a + P->row * 4, b + P->row * 4, t, out + P->row * 4);
                memset(done + P->row * 4, 1, 12);
            }
            // One turn past the switch angle switches the whole buffer half-way: a skeleton's bones together (Link
            // turning round: his body's bones switched while the hat's and hair's, turned a little less, blended
            // through a quarter turn - the hat stretched into a long curve for an image)
            if (cc->plan_switch) {
                memcpy(out, t < 0.5f ? a : b, nf * 4);
                memcpy((uint8_t *)out + nf * 4, (const uint8_t *)b + nf * 4, cc->size - nf * 4);
                cc->blend = (int64_t)blend_n;
                blend_n += (cc->size + 15) & ~15u;
                continue;
            }
        }
        // A fragment stage buffer with an element exactly zero in one frame and not in the other is a list whose
        // slots switch on or off - the game's light list, which fills its slots in a different order as Link walks
        // (a slot held light A, was empty, then held light B; light A moved to another slot) - not motion: the whole
        // buffer shows this frame's list. Blended per element, an in-between drew lights at half strength or not at
        // all: the Angler's Tunnel water darker for single images, now and then (2026-11-03). The vertex stage keeps
        // blending (a position or matrix element can pass through an exact zero while it moves); values near zero
        // (denormal leftovers, 1e-15) don't count, nor a value back at its value of two frames ago (the toggle
        // rule shows this frame's: in Mabe Village 16-30 buffers an image, which then kept blending their other values).
        // LARECOMP_INTERP_ZERO_SWITCH=0: blended (research).
        static int zero_switch = -1;
        if (zero_switch < 0) { const char *e = getenv("LARECOMP_INTERP_ZERO_SWITCH"); zero_switch = !e || atoi(e); }
        // The light lists themselves (fragment stage, 2 KB and over) come in two copies the game writes in turn, each
        // listing the lights in its own order: paired slot by slot with the other copy, a light was blended with
        // another light (the values both copies hold alike show up as toggling elements). Any element toggling: the
        // whole list is this frame's.
        bool onoff = false;
        if (zero_switch && cc->stage == 1)
            for (uint32_t q = 0; q < nf && !onoff; q++) {
                bool tog = toggles && a2 && a2[q] == b[q] && a[q] != b[q];  // (back at its value of two frames ago)
                onoff = !done[q] && ((tog && cc->size >= 2048) ||
                                     ((a[q] == 0) != (b[q] == 0) && fmaxf(fabsf(a[q]), fabsf(b[q])) > 1e-6f && !tog));
            }
        s_onoff += onoff;
        for (uint32_t q = 0; q < nf; q++) {
            if (done[q]) continue;
            if (onoff) { out[q] = b[q]; continue; }
            float av = a[q], bv = b[q], d = bv - av;
            bool toggle = toggles && a2 && a2[q] == bv && av != bv;
            s_toggled += toggle;
            if (toggle) {  // LARECOMP_INTERP_LOG_TOGGLES=1 (debug): each toggling element (at most 400 lines a second)
                static int lt = -1, nl;
                static uint64_t sec;
                if (lt < 0) lt = getenv("LARECOMP_INTERP_LOG_TOGGLES") && atoi(getenv("LARECOMP_INTERP_LOG_TOGGLES"));
                uint64_t now = plat_now_ns() / 1000000000ull;
                if (now != sec) sec = now, nl = 0;
                if (lt && nl++ < 400)
                    LOG("[interp-toggle] f%llu t %.2f cb %u stage %d slot %d size %u addr %llx elem %u: %g, %g, %g\n",
                        (unsigned long long)s_frame_no, t, i, cc->stage, cc->slot, cc->size, (unsigned long long)cc->addr, q,
                        a2[q], av, bv);
            }
            if (!blendable(av, bv) || toggle || (cc->snap && fabsf(d) > cut + 0.05f * fmaxf(fabsf(av), fabsf(bv)))) { out[q] = bv; continue; }
            // Values that are whole numbers in both frames and changed by one or more are a selection, not motion
            // (a texture's frame, a mode): this frame's - unless the value took the same step the frame before (a
            // frame counter, 1127 -> 1128 every frame: time, blended). Dusklight never blends a texture-swap
            // animation (src/dusk/interp/material.cpp: a texNo track that changed is left at the sim frame's value).
            // A switch shows the previous frame's value until half-way and this frame's after (letsgo-recomp: values
            // that held still, then changed, switch half-way): the change lands between the two frames, as it did.
            static long log_rules = -1;  // LARECOMP_INTERP_LOG_RULES=<frame> (research): the rules' decisions for 5 frames from there
            if (log_rules < 0) { const char *e = getenv("LARECOMP_INTERP_LOG_RULES"); log_rules = e ? atol(e) : 0; }
            bool lr = log_rules && (long)s_frame_no >= log_rules && (long)s_frame_no < log_rules + 5;
            if (av == rintf(av) && bv == rintf(bv) && fabsf(d) >= 1.0f && (!a2 || (a2[q] == rintf(a2[q]) && av - a2[q] != d))) {
                if (lr) LOG("[interp-rule] f%llu whole-number cb %u stage %d slot %d elem %u: %g, %g -> %g\n", (unsigned long long)s_frame_no, i, cc->stage, cc->slot, q, a2 ? a2[q] : NAN, av, bv);
                out[q] = halfway && t < 0.5f ? av : bv; s_integral++; continue;
            }
            // A texture translation that scrolls a little every frame and wraps (0.98 -> 0.01) changed by nearly a
            // whole tile: blended, the wrap's midpoint is on screen for an image (the water's ripples jumping back).
            // When the change less a whole number of tiles is the step the value took the frame before, it is a
            // wrap: blend along the short way, which texture addressing wraps back (Dusklight's
            // sample_texture_translation, src/dusk/interp/material.cpp: a whole-tile jump between adjacent keys
            // continues the translation linearly).
            // (only values that look like texture coordinates: inside (-1, 1) and not a whole number at either end -
            // a lighting weight that faded 0.1, 0.05 and then jumped to 1 is no wrap, though its wrapped step matches)
            if (a2 && fabsf(d) >= 0.9f && fabsf(av) < 1 && fabsf(bv) < 1 && av != rintf(av) && bv != rintf(bv)) {
                float k = rintf(d), dw = d - k, s = av - a2[q];
                if (k != 0 && dw != 0 && fabsf(dw) < 0.5f && fabsf(s - dw) <= 0.1f * fabsf(dw) + 1e-5f) {
                    if (lr) LOG("[interp-rule] f%llu wrap cb %u stage %d slot %d elem %u: %g, %g -> %g\n", (unsigned long long)s_frame_no, i, cc->stage, cc->slot, q, a2[q], av, bv);
                    out[q] = av + dw * t; s_wraps++; continue;
                }
            }
            // A discontinuity: a change over half the value's size and over eight times the step it took the frame
            // before (a lighting weight jumping 0.05 -> 1, Link's facing turning in one frame) is a switch, not motion:
            // switched half-way (above), as Dusklight shows an animation that was not continuous this tick unblended
            // (src/dusk/interp/material.cpp, Update::~Update's `continuous`). Blended, the in-between showed a
            // state neither frame has (the village 5% darker for one image).
            if (a2 && fabsf(d) > 0.5f * fmaxf(fabsf(av), fabsf(bv)) && fabsf(d) > 8.0f * fabsf(av - a2[q]) + 1e-4f) {
                if (lr) LOG("[interp-rule] f%llu discontinuity cb %u stage %d slot %d elem %u: %g, %g -> %g\n", (unsigned long long)s_frame_no, i, cc->stage, cc->slot, q, a2[q], av, bv);
                out[q] = halfway && t < 0.5f ? av : bv; s_discont++; continue;
            }
            out[q] = av + d * t;
        }
        {   // LARECOMP_INTERP_LOG_ONOFF=1 (debug): elements of big buffers (light lists) that went from zero to a value or
            // back and did not show this frame's value (at most 400 lines a second)
            static int lo = -1, nl;
            static uint64_t sec;
            if (lo < 0) lo = getenv("LARECOMP_INTERP_LOG_ONOFF") && atoi(getenv("LARECOMP_INTERP_LOG_ONOFF"));
            if (lo && cc->size >= 4096) {
                uint64_t now = plat_now_ns() / 1000000000ull;
                if (now != sec) sec = now, nl = 0;
                for (uint32_t q = 0; q < nf && nl < 400; q++)
                    if ((a[q] == 0) != (b[q] == 0) && out[q] != b[q]) {
                        nl++;
                        LOG("[interp-onoff] f%llu t %.2f cb %u stage %d slot %d addr %llx elem %u: %g, %g, %g -> %g\n",
                            (unsigned long long)s_frame_no, t, i, cc->stage, cc->slot, (unsigned long long)cc->addr, q,
                            a2 ? a2[q] : NAN, a[q], b[q], out[q]);
                    }
            }
        }
        memcpy((uint8_t *)out + nf * 4, (const uint8_t *)b + nf * 4, cc->size - nf * 4);
        cc->blend = (int64_t)blend_n;
        blend_n += (cc->size + 15) & ~15u;
    }
    for (uint32_t i = 0; i < cur->ncbs; i++) {
        CbRec *cc = &cur->cbs[i];
        if (reuse && cc->dup >= 0 && cc->prev_off >= 0) cc->blend = cur->cbs[cc->dup].blend, s_reused++;
    }
    plans_serial = frame_serial;
    s_prep += plat_now_ns() / 1e6 - t0;
    return true;
}

uint32_t interp_handles(const uint64_t **out) { *out = cur->handles; return cur->nhandles; }

// ---- replays (the pacer: rqueue.c takes the time out of the game thread's busy time while it waited for one)
static _Atomic uint64_t rp_total_ns, rp_since;
static uint64_t rp_salt, rp_t0;
uint64_t interp_replay_ns(uint64_t now) {
    uint64_t since = atomic_load(&rp_since), total = atomic_load(&rp_total_ns);
    return total + (since && now > since ? now - since : 0);
}
// (per-frame caches only - targets cleared at first use, the scene camera: the storage-snapshot table and texture
// uploads never run in a replay, nothing stateful may key on this frame number there)
uint64_t interp_replay_salt(void) { return g_interp_replaying ? rp_salt : 0; }

void interp_replay_begin(void) {
    g_interp_replaying = true;
    rp_draw = rp_cb = 0;
    rp_cur = -1;
    rp_salt += 1ull << 40;
    rp_t0 = plat_now_ns();
    atomic_store(&rp_since, rp_t0);
}

void interp_replay_end(void) {
    if (!g_interp_replaying) return;
    g_interp_replaying = false;
    uint64_t d = plat_now_ns() - rp_t0;
    atomic_fetch_add(&rp_total_ns, d);
    atomic_store(&rp_since, 0);
    s_replays++;
    s_replay_cpu += d / 1e6;
    if (d / 1e6 > s_replay_cpu_max) s_replay_cpu_max = d / 1e6;
}

uint64_t interp_frame_serial(void) { return frame_serial; }
void interp_frame_rotate(void) {
    frame_serial++;
    Frame *t = prev2; prev2 = prev; prev = cur; cur = t;
    prev_valid = prev->ndraws > 0 && !prev->overflow;
    frame_reset(cur);
    for (uint32_t i = 0; i < prev->ncbs; i++) prev->cbs[i].prev_off = prev->cbs[i].blend = -1;
}

// ---- the game's timeline
static _Atomic uint64_t stamps[256];
void interp_stamp(uint64_t frame, uint64_t vsync_ns, uint64_t call_ns) {
    atomic_store(&stamps[frame & 255], vsync_ns);
    atomic_store(&called[frame & 255], call_ns);
}

// Latency (LARECOMP_INTERP_LOG period, with the option on or off): from the game's present call to its frame's own
// image on screen (presented handlers), and to the render thread reaching that present.
static pthread_mutex_t lat_mx = PTHREAD_MUTEX_INITIALIZER;
static double lat_sum, lat_max, lag_sum, lat_t0;
static uint64_t lat_n, lag_n;
void interp_latency_lag(uint64_t frame, uint64_t now) {
    uint64_t c = atomic_load(&called[frame & 255]);
    if (!c || now < c) return;
    pthread_mutex_lock(&lat_mx);
    lag_sum += (now - c) / 1e6, lag_n++;
    pthread_mutex_unlock(&lat_mx);
}
void interp_latency_shown(uint64_t frame, double presented_s, bool interp) {
    uint64_t c = atomic_load(&called[frame & 255]);
    if (!c || presented_s <= 0) return;
    double ms = presented_s * 1e3 - c / 1e6;
    if (ms < 0 || ms > 500) return;
    pthread_mutex_lock(&lat_mx);
    lat_sum += ms, lat_n++;
    if (ms > lat_max) lat_max = ms;
    double now = presented_s;
    if (!lat_t0) lat_t0 = now;
    static double every = -1;
    if (every < 0) { const char *e = getenv("LARECOMP_INTERP_LOG"); every = e ? atof(e) : 2.0; }
    if (every > 0 && now - lat_t0 >= every) {
        LOG("[latency] frame interpolation %s: game present -> its image on screen %.1f ms avg, %.1f max (render thread "
            "reached the present after %.1f ms)\n", interp ? "on" : "off", lat_sum / lat_n, lat_max, lag_n ? lag_sum / lag_n : 0);
        lat_sum = lat_max = lag_sum = 0, lat_n = lag_n = 0, lat_t0 = now;
    }
    pthread_mutex_unlock(&lat_mx);
}
uint64_t interp_stamp_of(uint64_t frame) { return atomic_load(&stamps[frame & 255]); }

// ---- scheduling
// The images of the game interval (s0, s1] are shown at the display's vsyncs in (s0 + L, s1 + L]: L is the latency
// behind the game's timeline, as small as lets the images be rendered before they're due (the render thread reaches
// a frame's present some time after the game queued it; the in-betweens of (s0, s1] can only start then). It goes up at
// once when needed and down only after two seconds with room to spare. When the display's rate is a multiple of the
// game's 60 Hz, L also puts the game's own frames exactly on vsyncs (no replay needed for them).
static double sched_L, win_need[120], last_at;
static int win_i, win_n, calm;
static double fmod_pos(double a, double m) { double r = fmod(a, m); return r < 0 ? r + m : r; }

int interp_schedule(uint64_t s0u, uint64_t s1u, uint64_t nowu, uint64_t phaseu, uint64_t periodu, uint64_t leadu, bool blend,
                    InterpShot *out, int max) {
    double s0 = (double)s0u, s1 = (double)s1u, now = (double)nowu + (double)leadu, phase = (double)phaseu, P = periodu ? (double)periodu : 16666667.0;
    double D = s1 > s0 ? s1 - s0 : 16666667.0;
    if (s1 <= s0) s0 = s1 - D;
    if (D > 100e6) { blend = false; log_cut(5, NULL); }
    double per_image = 2.5e6;  // (render thread + GPU per in-between, generous: M3 Max ~1.5 + ~1.5 ms at 1080p)
    // (now: when an image made at once would be needed by the presenter; the first in-between can be due right
    // after s0 + L)
    static int extrap = -1;  // LARECOMP_INTERP_MODE=extrapolate: the frame at once, images after it from its motion
    if (extrap < 0) { const char *e = getenv("LARECOMP_INTERP_MODE"); extrap = e && !strcmp(e, "extrapolate"); }
    double need = blend && !extrap ? now + 1e6 + per_image - s0 : now + 2e6 - s1;
    win_need[win_i] = need;
    win_i = (win_i + 1) % 120;
    if (win_n < 120) win_n++;
    double wmax = need;
    for (int i = 0; i < win_n; i++) if (win_need[i] > wmax) wmax = win_need[i];
    int ratio = (int)lround(16666667.0 / P);
    bool aligned = ratio >= 1 && fabs(ratio * P - 16666667.0) < 0.02 * P;
    bool reset = false;
    if (!sched_L || need > sched_L) sched_L = need + 1e6, reset = true;  // (up: at once, with 1 ms to spare)
    else if (sched_L > wmax + P + 2e6) {
        if (++calm >= 120) sched_L = wmax + 1e6, reset = true, calm = 0;  // (down: after 2 s with room)
    } else calm = 0;
    if (aligned) {  // s1 + L on a vsync
        double off = fmod_pos(s1 + sched_L - phase, P);
        if (reset) sched_L += off > 0 ? P - off : 0;
        else if (off > 0.1 * P && off < 0.9 * P) sched_L += P - off;  // (the two clocks drifted)
        else if (off >= 0.9 * P) sched_L += P - off;
        else sched_L -= off;
    }
    s_L += sched_L / 1e6;
    double start = s0 + sched_L, end = s1 + sched_L;
    if (extrap) {  // own frame on the first vsync at s1 + L, then t = 1 .. 2 on the vsyncs until the next one is due
        int n = 0;
        double k = ceil((end - 0.25 * P - phase) / P), own = phase + k * P;
        out[n++] = (InterpShot){(uint64_t)own, 1.0f};
        for (double V = own + P; blend && V < own + D - 0.25 * P && n < max; V += P) {
            if (V < now + 1e6) { s_late++; continue; }
            out[n++] = (InterpShot){(uint64_t)V, (float)(1.0 + (V - own) / D)};
        }
        s_shots += (uint64_t)n;
        s_mids += (uint64_t)(n - 1);
        return n;
    }
    if (last_at > start) start = last_at;  // (an image already placed there: the previous interval's own frame)
    int n = 0;
    double snap = 0.25 * P;
    double k = ceil((start + 0.5 * P - phase) / P - 0.5);  // first vsync after start (half a vsync of slack)
    bool own = false;
    for (double V = phase + k * P; V <= end + snap && n < max; V += P) {
        if (V <= start + 0.1 * P) continue;
        double t = (V - sched_L - s0) / D;
        if (V >= end - snap) {  // the game's own frame
            out[n++] = (InterpShot){(uint64_t)V, 1.0f};
            own = true;
            last_at = V;
            break;
        }
        if (!blend) continue;
        if (V < now + 1e6) { s_late++; continue; }  // (too late to render: the display shows what it has)
        out[n++] = (InterpShot){(uint64_t)V, (float)t};
    }
    if (!own && n < max) {
        out[n++] = (InterpShot){(uint64_t)end, 1.0f};
        last_at = end;
    }
    s_shots += (uint64_t)n;
    for (int i = 0; i < n; i++) s_mids += out[i].t < 1.0f;
    return n;
}

void interp_note_skipped(void) { s_skipped++; }
void interp_note_given_way(void) { s_gave_way++; }
void interp_note_no_gpu(void) { s_no_gpu++; }
void interp_note_images(int mids) { s_shots += (uint64_t)(1 + mids), s_mids += (uint64_t)mids; }

void interp_log_stats(void) {
    static double every = -1, last;
    if (every < 0) { const char *e = getenv("LARECOMP_INTERP_LOG"); every = e ? atof(e) : 2.0; }
    double now = plat_now_ns() / 1e9;
    if (every <= 0 || now - last < every) return;
    double dt = last ? now - last : every;
    last = now;
    if (!s_frames) return;
    extern const char *game_level_name(void);
    double n = (double)s_frames;
    uint64_t gn = atomic_exchange(&s_gpu_n, 0), cn = atomic_exchange(&s_compose_n, 0);
    char cuts[200] = "";
    int o = 0;
    for (int i = 0; i < 9; i++)
        if (s_cuts[i]) o += snprintf(cuts + o, sizeof cuts - (size_t)o, " %s %llu", cut_name[i], (unsigned long long)s_cuts[i]);
    LOG("[interp] %s: %.1f game frames/s, draws %.0f/frame matched %.1f%% (by object %.0f/f, by content %.0f/f, by order %.1f/f, new objects %.1f/f), buffers blended %.0f/f jumps %.0f/f (+%.0f/f with their class), toggling elements %.0f/image, whole-number %.0f, wraps %.1f, discontinuities %.1f, lists switched %.1f, rotations %.0f (switched %.1f), split buffers %.1f/f, reused %.0f/image | images "
        "%.1f/s planned (in-between %.1f/s), late %llu, skipped %llu, gave way %llu, no GPU time %llu | replay cpu %.2f ms avg %.2f max, gpu %.2f ms; "
        "present pass gpu %.2f ms; prep %.2f ms/f | latency %.1f ms | cuts:%s\n",
        game_level_name(), n / dt, s_draws / n, s_draws ? 100.0 * s_matched / s_draws : 0, s_id_pairs / n, s_content_pairs / n, s_order_pairs / n, s_new_objects / n, s_blended_cbs / n, s_cut_cbs / n, s_class_snaps / n, s_replays ? (double)s_toggled / s_replays : 0, s_replays ? (double)s_integral / s_replays : 0, s_replays ? (double)s_wraps / s_replays : 0, s_replays ? (double)s_discont / s_replays : 0, s_replays ? (double)s_onoff / s_replays : 0,
        s_replays ? (double)s_rot_blocks / s_replays : 0, s_replays ? (double)s_rot_switch / s_replays : 0, (double)s_split / n, s_replays ? (double)s_reused / s_replays : 0,
        s_shots / dt, s_mids / dt, (unsigned long long)s_late, (unsigned long long)s_skipped, (unsigned long long)s_gave_way, (unsigned long long)s_no_gpu,
        s_replays ? s_replay_cpu / s_replays : 0, s_replay_cpu_max, gn ? s_replay_gpu / gn : 0, cn ? s_compose_gpu / cn : 0,
        s_prep / n, s_frames ? s_L / n : 0, o ? cuts : " none");
    {   extern void renderer_interp_geo_stats(uint64_t *binds, uint64_t *differed, uint64_t *missed);
        uint64_t gb, gd, gm;
        renderer_interp_geo_stats(&gb, &gd, &gm);
        if (gb || gm) LOG("[interp] replay geometry: %llu binds from the frame's copies, %llu of them rewritten live since, %llu not found\n",
                          (unsigned long long)gb, (unsigned long long)gd, (unsigned long long)gm);
    }
    if (s_frees[0] || s_frees[1] || s_frees[2])
        LOG("[interp] freed by the game: pools %llu (%.1f MB), samplers %llu, programs %llu | frames without in-betweens after: a pool %llu, a sampler %llu, a program %llu\n",
            (unsigned long long)s_frees[0], s_free_pool_bytes / 1048576.0, (unsigned long long)s_frees[1], (unsigned long long)s_frees[2],
            (unsigned long long)s_free_cuts[0], (unsigned long long)s_free_cuts[1], (unsigned long long)s_free_cuts[2]);
    memset(s_frees, 0, sizeof s_frees), memset(s_free_cuts, 0, sizeof s_free_cuts), s_free_pool_bytes = 0;
    if (st_replay_misses || st_replay_cb_miss)
        LOG("[interp] replay draw misses %llu/%llu, uniform misses %llu\n", (unsigned long long)st_replay_misses,
            (unsigned long long)st_replay_draws, (unsigned long long)st_replay_cb_miss);
    s_frames = s_draws = s_matched = s_changed = s_cut_cbs = s_blended_cbs = s_id_pairs = s_order_pairs = s_content_pairs = s_new_objects = s_class_snaps = s_toggled = s_integral = s_wraps = s_discont = s_onoff = s_rot_blocks = s_rot_switch = s_split = s_reused = 0;
    memset(s_cuts, 0, sizeof s_cuts);
    s_shots = s_mids = s_late = s_skipped = s_gave_way = s_no_gpu = s_replays = 0;
    s_replay_cpu = s_replay_cpu_max = s_prep = s_L = s_replay_gpu = s_compose_gpu = 0;
    st_replay_misses = st_replay_draws = st_replay_cb_miss = 0;
}
