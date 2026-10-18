// The SDK's C library routines (memcpy, memset, memcmp, strlen, ...) on the host's: the recompiled versions are the
// SDK's byte loops lifted to C, and they were ~3% of the game's CPU time (memcmp alone ~1.5% of the presenting
// thread in a busy overworld scene). tools/gen.py replaces each function listed in hle_symbols.txt with a call to
// hle_<name>. Guest pointers are host addresses. The comparisons return the difference of the first unequal bytes,
// as the SDK's do (not just the sign).
#include <stdint.h>
#include <string.h>

#include "../hle.h"

#define P(r) ((void *)(uintptr_t)c->x[r])
#define CP(r) ((const uint8_t *)(uintptr_t)c->x[r])

static int64_t byte_diff(const uint8_t *a, const uint8_t *b) {  // (a and b differ somewhere ahead)
    while (*a == *b) a++, b++;
    return (int64_t)*a - (int64_t)*b;
}

void hle_memcpy(Cpu *c) { memmove(P(0), P(1), c->x[2]); }  // (memmove: an overlapping copy behaves as the SDK's)
void hle_memmove(Cpu *c) { memmove(P(0), P(1), c->x[2]); }
void hle_memset(Cpu *c) { memset(P(0), (int)(uint8_t)c->x[1], c->x[2]); }
void hle_memcmp(Cpu *c) { c->x[0] = (uint64_t)(memcmp(P(0), P(1), c->x[2]) ? byte_diff(CP(0), CP(1)) : 0); }
void hle_bcmp(Cpu *c) { c->x[0] = memcmp(P(0), P(1), c->x[2]) != 0; }
void hle_strlen(Cpu *c) { c->x[0] = strlen((const char *)P(0)); }
void hle_strcmp(Cpu *c) {
    const uint8_t *a = CP(0), *b = CP(1);
    while (*a && *a == *b) a++, b++;
    c->x[0] = (uint64_t)((int64_t)*a - (int64_t)*b);
}
void hle_strncmp(Cpu *c) {
    const uint8_t *a = CP(0), *b = CP(1);
    uint64_t n = c->x[2];
    if (!n) { c->x[0] = 0; return; }
    while (--n && *a && *a == *b) a++, b++;
    c->x[0] = (uint64_t)((int64_t)*a - (int64_t)*b);
}
void hle_memchr(Cpu *c) { c->x[0] = (uint64_t)(uintptr_t)memchr(P(0), (int)(uint8_t)c->x[1], c->x[2]); }
