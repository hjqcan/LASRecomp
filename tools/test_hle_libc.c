// Shared SDK fast paths: guest registers, byte semantics, bounds and overlap.
#include <assert.h>
#include "../runtime/hle/libc.c"

static Cpu cpu;
static uint64_t call(void (*fn)(Cpu *), void *a, const void *b, uint64_t n) {
    memset(&cpu, 0, sizeof cpu);
    cpu.x[0] = (uintptr_t)a; cpu.x[1] = (uintptr_t)b; cpu.x[2] = n;
    cpu.x[19] = 0x12345678; cpu.x[30] = 0x87654321;
    fn(&cpu);
    assert(cpu.x[19] == 0x12345678 && cpu.x[30] == 0x87654321);
    return cpu.x[0];
}

int main(void) {
    uint8_t a[16], expected[16], b[] = {0, 128, 255, 0, 3};
    for (int move = 0; move < 2; ++move) {
        void (*fn)(Cpu *) = move ? hle_memmove : hle_memcpy;
        for (int reverse = 0; reverse < 2; ++reverse) {
            for (int i = 0; i < 16; ++i) a[i] = expected[i] = (uint8_t)i;
            unsigned dst = reverse ? 0 : 2, src = reverse ? 2 : 0;
            memmove(expected + dst, expected + src, 10);
            assert(call(fn, a + dst, a + src, 10) == (uintptr_t)(a + dst));
            assert(!memcmp(a, expected, sizeof a));
            call(fn, a, b, 0); assert(!memcmp(a, expected, sizeof a));
        }
    }
    memset(a, 0x11, sizeof a);
    assert(call(hle_memset, a + 2, (void *)(uintptr_t)0x1FE, 4) == (uintptr_t)(a + 2));
    assert(a[1] == 0x11 && a[2] == 0xFE && a[5] == 0xFE && a[6] == 0x11);
    uint8_t x[] = {0, 128, 255}, y[] = {0, 128, 2};
    assert(call(hle_memcmp, x, y, 2) == 0 && call(hle_bcmp, x, y, 2) == 0);
    assert((int64_t)call(hle_memcmp, x, y, 3) == 253);
    assert((int64_t)call(hle_memcmp, y, x, 3) == -253);
    assert(call(hle_bcmp, x, y, 3) == 1 && call(hle_memcmp, x, y, 0) == 0);
    assert(call(hle_memchr, b, (void *)(uintptr_t)0x1FF, 2) == 0);
    assert(call(hle_memchr, b, (void *)(uintptr_t)0x1FF, 3) == (uintptr_t)(b + 2));
    assert(call(hle_memchr, b, NULL, 5) == (uintptr_t)b && call(hle_memchr, b, NULL, 0) == 0);
    char s[] = {'a', (char)255, 0, 'x'}, t[] = {'a', (char)128, 0, 'y'};
    assert(call(hle_strlen, s, NULL, 0) == 2);
    assert((int64_t)call(hle_strcmp, s, t, 0) == 127);
    assert((int64_t)call(hle_strcmp, t, s, 0) == -127);
    assert(call(hle_strcmp, s, s, 0) == 0);
    assert(call(hle_strncmp, s, t, 0) == 0 && call(hle_strncmp, s, t, 1) == 0);
    assert((int64_t)call(hle_strncmp, s, t, 2) == 127);
    t[1] = (char)255;
    assert(call(hle_strncmp, s, t, 4) == 0); // stop at NUL before the different tail
    puts("PASS: shared SDK libc byte semantics, overlap, bounds and guest return registers");
}
