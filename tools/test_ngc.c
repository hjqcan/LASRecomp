// Check the local fallback's public ABI, Unicode preservation and buffer bounds.
#include <assert.h>
#include "../runtime/hle/ngc.c"

int main(void) {
    uint64_t object[8] = {0}; uint8_t work[0x20000];
    Cpu c = {0}; c.x[0] = (uint64_t)(uintptr_t)object;
    c.x[1] = (uint64_t)(uintptr_t)work; c.x[2] = sizeof work; c.x[3] = 1;
    ngc_init(&c); assert(!c.x[0]);
    uint16_t name[] = {0x6d77,0x5c9b,0};
    uint16_t bad[] = {'S','H','I','T',0};
    uint16_t ordinary[] = {'s','h','i','t','a','k','e',0};
    uint64_t words[] = {(uint64_t)(uintptr_t)name,(uint64_t)(uintptr_t)bad,(uint64_t)(uintptr_t)ordinary};
    uint32_t out[4] = {0,0,0,0xcafebabe};
    c.x[0] = (uint64_t)(uintptr_t)object; c.x[1] = (uint64_t)(uintptr_t)out;
    c.x[2] = 0xffff; c.x[3] = (uint64_t)(uintptr_t)words; c.x[4] = 3;
    ngc_check_utf16(&c);
    assert(!c.x[0] && !out[0] && out[1] == 0xffff && !out[2] && out[3] == 0xcafebabe);
    c.x[0] = (uint64_t)(uintptr_t)object; c.x[4] = 17;
    ngc_check_utf16(&c); assert(c.x[0] == NGC_SIZE && out[3] == 0xcafebabe);
    uint16_t text[] = {0x5c9b,' ','s','h','i','t','!',0}; int masked = -1;
    c.x[0] = (uint64_t)(uintptr_t)object; c.x[1] = (uint64_t)(uintptr_t)&masked;
    c.x[2] = (uint64_t)(uintptr_t)text; ngc_mask_utf16(&c);
    assert(!c.x[0] && masked == 1 && text[0] == 0x5c9b && text[2] == '*' && text[5] == '*' && text[6] == '!');
    c.x[0] = (uint64_t)(uintptr_t)object; ngc_destroy_1(&c);
    c.x[0] = (uint64_t)(uintptr_t)object; ngc_check_utf16(&c); assert(c.x[0] == NGC_UNINITIALIZED);
    puts("PASS: local text filter ABI, UTF-16 names, masking, count bounds and destruction");
}
