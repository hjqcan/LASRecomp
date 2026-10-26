"""CPU / GPU overlap (exec.c): the deferred frame fence and the storage snapshot state machine. No GPU is needed.

Extracts overlap_defer (the SyncWait decision) and the snap_storage block from runtime/nvn/vulkan/exec.c by their
markers, compiles each with stubs, and checks the sequences the design relies on (LAS-reports/vk_overlap_design.md).
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
defer = source[source.index('static uint64_t overlap_defer('):source.index('\nuint64_t renderer_fence_seq(')]
snap = source[source.index('#define SNAP_HALF'):source.index('// (debug) LARECOMP_LOG_BINDS=1:')]
gmap = source[source.index('// ---- gmap_build (tools/test_vulkan_overlap.py'):source.index('// ---- end gmap_build')]
decide = source[source.index('#define VK_OVERLAP_DEFAULT'):source.index('static _Atomic uint64_t g_last_gpu_frame_ticks;')]
assert 'VK_OVERLAP_DEFAULT 1' in decide, 'the default is on where the write watch is available (Beta 5)'
assert 'static void snap_ring_import_tail(void);' in snap

defer_harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
''' + defer + r'''
// The game's frame loop: SyncWait on each frame's fence, each a new submission; `done` is what the GPU completed.
static uint64_t d;  // deferred_seq
int main(void) {
    // frame 1 submitted, nothing done: nothing was deferred before, so return at once and defer 1
    assert(overlap_defer(1, 0, &d) == 0 && d == 1);
    // frame 2 submitted, 1 not done yet: wait for 1, defer 2
    assert(overlap_defer(2, 0, &d) == 1 && d == 2);
    // frame 3, 2 done already (a fast GPU): no wait, nothing deferred any more
    assert(overlap_defer(3, 3, &d) == 0 && d == 0);
    // frame 4, nothing newer done: deferred
    assert(overlap_defer(4, 3, &d) == 0 && d == 4);
    // frame 5, 4 done meanwhile: no wait (4 was complete: a deferred fence already complete is never waited for)
    assert(overlap_defer(5, 4, &d) == 0 && d == 5);
    // the same fence again (the game waits twice): a real wait, nothing deferred after it
    assert(overlap_defer(5, 4, &d) == 5 && d == 0);
    // an older fence than the deferred one: waited for, the deferred one stays
    assert(overlap_defer(7, 5, &d) == 0 && d == 7);
    assert(overlap_defer(6, 5, &d) == 6 && d == 7);
    // the deferred one once done: cleared
    assert(overlap_defer(7, 7, &d) == 0 && d == 0);
    // a completed older fence while a newer one is deferred: the deferral stays (seq < prev)
    assert(overlap_defer(9, 7, &d) == 0 && d == 9);
    assert(overlap_defer(8, 8, &d) == 0 && d == 9);
    assert(overlap_defer(10, 8, &d) == 9 && d == 10);
    // at most two frames queued: with the GPU never finishing, each call waits for the previous frame
    d = 0;
    uint64_t done = 0;
    for (uint64_t f = 20; f < 40; f++) {
        uint64_t w = overlap_defer(f, done, &d);
        assert(w == (f == 20 ? 0 : f - 1) && d == f);
        if (w) done = w;  // (the wait completes it)
    }
    puts("PASS: overlap_defer - first fence, defer / wait-previous, done fences, repeats, older fences, two in flight");
}
'''

# overlap_on: the switch, the default, and the automatic off without page write tracking
decide_harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HEAP_BASE 0x7240000000ull
#define LOG(...) fprintf(stderr, __VA_ARGS__)
static int tracking = 1;
static int pages_cpu_written(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    assert(addr == HEAP_BASE && size == 4096 && !since); *now = 1; return tracking ? 0 : -1;
}
static const char *heap_tracking_reason(void) { return "the guest heap is driver memory (dma-buf), which the OS can't track"; }
''' + decide + r'''
int main(int argc, char **argv) {
    assert(argc == 3);
    tracking = !strcmp(argv[1], "tracked");
    bool want = !strcmp(argv[2], "on");
    assert(overlap_on() == want && overlap_on() == want);  // (decided once)
    printf("PASS: overlap %s, tracking %s, LARECOMP_GPU_OVERLAP=%s, LARECOMP_OVERLAP_WATCH=%s\n", want ? "on" : "off", tracking ? "yes" : "no",
           getenv("LARECOMP_GPU_OVERLAP") ? getenv("LARECOMP_GPU_OVERLAP") : "(unset)",
           getenv("LARECOMP_OVERLAP_WATCH") ? getenv("LARECOMP_OVERLAP_WATCH") : "(unset)");
}
'''

snap_harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct { uint64_t addr, size; } Range;
static struct { Range sbuf[6][16]; } st;
static uint8_t *heap_mem;
#define HEAP_MAX (64ull << 20)
#define HEAP_BASE ((uint64_t)(uintptr_t)heap_mem)
#define LOG(...) fprintf(stderr, __VA_ARGS__)
static uint64_t heap_size = 16ull << 20;  // (the test ranges below sit under 8 MB; the ring follows at 16 MB)
static uint64_t kernel_heap_size(void) { return heap_size; }
static unsigned commits;
static void plat_commit(uint64_t addr, uint64_t size) { assert(addr == HEAP_BASE + heap_size && size == (16ull << 20)); commits++; }
// The OS's page write tracking (exec.c pages_cpu_written over plat_written_pages): 4 KB dirty bits the test sets
// as "the CPU wrote here", reset by a look, and the shared page generations a look stamps
static int track_off;  // (no tracking: -1)
static uint8_t page_dirty[HEAP_MAX / 4096];
static uint32_t page_gen[HEAP_MAX / 4096], pgen = 1;
static unsigned looks;
static uint64_t race_at, race_size, race_poke;  // (W14)
static void cpu_wrote(uint64_t addr) { page_dirty[(addr - HEAP_BASE) / 4096] = 1; }
static int pages_cpu_written(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    if (track_off) return -1;
    assert(addr >= HEAP_BASE && addr - HEAP_BASE + size <= HEAP_MAX);
    looks++;
    uint64_t lo = (addr - HEAP_BASE) / 4096, hi = (addr - HEAP_BASE + size + 4095) / 4096;
    uint32_t g = ++pgen;
    int w = 0;
    for (uint64_t p = lo; p < hi; p++) {
        if (page_dirty[p]) { page_dirty[p] = 0; page_gen[p] = g; }
        if (page_gen[p] > since) w = 1;
    }
    *now = g;
    if (race_at && addr == race_at && size == race_size) {  // (a game-thread store right after this look, before the hash)
        heap_mem[race_poke - HEAP_BASE]++;
        page_dirty[(race_poke - HEAP_BASE) / 4096] = 1;
        race_at = 0;
    }
    return w;
}
static bool userptr;
static bool overlap_on(void) { return true; }
static uint64_t completed, pending = 1, issued, waited;
static uint64_t vk_completed_seq(void) { return completed; }
static uint64_t vk_pending_seq(void) { return pending; }
static uint64_t renderer_issued_seq(void) { return issued; }
static const char *wait_why;
static void vk_wait_seq(uint64_t seq) { assert(seq <= issued); waited = seq; completed = seq; }
static int pool_flags = 4;  // CPU_CACHED
int nvn_pool_flags_at(uint64_t addr) { (void)addr; return pool_flags; }
static uint64_t frame = 10;
uint64_t renderer_exec_frame(void) { return frame; }
static const char *level = "Field";
const char *game_level_name(void) { return level; }
static uint64_t stat_snap_bytes, stat_snap_copies;
''' + snap + r'''
static void snap_ring_import_tail(void) {}
static int pages_seen_written(uint64_t addr, uint64_t size, uint32_t since) {  // (the stamps the looks left)
    if (track_off) return -1;
    for (uint64_t p = (addr - HEAP_BASE) / 4096; p < (addr - HEAP_BASE + size + 4095) / 4096; p++) if (page_gen[p] > since) return 1;
    return 0;
}
static void gpu_wrote(uint64_t addr, uint64_t size) {  // (a dispatch's output: bytes change, no dirty bit)
    for (uint64_t o = 0; o + 8 <= size; o += 256) heap_mem[addr - HEAP_BASE + o]++;
}
static bool heap_is_userptr(void) { return userptr; }
static uint64_t ring_lo(void) { return HEAP_BASE + snap_base; }
static bool in_ring(uint64_t a) { return a >= ring_lo() && a < ring_lo() + SNAP_RING; }
static bool in_half(uint64_t a, int h) { return a >= ring_lo() + h * SNAP_HALF && a < ring_lo() + (h + 1) * SNAP_HALF; }

// The write watch (LARECOMP_OVERLAP_WATCH unset): compute-only ranges in place, copied once the OS's page write
// tracking shows a CPU write on their pages; no page protection anywhere
static void watch_tests(void) {
    assert(snap_on() && watch_on());
    uint64_t A = HEAP_BASE + 0x1000, B = HEAP_BASE + 0x20000, B2 = HEAP_BASE + 0x20800, C = HEAP_BASE + 0x40000, D = HEAP_BASE + 0x60000;
    // W1. a compute-only range starts in place; its pages are looked at (stamped) once, and once per frame after
    unsigned l = looks;
    assert(snap_storage(A, 32640, true) == A && rs_tab[0].mode == RS_PLACE && rs_tab[0].wgen && looks == l + 1);
    assert(snap_storage(A, 32640, true) == A && looks == l + 1);  // (again this frame: no look)
    frame++;
    assert(snap_storage(A, 32640, true) == A && looks == l + 2);  // no CPU write: in place
    // W2. the CPU writes into it: one write is noted (an initialisation stays in place); writes that keep coming
    //     (within 8 frames) make it a copy for good, kept while unchanged
    cpu_wrote(A + 100);
    frame++;
    assert(snap_storage(A, 32640, true) == A && rs_tab[0].mode == RS_PLACE);
    cpu_wrote(A + 200);
    frame++;
    uint64_t a1 = snap_storage(A, 32640, true);
    assert(a1 != A && in_ring(a1) && rs_tab[0].mode == RS_COPY && !memcmp((void *)(uintptr_t)a1, heap_mem + 0x1000, 32640));
    frame++;
    assert(snap_storage(A, 32640, true) == a1);
    // W3. a level change or 10 s unused never demotes a copied range; a data change refreshes it
    level = "Dungeon";
    frame += 601;  // (unused 10 s: its copy is released and made again - still a copy)
    uint64_t a1b = snap_storage(A, 32640, true);
    assert(a1b != A && in_ring(a1b) && rs_tab[0].mode == RS_COPY);
    heap_mem[0x1000 + 3] ^= 1;
    frame++;
    uint64_t a2 = snap_storage(A, 32640, true);
    assert(a2 != a1b && rs_tab[0].mode == RS_COPY && !memcmp((void *)(uintptr_t)a2, heap_mem + 0x1000, 32640));
    // W4. an in-place tracked range stays in place over a level change (no hash probe, no copy); a GPU write (no
    //     dirty bit) changes nothing
    assert(snap_storage(C, 2048, true) == C && rs_tab[1].mode == RS_PLACE);
    heap_mem[0x40000 + 9] = 0x42;  // (the GPU wrote: the tracking doesn't see it)
    level = "Beach";
    frame += 601;
    assert(snap_storage(C, 2048, true) == C && rs_tab[1].mode == RS_PLACE);
    // W5. two ranges on one page (the light lists): a CPU write into one marks both - from their third look on (a
    //     store on a shared page: the first two looks leave the GPU's first writes time to show)
    frame++;
    assert(snap_storage(B, 2048, true) == B && snap_storage(B2, 2048, true) == B2);
    cpu_wrote(B2 + 8);
    frame++;
    assert(snap_storage(B, 2048, true) == B && snap_storage(B2, 2048, true) == B2 && rs_tab[2].mode == RS_PLACE);  // (not even noted)
    cpu_wrote(B2 + 16);
    frame++;
    assert(snap_storage(B, 2048, true) == B && snap_storage(B2, 2048, true) == B2);  // (noted)
    cpu_wrote(B2 + 32);
    frame++;
    uint64_t b1 = snap_storage(B, 2048, true), b2 = snap_storage(B2, 2048, true);
    assert(b1 != B && b2 != B2 && rs_tab[2].mode == RS_COPY && rs_tab[3].mode == RS_COPY);
    // W6. a write before the range is first seen is consumed by the first look: not a reason to copy
    cpu_wrote(D + 8);
    frame++;
    assert(snap_storage(D, 2048, true) == D && rs_tab[4].mode == RS_PLACE);
    frame++;
    assert(snap_storage(D, 2048, true) == D && rs_tab[4].mode == RS_PLACE);
    // W7. a draw's storage read of a tracked in-place range: a copy for good, as without the watch
    uint64_t E = HEAP_BASE + 0x80000;
    frame++;
    assert(snap_storage(E, 2048, true) == E);
    frame++;
    uint64_t e1 = snap_storage(E, 2048, false);
    assert(e1 != E && rs_tab[5].mode == RS_COPY && rs_tab[5].draw_bound);
    frame++;
    assert(snap_storage(E, 2048, true) == e1);
    // W8. another looker (the vertex cache) consuming the OS bits first doesn't hide the write: the generations carry it
    uint64_t F = HEAP_BASE + 0xA0000;
    frame++;
    assert(snap_storage(F, 2048, true) == F);
    frame++;
    assert(snap_storage(F, 2048, true) == F);  // (its second look: from the third on, a store on its shared page counts)
    uint32_t g;
    for (int i = 0; i < 2; i++) {  // (written in two frames, someone else looking first each time)
        cpu_wrote(F + 4);
        assert(pages_cpu_written(F, 4096, 0, &g) == 1);  // (the bits are reset, the page stamped)
        frame++;
        uint64_t fc = snap_storage(F, 2048, true);
        assert(i == 0 ? fc == F : (fc != F && rs_tab[6].mode == RS_COPY));
    }
    // W9. BowWow's skinning output (22048 bytes, a GPU output the CPU reads back) whose last page holds the dispatch's
    //     12-byte parameter blocks, which the CPU rewrites every frame: the output stays in place, the params are copied
    uint64_t S = HEAP_BASE + 0xC0000, P = S + 22048 + 0x40;  // (P on S's last page)
    frame++;
    assert(snap_storage(S, 22048, true) == S && snap_storage(P, 12, true) == P);
    for (int i = 0; i < 4; i++) {
        cpu_wrote(P);
        frame++;
        uint64_t sp = snap_storage(P, 12, true);
        assert(snap_storage(S, 22048, true) == S && rs_tab[7].mode == RS_PLACE);  // (never the output)
        if (i >= 1) assert(sp != P && rs_tab[8].mode == RS_COPY);              // (the params: written every frame)
    }
    cpu_wrote(S + 8);  // a write on one of the output's own pages, then another: that is the CPU's - copied
    frame++;
    assert(snap_storage(S, 22048, true) == S);
    cpu_wrote(S + 8);
    frame++;
    assert(snap_storage(S, 22048, true) != S && rs_tab[7].mode == RS_COPY);
    // W10. a single write (an initialisation) leaves a range in place, also after many frames
    uint64_t I = HEAP_BASE + 0x100000;
    frame++;
    assert(snap_storage(I, 4096, true) == I);
    cpu_wrote(I);
    frame++;
    assert(snap_storage(I, 4096, true) == I);
    for (int i = 0; i < 20; i++) { frame++; assert(snap_storage(I, 4096, true) == I && rs_tab[9].mode == RS_PLACE); }
    cpu_wrote(I);  // (a lone write 21 frames later: still not a rewrite)
    frame++;
    assert(snap_storage(I, 4096, true) == I && rs_tab[9].mode == RS_PLACE);
    // W11 (#355). BowWow's skinning outputs as the first Linux log laid them out: eight 22048-byte outputs 224 bytes
    //     apart, then (992 bytes on) four 3104-byte ones 224 bytes apart, which the GPU writes every frame, with
    //     per-frame constants the CPU stores in the memory before, between and after them (no watched range there).
    //     The outputs' bytes change on pages the CPU didn't touch: GPU outputs, and a store beside them doesn't copy
    //     them. A store on a page wholly inside one, in two frames, still does.
    uint64_t X = HEAP_BASE + 0x200000, O[8], T[4];
    for (int i = 0; i < 8; i++) O[i] = X + 0x700 + (uint64_t)i * 0x5700;
    for (int i = 0; i < 4; i++) T[i] = X + 0x2c800 + (uint64_t)i * 0xd00;
    frame++;
    for (int i = 0; i < 8; i++) assert(snap_storage(O[i], 22048, true) == O[i]);
    for (int i = 0; i < 4; i++) assert(snap_storage(T[i], 3104, true) == T[i]);
    for (int f = 0; f < 6; f++) {
        cpu_wrote(X + 0x100);    // (before the first output, on its first page)
        cpu_wrote(X + 0x2c500);  // (between the last 22048 output's tail and the first 3104 one, on both their pages)
        cpu_wrote(X + 0x2f800);  // (after the last 3104 output, on its page)
        if (f >= 1) {  // (the dispatch of the frame they were made in lands two frames later at most: the overlap)
            for (int i = 0; i < 8; i++) gpu_wrote(O[i], 22048);
            for (int i = 0; i < 4; i++) gpu_wrote(T[i], 3104);
        }
        frame++;
        for (int i = 0; i < 8; i++) assert(snap_storage(O[i], 22048, true) == O[i]);
        for (int i = 0; i < 4; i++) assert(snap_storage(T[i], 3104, true) == T[i]);
    }
    uint64_t got = 0;
    for (int f = 0; f < 2; f++) { cpu_wrote(O[0] + 0x2000); frame++; got = snap_storage(O[0], 22048, true); }  // (a store inside output 0, in two frames)
    assert(got != O[0]);
    for (int i = 2; i < 8; i++) assert(snap_storage(O[i], 22048, true) == O[i]);
    // W12. the light / exposure lists (32640, 32640, 2040, 2040: one allocation the CPU memcpys every frame), at any
    //      offset in the page: all four are copied, also the small list whose only page it shares with other memory
    for (uint64_t off = 0; off < 0x1000; off += 0x300) {
        uint64_t L = HEAP_BASE + 0x300000 + off / 0x300 * 0x20000 + off, R[4] = {L, L + 0x8000, L + 0x10000, L + 0x10800};
        const uint64_t Z[4] = {32640, 32640, 2040, 2040};
        frame++;
        for (int i = 0; i < 4; i++) assert(snap_storage(R[i], Z[i], true) == R[i]);
        for (int f = 0; f < 3; f++) {  // (every frame: the small lists' shared page counts from their third look)
            for (uint64_t p = L; p < L + 0x10800 + 2040; p += 1024) cpu_wrote(p);
            gpu_wrote(R[2], 2040);  // (the exposure program writes the list too: never taken for an output, its page is never clean)
            frame++;
            for (int i = 0; i < 4; i++) snap_storage(R[i], Z[i], true);
        }
        for (int i = 0; i < 4; i++) assert(snap_storage(R[i], Z[i], true) != R[i]);
    }
    // W13 (#355, second log: Sale's house after the crane game). BowWow's buffers made again where the heap had room:
    //     3104-byte ones 0x1800 apart (3040 bytes between them: no group), 22048-byte ones right after (gap 0) a 32768-byte
    //     buffer draws read, which the CPU rewrites every frame, so their first page is the CPU's too. Addresses as in
    //     the log (7283xxxxxx -> K + xxxxxx). Constants in the gaps dirty one page of each 3104-byte output every frame.
    //     None of the outputs is copied; the draw buffers are (a draw's read); a 3104-byte buffer the CPU writes and
    //     the GPU doesn't (an input: a bone palette) is still copied.
    uint64_t K = HEAP_BASE + 0x400000 - 0xf0000;
    const uint64_t t13[6] = {0xf8500, 0xf9d00, 0xfb500, 0xfcd00, 0xfe500, 0xffd00}, o13[2] = {0x123500, 0x187700}, d13[3] = {0x11b500, 0x17f700, 0x101500};
    frame++;
    for (int i = 0; i < 6; i++) assert(snap_storage(K + t13[i], 3104, true) == K + t13[i]);
    for (int i = 0; i < 2; i++) assert(snap_storage(K + o13[i], 22048, true) == K + o13[i]);
    for (int i = 0; i < 3; i++) assert(snap_storage(K + d13[i], 32768, false) != K + d13[i]);
    uint64_t IN = K + 0x1c0f00;  // (the input, 0x...f00 like 72827b8f00: on two pages)
    assert(snap_storage(IN, 3104, true) == IN);
    for (int f = 0; f < 12; f++) {
        for (int i = 0; i < 3; i++) for (uint64_t p = K + d13[i]; p < K + d13[i] + 32768; p += 1024) cpu_wrote(p);
        cpu_wrote(K + 0xf9500), cpu_wrote(K + 0xfc500), cpu_wrote(K + 0xff500);  // (pages f9, fc, ff: one of each 3104's two)
        cpu_wrote(IN + 0x40), cpu_wrote(IN + 0x200);
        if (f >= 1) {
            for (int i = 0; i < 6; i++) gpu_wrote(K + t13[i], 3104);
            for (int i = 0; i < 2; i++) gpu_wrote(K + o13[i], 22048);
        }
        frame++;
        for (int i = 0; i < 6; i++) assert(snap_storage(K + t13[i], 3104, true) == K + t13[i]);
        for (int i = 0; i < 2; i++) assert(snap_storage(K + o13[i], 22048, true) == K + o13[i]);
        for (int i = 0; i < 3; i++) assert(snap_storage(K + d13[i], 32768, false) != K + d13[i]);
        got = snap_storage(IN, 3104, true);
    }
    assert(got != IN);
    // W14 (review): a CPU store landing between a look and the part hashes changes the bytes of a part whose pages the
    //     look saw clean. Asked again after the hash, it is the CPU's: the part is not taken for a GPU output, and the
    //     store counts at the next look (with a second one, a copy).
    uint64_t Q = HEAP_BASE + 0x600000 + 0x700;
    frame++;
    assert(snap_storage(Q, 22048, true) == Q);
    int qi = -1;
    for (int i = 0; i < nrs; i++) if (rs_tab[i].addr == Q) qi = i;
    for (int f = 0; f < 3; f++) { frame++; assert(snap_storage(Q, 22048, true) == Q); }  // (past the new-range looks)
    race_at = Q, race_size = 22048, race_poke = Q + 0x1000;  // (its inner pages, in the hashed first 4 KB)
    frame++;
    assert(snap_storage(Q, 22048, true) == Q && !race_at && rs_tab[qi].gpu_parts == 0);
    frame++;
    assert(snap_storage(Q, 22048, true) == Q && rs_tab[qi].gpu_parts == 0);  // (the raced store, seen at this look: noted)
    cpu_wrote(Q + 0x1000);
    frame++;
    assert(snap_storage(Q, 22048, true) != Q && rs_tab[qi].mode == RS_COPY);  // (another: writes that keep coming)
    // W15 (review): learning starts again when a range comes back after more than 8 frames, or on a new level - the
    //     heap there may hold a CPU input now. A 3104-byte range learned GPU-written, unbound 20 frames, then written by
    //     the CPU on a shared page every frame and no longer by the GPU: copied. Same after a level change.
    for (int v = 0; v < 2; v++) {
        uint64_t U = HEAP_BASE + 0x620000 + (uint64_t)v * 0x10000 + 0x500;
        frame++;
        assert(snap_storage(U, 3104, true) == U);
        int ui = -1;
        for (int i = 0; i < nrs; i++) if (rs_tab[i].addr == U) ui = i;
        for (int f = 0; f < 4; f++) { gpu_wrote(U, 3104); frame++; assert(snap_storage(U, 3104, true) == U); }
        assert(rs_tab[ui].gpu_parts);
        if (v == 0) frame += 20; else { level = v == 1 ? "Lv01TailCave" : "Field"; frame++; }
        for (int f = 0; f < 5; f++) { cpu_wrote(U + 0x40); frame++; got = snap_storage(U, 3104, true); }
        assert(got != U && rs_tab[ui].mode == RS_COPY);
    }
    // W16 (review): GPU-written parts are learned one by one. A range whose first page part the GPU writes (a counter)
    //     and whose last page the CPU writes is copied: only the first part's page is excused.
    uint64_t V = HEAP_BASE + 0x660000 + 0x800;
    frame++;
    assert(snap_storage(V, 0x3000, true) == V);
    int vi = -1;
    for (int i = 0; i < nrs; i++) if (rs_tab[i].addr == V) vi = i;
    for (int f = 0; f < 6; f++) {
        heap_mem[V - HEAP_BASE + 8]++;  // (the GPU: its first page part)
        cpu_wrote(V + 0x2f00);          // (the CPU: its last page)
        frame++;
        got = snap_storage(V, 0x3000, true);
    }
    assert(rs_tab[vi].gpu_parts == 1 && got != V && rs_tab[vi].mode == RS_COPY);
    // W17 (#355 round 4): the GPU's report of which slots each program stores into (gfast flags) decides before the
    //     page heuristic. Learning: a program without stores writes nothing; a slot is written once a dispatch stored
    //     into it, read once 8 reported dispatches covering it didn't; unknown before.
    const uint64_t CS1 = 0x72a8da0000ull, CS2 = 0x72e8f33e00ull;
    assert(cs_slot_class(CS1, false, 6) == 0 && cs_slot_class(CS1, true, 6) == -1);
    csw_note(CS1, 0x00EF, 0x00C0);  // (slots 0-3, 5, 6, 7 covered; 6 and 7 stored into)
    assert(cs_slot_class(CS1, true, 6) == 1 && cs_slot_class(CS1, true, 7) == 1 && cs_slot_class(CS1, true, 0) == -1);
    for (int i = 0; i < 7; i++) csw_note(CS1, 0x00EF, 0x00C0);
    assert(cs_slot_class(CS1, true, 0) == 0 && cs_slot_class(CS1, true, 5) == 0 && cs_slot_class(CS1, true, 4) == -1);
    for (int i = 0; i < 8; i++) csw_note(CS2, 0x00EF, 0x0087);  // (64-byte slots 0, 1, 2 and the 1552-byte 7 stored; 3 the palette, 5, 6 read)
    csw_note(CS2, 0x00EF, 0x00C7);                             // (slot 6 stored into once, later: written from then on)
    assert(cs_slot_class(CS2, true, 3) == 0 && cs_slot_class(CS2, true, 6) == 1 && cs_slot_class(CS2, true, 0) == 1);
    // Outputs (written slots) beside CPU-rewritten memory on every page they have - the case no page heuristic can
    // read: small outputs (64 / 160 bytes, an eye's) and a 3104-byte one, with no GPU byte change in the test either.
    // None is copied. A palette in a read slot on such a page is copied (no new-range delay: the class is known).
    uint64_t Y = HEAP_BASE + 0x700000;
    const uint64_t so[3] = {Y + 0x740, Y + 0x2800, Y + 0x4500}, sz[3] = {64, 160, 3104};
    uint64_t PAL = Y + 0x6400;
    frame++;
    snap_slot_class = 1;
    for (int i = 0; i < 3; i++) assert(snap_storage(so[i], sz[i], true) == so[i]);
    snap_slot_class = 0;
    assert(snap_storage(PAL, 3104, true) == PAL);
    for (int f = 0; f < 6; f++) {
        for (uint64_t pg = Y; pg < Y + 0x8000; pg += 4096) cpu_wrote(pg + 0x10);  // (every page of the block, outside the ranges)
        cpu_wrote(PAL + 0x40);
        frame++;
        snap_slot_class = 1;
        for (int i = 0; i < 3; i++) assert(snap_storage(so[i], sz[i], true) == so[i]);
        snap_slot_class = 0;
        got = snap_storage(PAL, 3104, true);
        if (f >= 1) assert(got != PAL);  // (writes in two looks: copied)
    }
    snap_slot_class = -1;
    // A written range the CPU stores into on a page wholly its own is the CPU's too: copied (with a second store)
    uint64_t WO = HEAP_BASE + 0x720000 + 0x700;
    frame++;
    snap_slot_class = 1;
    assert(snap_storage(WO, 22048, true) == WO);
    for (int f = 0; f < 2; f++) { cpu_wrote(WO + 0x1000); frame++; got = snap_storage(WO, 22048, true); }
    assert(got != WO);
    // The light / exposure lists with the exposure program storing into the small lists (written slots) and the big
    // ones read: the CPU's memcpy of the allocation copies all four, at every page offset
    for (uint64_t off = 0; off < 0x1000; off += 0x300) {
        uint64_t L = HEAP_BASE + 0x740000 + off / 0x300 * 0x20000 + off, R[4] = {L, L + 0x8000, L + 0x10000, L + 0x10800};
        const uint64_t Z[4] = {32640, 32640, 2040, 2040};
        frame++;
        for (int i = 0; i < 4; i++) { snap_slot_class = i < 2 ? 0 : 1; assert(snap_storage(R[i], Z[i], true) == R[i]); }
        for (int f = 0; f < 2; f++) {
            for (uint64_t pg = L; pg < L + 0x10800 + 2040; pg += 1024) cpu_wrote(pg);
            gpu_wrote(R[2], 2040);
            frame++;
            for (int i = 0; i < 4; i++) { snap_slot_class = i < 2 ? 0 : 1; snap_storage(R[i], Z[i], true); }
        }
        for (int i = 0; i < 4; i++) { snap_slot_class = i < 2 ? 0 : 1; assert(snap_storage(R[i], Z[i], true) != R[i]); }
    }
    snap_slot_class = -1;
    puts("PASS: write watch - in place + tracked, CPU writes that keep coming make a copy for good (a single one doesn't), no demotion on level / idle, GPU writes ignored, a shared page marks both, a page shared with a small parameter block doesn't, nor a store beside a GPU output on its shared pages (#355, both Linux layouts), learning: a store between look and hash, reuse after a gap / level, per part, the GPU's slot report (outputs never copied by neighbours' stores, inputs at once, the lists), the lists at any page offset, a prior write is consumed, draw reads copy, other lookers");
}

int main(int argc, char **argv) {
#ifdef _WIN32
    heap_mem = _aligned_malloc(HEAP_MAX, 0x10000);  // (MinGW has no posix_memalign)
    if (heap_mem) memset(heap_mem, 0, HEAP_MAX);
#else
    if (posix_memalign((void **)&heap_mem, 0x10000, HEAP_MAX)) heap_mem = NULL;  // (aligned like the real heap base)
    else memset(heap_mem, 0, HEAP_MAX);
#endif
    assert(heap_mem);
    if (argc > 1 && !strcmp(argv[1], "watch")) { watch_tests(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "notrack")) {  // no OS write tracking (a userptr / dma-buf heap on Linux, no write watch): the probe instead
        track_off = 1; userptr = true;
        assert(snap_on() && !watch_on());
        uint64_t A = HEAP_BASE + 0x1000;
        assert(snap_storage(A, 32640, true) != A && rs_tab[0].mode == RS_PROBE);
        puts("PASS: no page write tracking - the watch is off, compute-only ranges get the hash probe");
        return 0;
    }
    uint64_t A = HEAP_BASE + 0x1000, B = HEAP_BASE + 0x20000, C = HEAP_BASE + 0x40000;
    uint8_t *a = heap_mem + 0x1000, *b = heap_mem + 0x20000;
    memset(a, 1, 32640); memset(b, 2, 2048); memset(heap_mem + 0x40000, 3, 4096);
    // decided on first use: the ring follows the heap (64 KB aligned), committed once
    assert(snap_ring_end() == heap_size + SNAP_RING && commits == 1 && snap_on());
    // 1. the probe: a first use binds a copy with the same bytes
    uint64_t c1 = snap_storage(A, 32640, true);
    assert(c1 != A && in_ring(c1) && !memcmp((void *)(uintptr_t)c1, a, 32640) && nrs == 1 && stat_snap_copies == 1);
    assert(snap_storage(A, 32640, true) == c1);  // (bound again this frame: the same copy)
    // 2. unchanged at the next use: IN PLACE from then on (the GPU-only skinning outputs)
    frame++;
    assert(snap_storage(A, 32640, true) == A && rs_tab[0].mode == RS_PLACE);
    frame++;
    a[100] = 9;  // (the GPU writes it on the Switch; here the probe said the CPU doesn't: stays in place, no hash)
    assert(snap_storage(A, 32640, true) == A);
    // 3. the light list: rewritten between frames -> COPY, a fresh copy whenever its data changes
    uint64_t c2 = snap_storage(B, 2048, true);
    assert(c2 != B && in_ring(c2) && rs_tab[1].mode == RS_PROBE);
    frame++;
    b[7] = 0x55;
    uint64_t c3 = snap_storage(B, 2048, true);
    assert(c3 != B && c3 != c2 && rs_tab[1].mode == RS_COPY && ((uint8_t *)(uintptr_t)c3)[7] == 0x55);
    assert(snap_storage(B, 2048, false) == c3);  // (a draw reading it this frame: the same copy)
    frame++;
    assert(snap_storage(B, 2048, true) == c3);   // unchanged: the copy (maybe GPU-modified) stays
    frame++;
    b[8] = 0x66;
    uint64_t c4 = snap_storage(B, 2048, true);
    assert(c4 != c3 && ((uint8_t *)(uintptr_t)c4)[8] == 0x66);
    // 4. a range a draw reads as storage: a copy for good from its first use (no probe: the hash can't see the CPU
    //    rewriting the same bytes), kept while unchanged, refreshed when changed; a compute bind takes the same copy
    uint64_t c5 = snap_storage(C, 4096, false);
    assert(c5 != C && rs_tab[2].mode == RS_COPY && rs_tab[2].draw_bound);
    assert(snap_storage(C, 4096, true) == c5);
    frame++;
    assert(snap_storage(C, 4096, false) == c5 && snap_storage(C, 4096, true) == c5);
    frame++;
    heap_mem[0x40000 + 5] = 7;
    uint64_t c6 = snap_storage(C, 4096, false);
    assert(c6 != C && c6 != c5 && rs_tab[2].mode == RS_COPY);
    level = "Beach";  // a new level: a draw-read range is not probed again, it stays a copy
    frame++;
    assert(snap_storage(C, 4096, true) == c6 && rs_tab[2].mode == RS_COPY);
    level = "Field";
    // 4b. first seen in a compute-only frame (the title screen): probed, settled in place; later read by a draw:
    //     copied for good, and the compute binds after that use the same copy even though the data never changes
    uint64_t H = HEAP_BASE + 0x60000;
    memset(heap_mem + 0x60000, 8, 2048);
    frame++;
    assert(snap_storage(H, 2048, true) != H && rs_tab[3].mode == RS_PROBE);
    frame++;
    assert(snap_storage(H, 2048, true) == H && rs_tab[3].mode == RS_PLACE);
    frame++;
    assert(snap_storage(H, 2048, true) == H);  // (a compute bind, first in the frame: still in place)
    uint64_t h1 = snap_storage(H, 2048, false);  // the draw, same frame: a copy from now on
    assert(h1 != H && in_ring(h1) && rs_tab[3].mode == RS_COPY && rs_tab[3].draw_bound);
    frame++;
    assert(snap_storage(H, 2048, true) == h1 && snap_storage(H, 2048, false) == h1);
    frame += 601;  // unused for 10 s: still not probed again (its copy was released: a new one, still a copy)
    uint64_t h2 = snap_storage(H, 2048, true);
    assert(h2 != H && in_ring(h2) && rs_tab[3].mode == RS_COPY && !memcmp((void *)(uintptr_t)h2, heap_mem + 0x60000, 2048));
    // 4c. in place from a compute-only probe, then a draw in a later frame (not the same one): copied for good
    uint64_t I = HEAP_BASE + 0x70000;
    frame++;
    assert(snap_storage(I, 2048, true) != I);
    frame++;
    assert(snap_storage(I, 2048, true) == I && rs_tab[4].mode == RS_PLACE);
    frame++;
    uint64_t i1 = snap_storage(I, 2048, false);
    assert(i1 != I && rs_tab[4].mode == RS_COPY);
    frame++;
    assert(snap_storage(I, 2048, true) == i1);
    // 5. a new level: probed again
    frame++;
    level = "Dungeon";
    assert(snap_storage(A, 32640, true) != A && rs_tab[0].mode == RS_PROBE);
    frame++;
    assert(snap_storage(A, 32640, true) == A && rs_tab[0].mode == RS_PLACE);
    // 6. unused for 10 s: probed again
    frame += 601;
    assert(snap_storage(A, 32640, true) != A && rs_tab[0].mode == RS_PROBE);
    // 7. not snapshotted: static pools, ranges in or past the ring, too large, outside the heap
    pool_flags = 1;
    assert(snap_storage(B, 2048, true) == B);
    pool_flags = 4;
    assert(snap_storage(ring_lo(), 2048, true) == ring_lo());
    assert(snap_storage(HEAP_BASE + heap_size - 1024, 2048, true) == HEAP_BASE + heap_size - 1024);
    assert(snap_storage(A, SNAP_HALF / 2 + 256, true) == A);
    assert(snap_storage(A + 0x4000, SNAP_HALF / 2, true) != A + 0x4000);  // (the largest allowed: a half holds two, one bound by compute)
    assert(snap_storage(HEAP_BASE - 4096, 2048, true) == HEAP_BASE - 4096);
    // 8. the halves: 1 MB copies (512 KB ranges and their pristine images) fill half 0; the switch to half 1 needs no wait (never used); a live copy stays where
    //    it is across the switch (the GPU's writes into it survive); the switch back waits for the submission pending
    //    when half 0 was left and reuses only freed slots, walking past the live copies
    frame++;
    uint64_t big = SNAP_HALF / 16;  // (a compute-bound copy takes twice that: the copy and its pristine image)
    uint8_t *d = heap_mem + (1ull << 20);
    uint64_t D = HEAP_BASE + (1ull << 20);
    memset(d, 4, big);
    snap_off = 0; snap_half = 0; snap_gen++;  // (a clean half 0: the earlier cases' copies released)
    for (int i = 0; i < nrs; i++) rs_tab[i].copy = 0;
    uint64_t keep = snap_storage(D, big, true);  // a COPY candidate: probe copy at the start of half 0
    assert(in_half(keep, 0));
    frame++;
    d[0] = 5;
    uint64_t keep2 = snap_storage(D, big, true);  // changed: COPY mode, a copy in half 0
    assert(in_half(keep2, 0) && rs_tab[nrs - 1].mode == RS_COPY);
    issued = 5; pending = 6; completed = 5; waited = 0;
    for (int i = 0; i < 6; i++) {  // 6 more fill half 0 (8 x 1 MB)
        uint64_t E = HEAP_BASE + (2ull << 20) + (uint64_t)i * 4096;
        assert(in_half(snap_storage(E, big, true), 0));
    }
    uint64_t F = HEAP_BASE + (3ull << 20);
    uint64_t f1 = snap_storage(F, big, true);  // the 9th: half 1, no wait, half 0's users noted as the pending submission
    assert(in_half(f1, 1) && !waited && snap_half == 1 && snap_half_seq[0] == 6);
    frame++;
    // the old COPY-mode entry, unchanged: its live copy in half 0 is bound again (not remade from the original)
    uint64_t keep3 = snap_storage(D, big, true);
    assert(keep3 == keep2 && in_half(keep3, 0));
    for (int i = 0; i < 7; i++) {  // 7 more fill half 1 (F + 7 x 1 MB)
        uint64_t E = HEAP_BASE + (5ull << 20) + (uint64_t)i * 4096;
        assert(in_half(snap_storage(E, big, true), 1));
    }
    // half 1 is full. Switching back needs submission 6 done: it is only being recorded (issued 5): the bind falls
    // back to the original for this one draw, nothing waits, nothing is overwritten
    frame++;
    uint64_t G = HEAP_BASE + (7ull << 20);
    assert(snap_storage(G, big, true) == G && snap_half == 1 && !waited);
    // once submitted (issued 7, still not complete): the switch waits for 6, then takes half 0
    issued = 7; pending = 8;
    uint64_t g1 = snap_storage(G, big, true);  // (half 0's only free slot: the probe copy D left behind; the live copies stay)
    assert(in_half(g1, 0) && g1 == keep && waited == 6 && snap_half == 0 && snap_half_seq[1] == 8);
    assert(rs_tab[nrs - 1].copy == g1 && !memcmp((void *)(uintptr_t)keep2, d, big));  // (D's live copy untouched)
    // 8b. the per-frame pools (0x12): a draw's storage read is copied without a table entry; a compute bind of one
    //     gets an entry, as before
    issued = pending = completed = 9;
    snap_off = 0; snap_half = 0;  // (a clean half 0: case 8's copies released)
    for (int i = 0; i < nrs; i++) rs_tab[i].copy = rs_tab[i].pristine = 0;
    frame++;
    int n0 = nrs;
    pool_flags = 0x12;
    uint8_t *p12 = heap_mem + (6ull << 20);
    uint64_t P = HEAP_BASE + (6ull << 20);
    memset(p12, 7, 64);
    uint64_t pc = snap_storage(P, 64, false);
    assert(pc != P && nrs == n0 && !memcmp((void *)(uintptr_t)pc, p12, 64));
    assert(snap_storage(P + 256, 64, true) != P + 256 && nrs == n0 + 1);
    pool_flags = 4;
    // 8c. a compute copy's remake keeps the GPU's writes into it where the CPU didn't write (the exposure list's state)
    frame++;
    uint8_t *x = heap_mem + (6ull << 20) + 65536;
    uint64_t X = HEAP_BASE + (6ull << 20) + 65536;
    memset(x, 1, 2048);
    assert(snap_storage(X, 2048, true) != X);  // probe
    frame++;
    x[100] = 2;
    uint64_t x1 = snap_storage(X, 2048, true);  // the CPU rewrote it: a copy, with its pristine image behind it
    RangeState *ex = &rs_tab[nrs - 1];
    assert(ex->addr == X && ex->mode == RS_COPY && ex->pristine == x1 + 2048);
    ((uint8_t *)(uintptr_t)x1)[10] = 9;  // the GPU writes bytes 10 and 20
    ((uint8_t *)(uintptr_t)x1)[20] = 9;
    frame++;
    x[20] = 3, x[30] = 4;  // the CPU writes bytes 20 and 30
    uint64_t x2 = snap_storage(X, 2048, true);
    const uint8_t *m2 = (const uint8_t *)(uintptr_t)x2;
    assert(x2 != x1 && m2[10] == 9 && m2[20] == 3 && m2[30] == 4 && m2[100] == 2 && m2[0] == 1);
    assert(!memcmp((void *)(uintptr_t)ex->pristine, x, 2048));
    // 8d. a full table evicts the entry bound longest ago (never the whole table)
    frame++;
    for (int i = 0; nrs < 512; i++) assert(snap_storage(HEAP_BASE + (6ull << 20) + 131072 + (uint64_t)i * 256, 256, true));
    uint64_t a0 = rs_tab[0].addr;
    rs_tab[3].frame = 0;
    frame++;
    uint64_t Z = HEAP_BASE + (6ull << 20) + 65536 + 4096;
    assert(snap_storage(Z, 256, true) != Z && nrs == 512 && rs_tab[3].addr == Z && rs_tab[0].addr == a0);
    // 9. the game grows its heap into the ring: off, everything in place from then on
    heap_size = snap_base + 4096;
    assert(snap_storage(A, 32640, true) == A && snap_state == 0 && snap_ring_end() == 0);
    // 10. a reboot decides again (heap not set up yet: not decided; then on)
    snap_reset();
    heap_size = 0;
    assert(!snap_on() && snap_state == -1);
    heap_size = 8ull << 20;
    assert(snap_on() && snap_base == (8ull << 20) && commits == 2 && nrs == 0);
    puts("PASS: snapshots - probe / in place / copy, level and idle re-probes, exclusions, the two halves and their waits, per-frame pool reads without entries, GPU writes kept across a remake, eviction, heap growth, reboot");
}
'''
# gmap_build: the fast-copy table and the store report's bit mapping. A store's report bit is the TABLE ENTRY it lands
# in (translate.c gfindw: GFast word GFAST_FLAG_WORD + entry, only inside the entry's bound bytes, gmap[i].w); the
# entry's slot bits (bit i = storage slot i of the dispatch) turn that into storage slots (gfast_flags_poll). gfindw is
# mirrored here in C from the same table words; tools/test_vulkan_overlap.py's 'glsl' check pins its GLSL text.
gmap_harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "shader.h"
''' + gmap + r'''
static uint32_t tab[(GMAP_MAX + 1) * 4];
// gfindw (translate.c), in C: the entry whose bound bytes hold heap offset g, -1 none (also -1: inside an entry's
// 256-byte rounding but outside its bound bytes - the load path still finds it there, the report doesn't)
static int gfindw(uint32_t g, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t o = g - tab[i * 4];
        if (g >= tab[i * 4] && o < tab[i * 4 + 1]) return (o >= (tab[i * 4 + 3] & 0xFFu) && (o >> 2) < (tab[i * 4 + 3] >> 8)) ? i : -1;
    }
    return -1;
}
static uint16_t slots_of_stores(const GmapEnt *r, int n, const uint32_t *g, int ng) {  // (gfast_flags_poll's mapping)
    uint16_t w = 0;
    for (int k = 0; k < ng; k++) { int e = gfindw(g[k], n); if (e >= 0) w |= r[e].slots; }
    return w;
}
int main(void) {
    const uint64_t B = 0x7281000000ull;  // (heap base in the logged addresses: offsets below)
    GmapEnt r[16];
    uint64_t used;
    // BowWow's skinning program 72a8da0000 as dispatched (Mac / Linux logs): slots 0-3 66144-byte, 5 the 12-byte
    // parameters, 6-7 22048-byte outputs. One entry per slot (none overlap), each with exactly its slot's bit.
    uint64_t off[16] = {0}, size[16] = {0};
    const uint64_t a1[8] = {0x7287c31b00, 0x7287c21800, 0x7287c11500, 0x7287c01200, 0, 0x7282e71900, 0x7282e46100, 0x7282e50f00};
    const uint64_t z1[8] = {66144, 66144, 66144, 66144, 0, 12, 22048, 22048};
    for (int i = 0; i < 8; i++) if (z1[i]) off[i] = a1[i] - B, size[i] = z1[i];
    int n = gmap_build(off, size, 1ull << 40, (uint64_t)GFAST_FLAG_WORD * 4, r, tab, &used);
    assert(n == 7);
    for (int e = 0; e < n; e++) {
        int s = __builtin_ctz(r[e].slots);
        assert(r[e].slots == (1u << s) && r[e].xlo == off[s] && r[e].xhi == off[s] + size[s]);
        assert(tab[e * 4] == (uint32_t)r[e].lo && (tab[e * 4 + 3] & 0xFF) == off[s] - r[e].lo && (tab[e * 4 + 3] >> 8) == (off[s] + size[s] - r[e].lo + 3) / 4);
    }
    assert(used <= (uint64_t)GFAST_FLAG_WORD * 4);
    // stores into the two outputs report slots 6 and 7 (0xc0) - not the entry indexes (sorted by address: 1 and 2)
    uint32_t st1[2] = {(uint32_t)(off[6] + 16), (uint32_t)(off[7] + 22048 - 4)};
    assert(slots_of_stores(r, n, st1, 2) == 0xc0);
    // a store past an output's last byte, in its 256-byte rounding, or between buffers: no slot
    uint32_t st2[2] = {(uint32_t)(off[6] + 22048), (uint32_t)(off[7] - 4)};
    assert(slots_of_stores(r, n, st2, 2) == 0);
    // 72e8f33e00: 64-byte slots 0, 1, 2 and 6, the 3104-byte palette in 3, 8 bytes in 5, 1552 in 7. Its stores go to
    // slots 0-2 (Windows' report 0x7): those bits and no other - the palette (3) and slot 7 stay unreported.
    memset(off, 0, sizeof off), memset(size, 0, sizeof size);
    const uint64_t a2[8] = {0x7273882740, 0x7273882940, 0x7273882b40, 0x7282e7b200, 0, 0x7282e7f800, 0x7273883540, 0x7282e7d400};
    const uint64_t z2[8] = {64, 64, 64, 3104, 0, 8, 64, 1552};
    const uint64_t B2 = 0x7240000000ull;
    for (int i = 0; i < 8; i++) if (z2[i]) off[i] = a2[i] - B2, size[i] = z2[i];
    n = gmap_build(off, size, 1ull << 40, (uint64_t)GFAST_FLAG_WORD * 4, r, tab, &used);
    assert(n == 7);
    uint32_t st3[3] = {(uint32_t)off[0], (uint32_t)(off[1] + 60), (uint32_t)(off[2] + 32)};
    assert(slots_of_stores(r, n, st3, 3) == 0x7);
    uint32_t st4[2] = {(uint32_t)(off[0] + 64), (uint32_t)(off[3] - 4)};  // (just past slot 0; just before the palette)
    assert(slots_of_stores(r, n, st4, 2) == 0);
    // two slots bound to overlapping bytes share an entry: a store there reports both
    memset(off, 0, sizeof off), memset(size, 0, sizeof size);
    off[2] = 0x10000, size[2] = 2040, off[9] = 0x10000 + 0x100, size[9] = 64;
    n = gmap_build(off, size, 1ull << 40, (uint64_t)GFAST_FLAG_WORD * 4, r, tab, &used);
    uint32_t st5[1] = {0x10100};
    assert(n == 1 && r[0].slots == ((1u << 2) | (1u << 9)) && slots_of_stores(r, n, st5, 1) == ((1u << 2) | (1u << 9)));
    // too many bytes for gfast (the flags sit above): no table
    memset(size, 0, sizeof size);
    off[0] = 0, size[0] = (uint64_t)GFAST_FLAG_WORD * 4 + 4;
    assert(gmap_build(off, size, 1ull << 40, (uint64_t)GFAST_FLAG_WORD * 4, r, tab, &used) == -1);
    puts("PASS: store report bits - one table entry per slot, a store reports its storage slot (BowWow's programs: 0xc0, 0x7), not padding or gaps, shared bytes report both slots, oversize falls back");
}
'''

# the GLSL the translator emits for a compute store: gfindw at the store, gfind at the load, and gfindw's report
glsl_driver = r'''
#include <stdio.h>
#include <string.h>
#include "shader.h"
#define PT (7ull << 16)
int main(void) {
    uint64_t p[16] = {0}, m = 0;
    const uint64_t ins[] = {
        0x0100000000000000ull | 0x10000ull << 20 | 0xFull << 12 | PT | 2,                     // MOV32I R2, 0x10000
        0x0100000000000000ull | 0x72ull << 20 | 0xFull << 12 | PT | 3,                        // MOV32I R3, 0x72
        0xeed0000000000000ull | 4ull << 48 | 1ull << 45 | PT | 2ull << 8 | 4,                 // LDG.E R4, [R2]
        0xeed0000000000000ull | 1ull << 51 | 4ull << 48 | 1ull << 45 | 16ull << 20 | PT | 2ull << 8 | 4,  // STG.E [R2+16], R4
        0};
    for (const uint64_t *w = ins; *w; w++) { if (m % 4 == 0) p[m++] = 0; p[m++] = *w; }
    if (m % 4 == 0) p[m++] = 0;
    p[m++] = 0xe30000000007000full;  // EXIT
    while (m % 4) p[m++] = 0x50b0000000070f00ull;
    maxwell_gmem_base = 0x7200000000ull, maxwell_gmem_size = 0x400000000ull;
    ShaderKey key = {0};
    key.glsl = 1;
    ShaderInfo *s = maxwell_translate((const uint8_t *)p, m * 8, STAGE_COMPUTE, &key);
    if (!s->msl) { printf("translation failed: %s\\n", s->error); return 1; }
    fputs(s->msl, stdout);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='vulkan-overlap-') as temporary:
    temporary = Path(temporary)
    nvn = root / 'runtime/nvn/maxwell'
    cc = shlex.split(os.environ.get('CC', 'clang'))
    gl = temporary / 'glsl_store.c'
    gl.write_text(glsl_driver)
    glexe = temporary / ('glsl_store.exe' if os.name == 'nt' else 'glsl_store')
    subprocess.run(cc + ['-std=gnu11', '-O1', '-w', '-I', str(nvn), str(gl), str(nvn / 'translate.c'), '-o', str(glexe), '-lm'], check=True)
    out = subprocess.run([str(glexe)], check=True, capture_output=True, text=True, timeout=30).stdout
    flag = f'gfast_.w[{(32 << 20) // 4 - 64}u + i] = gmap[16].y;'
    assert 'fw = ok ? gfindw(uint(ga))' in out and 'fw = ok ? gfind(uint(ga))' in out, 'the store looks up with gfindw, the load with gfind'
    assert ('uint gfindw(uint g) { uint n = gmap[16].x; for (uint i = 0u; i < n; i++) { uint o = g - gmap[i].x; '
            'if (g >= gmap[i].x && o < gmap[i].y) { if (o >= (gmap[i].w & 0xFFu) && (o >> 2) < (gmap[i].w >> 8)) ' + flag) in out, \
        'gfindw reports table entry i (only inside its bound bytes) with the stamp: the mapping gmap_build / gfast_flags_poll read'
    print('PASS: GLSL store report - gfindw at the store, gfind at the load, entry i flagged with the stamp inside its bound bytes')
    for name, harness in (('defer', defer_harness), ('decide', decide_harness), ('snap', snap_harness), ('gmap', gmap_harness)):
        cfile = temporary / f'{name}.c'
        cfile.write_text(harness)
        exe = temporary / (f'{name}.exe' if os.name == 'nt' else name)
        subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + ['-std=gnu11', '-O1', '-Wall', '-Wno-unused-function', '-Wno-unused-variable',
                        '-I', str(nvn), str(cfile), '-o', str(exe)], check=True)
        env = dict(os.environ)
        env.pop('LARECOMP_OVERLAP_WATCH', None)
        if name == 'decide':  # default on with tracking, off without (unless forced), =0 off
            env.pop('LARECOMP_GPU_OVERLAP', None)
            subprocess.run([str(exe), 'tracked', 'on'], check=True, timeout=10, env=env)
            subprocess.run([str(exe), 'untracked', 'off'], check=True, timeout=10, env=env)
            subprocess.run([str(exe), 'untracked', 'on'], check=True, timeout=10, env=dict(env, LARECOMP_GPU_OVERLAP='1'))
            subprocess.run([str(exe), 'tracked', 'off'], check=True, timeout=10, env=dict(env, LARECOMP_GPU_OVERLAP='0'))
            subprocess.run([str(exe), 'tracked', 'off'], check=True, timeout=10, env=dict(env, LARECOMP_OVERLAP_WATCH='0'))
            continue
        if name == 'snap':  # the hash probe (watch off), the write watch, no tracking
            subprocess.run([str(exe)], check=True, timeout=10, env=dict(env, LARECOMP_OVERLAP_WATCH='0'))
            subprocess.run([str(exe), 'watch'], check=True, timeout=10, env=env)
            subprocess.run([str(exe), 'notrack'], check=True, timeout=10, env=env)
        else:
            subprocess.run([str(exe)], check=True, timeout=10, env=env)
