#include <assert.h>
#include "../runtime/hle.h"
#undef HLE_FUNC
#define HLE_FUNC(sym, fn) static void fn(Cpu *c)
#include "../runtime/hle/touch.c"

int main(void) {
    struct { uint64_t before; TouchSample sample[2]; uint64_t after; } guarded;
    Cpu cpu = {0};
    touch_reset();
    for (int i = 0; i < 1024; i++) {
        memset(&guarded, 0xcd, sizeof guarded);
        cpu.x[0] = (uint64_t)(uintptr_t)guarded.sample;
        cpu.x[1] = i % 2 ? 16 : 1;
        touch_states(&cpu);
        assert(cpu.x[0] == 1 && guarded.sample[0].sampling == (uint64_t)i + 1);
        assert(guarded.sample[0].count == 0 && guarded.sample[0].reserved == 0);
        for (size_t j = 0; j < sizeof guarded.sample[0].touches; j++)
            assert(((unsigned char *)guarded.sample[0].touches)[j] == 0);
        // One returned record, even when the caller permits more: no overwrite
        // of a second record or either outer canary.
        for (size_t j = 0; j < sizeof guarded.sample[1]; j++)
            assert(((unsigned char *)&guarded.sample[1])[j] == 0xcd);
        assert(guarded.before == UINT64_C(0xcdcdcdcdcdcdcdcd));
        assert(guarded.after == UINT64_C(0xcdcdcdcdcdcdcdcd));
    }
    for (int capacity = -1; capacity <= 0; capacity++) {
        memset(&guarded, 0xcd, sizeof guarded);
        cpu.x[0] = (uint64_t)(uintptr_t)guarded.sample; cpu.x[1] = capacity;
        touch_states(&cpu); assert(cpu.x[0] == 0);
        for (size_t i = 0; i < sizeof guarded; i++) assert(((unsigned char *)&guarded)[i] == 0xcd);
    }
    cpu.x[0] = 0; cpu.x[1] = 1; touch_states(&cpu); assert(cpu.x[0] == 0);
    touch_reset(); cpu.x[0] = (uint64_t)(uintptr_t)guarded.sample; touch_state(&cpu);
    assert(guarded.sample[0].sampling == 1 && guarded.sample[0].count == 0);
    puts("PASS: empty touchscreen samples, poisoned output, capacity bounds, canaries and reboot");
}
