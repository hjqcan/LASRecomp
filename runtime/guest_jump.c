#include "guest_jump.h"
#include "rt.h"

typedef struct JumpPoint {
    uint64_t buffer, sp, pc;
    struct JumpPoint *next;
} JumpPoint;

__thread RtGuestJumpScope *rt_guest_jump_head;

RtGuestJumpScope *rt_guest_jump_enter(Cpu *c) {
    RtGuestJumpScope *s = calloc(1, sizeof *s);
    if (!s) FATAL("guest jump checkpoint allocation failed\n");
    s->cpu = c;
    s->previous = rt_guest_jump_head;
    rt_guest_jump_head = s;
    return s;
}

void rt_guest_jump_mark(RtGuestJumpScope *s, uint64_t buffer, uint64_t sp, uint64_t pc) {
    JumpPoint *p = s->points;
    for (; p && p->buffer != buffer; p = p->next) {}
    if (!p) {
        p = malloc(sizeof *p);
        if (!p) FATAL("guest jump point allocation failed\n");
        p->next = s->points;
        s->points = p;
    }
    p->buffer = buffer; p->sp = sp; p->pc = pc;
}

static void free_scope(RtGuestJumpScope *s) {
    JumpPoint *p = s->points;
    while (p) { JumpPoint *next = p->next; free(p); p = next; }
    free(s);
}

void rt_guest_jump_leave(RtGuestJumpScope *s) {
    if (s != rt_guest_jump_head) FATAL("guest jump checkpoint nesting mismatch\n");
    rt_guest_jump_head = s->previous;
    free_scope(s);
}

void rt_guest_jump_resume(Cpu *c) {
    for (RtGuestJumpScope *s = rt_guest_jump_head; s; s = s->previous) {
        if (s->cpu != c) continue;
        for (JumpPoint *p = s->points; p; p = p->next) {
            if (p->pc != c->pc || p->sp != c->sp) continue;
            // The guest longjmp has already restored the CPU, including its return value.
            // Drop checkpoints belonging to skipped host frames before returning to this one.
            while (rt_guest_jump_head != s) rt_guest_jump_leave(rt_guest_jump_head);
            longjmp(s->host, 1);
        }
    }
}

void rt_guest_jump_reset(Cpu *c) {
    // Kernel thread exit/reboot uses a separate host longjmp and skips ordinary function exits.
    RtGuestJumpScope **at = &rt_guest_jump_head;
    while (*at) {
        RtGuestJumpScope *s = *at;
        if (s->cpu == c) { *at = s->previous; free_scope(s); }
        else at = &s->previous;
    }
}
