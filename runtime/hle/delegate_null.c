// Issue #19: the two forwarders of a recycled action object (main+0x10cce50 = the vtable slot +0x28 of the class
// whose vtable is 0x7001c51358, main+0x10cce30 = its slot +0x30) do
//
//     x1 = this; x0 = [this+0x10]; pc = [[x0] + 0xa0]      (0xa8 in the +0x30 one)
//
// without checking [this+0x10]. The object is recycled by the task loop (main+0x13c21e0 dispatches the slot the
// task's state picks, and slot +0x00, main+0x10cce00, re-initialises the object: it writes the vtable and clears
// the delegate at +0x10). The class's own slot +0x38 (main+0x10cce80) checks *its* delegate (+0x18) with `cbz`
// before using it, so a missing delegate means "nothing to do". With the game's threads in the port's order the
// tick can dispatch +0x28 on an object whose delegate is still NULL, and the dereference crashed (signal 11, always
// at main+0x10cce50+0x8, always the same object). tools/patches.py calls this from both forwarders so they read a
// missing delegate the same way +0x38 does.
//
// The hang that followed (issue #19, thread dump): skipping isn't always "nothing to do". The object is a frame job J
// living at X+0xc8 inside its owner X (X = [J+0x28]); a per-frame kick-off (main+0x15730) rebuilds J in place under
// X's lock - vtable 0x7001c51308, [J+0x10] = 0, then the delegate - and hangs it after the device's chain job W. W's
// finish releases J through slot +0x28 (the original crash's caller, main+0x13c229c). Arriving in the rebuild window,
// the skip dropped that release: J was never queued, its Run never set X's done flag (X+0x1d8), and the main thread
// waited on X's condition variable forever. So a live job (the derived vtable) waits, up to 1 s, until its delegate is
// set and the kick-off has let go of X's lock (the lock word at X+0x1f8), then forwards like the original; only a job
// already torn down (slot +0x00 put the base vtable back: it has run) or a wait that runs out is skipped.
#include <stdint.h>

#include "../hle.h"
#include "../platform.h"

#define JOB_VTABLE_LIVE 0x7001c51308ull
#define JOB_VTABLE_BASE 0x7001c51358ull

static uint64_t rd64(uint64_t a) { return __atomic_load_n((uint64_t *)(uintptr_t)a, __ATOMIC_ACQUIRE); }
static uint32_t rd32(uint64_t a) { return __atomic_load_n((uint32_t *)(uintptr_t)a, __ATOMIC_ACQUIRE); }

int rt_delegate_null(Cpu *c, int slot) {
    uint64_t obj = c->x[0];
    if (obj && rd64(obj + 0x10)) return 0;  // the usual case: forward like the original
    if (!obj) return 1;
    uint64_t vt = rd64(obj), owner = rd64(obj + 0x28);
    bool in_owner = owner && owner + 0xc8 == obj;  // (the layout this was worked out on; anything else just skips)
    uint64_t t0 = plat_now_ns(), waited = 0;
    bool forwarded = false;
    if (vt == JOB_VTABLE_LIVE && in_owner) {
        while ((waited = plat_now_ns() - t0) < 1000000000ull) {
            if (rd64(obj + 0x10) && !rd32(owner + 0x1f8)) { forwarded = true; break; }
            if (rd64(obj) != JOB_VTABLE_LIVE) break;  // (torn down meanwhile: it ran)
            plat_sleep_until_ns(plat_now_ns() + 50000);
        }
    }
    static int logged;
    if (logged++ < 16)
        LOG("[delegate] %#llx slot +0x%x: no delegate at [this+0x10] (vtable %#llx, count %#x, lr %#llx, owner %#llx done %d "
            "lock %#x) - %s after %llu us (issue #19)\n", (unsigned long long)obj, slot, (unsigned long long)vt, rd32(obj + 0x20),
            (unsigned long long)c->x[30], (unsigned long long)owner, in_owner ? *(uint8_t *)(uintptr_t)(owner + 0x1d8) : -1,
            in_owner ? rd32(owner + 0x1f8) : 0, forwarded ? "forwarded" : "skipped", (unsigned long long)(waited / 1000));
    if (forwarded) return 0;
    c->x[0] = 0;  // (returns 0, as slot +0x38 does when its delegate is missing: x0 is the delegate it loaded)
    return 1;
}
