// Host checkpoints for guest setjmp callers. The guest's own setjmp/longjmp still
// saves/restores its registers and buffer layout; this only unwinds the host C stack.
#pragma once
#include <setjmp.h>
#include "cpu.h"

typedef struct RtGuestJumpScope {
    jmp_buf host;
    struct RtGuestJumpScope *previous;
    Cpu *cpu;
    void *points;
} RtGuestJumpScope;

extern __thread RtGuestJumpScope *rt_guest_jump_head;
RtGuestJumpScope *rt_guest_jump_enter(Cpu *c);
void rt_guest_jump_mark(RtGuestJumpScope *scope, uint64_t buffer, uint64_t sp, uint64_t pc);
void rt_guest_jump_leave(RtGuestJumpScope *scope);
void rt_guest_jump_resume(Cpu *c);
void rt_guest_jump_reset(Cpu *c);
