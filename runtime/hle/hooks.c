// Hook points: instructions of the game's code that call into the runtime (tools/patches.py HOOKS). The generated
// code at a hook site runs `rt_hook(c, site)` first; what a hook does lives here, so changing it rebuilds the app,
// not the game code.
//
// rt_hook returns 0: run the site's own instruction, 1: the hook did it all, 2..: jump to the site's targets
// (HOOKS[site][1], in order).
#include "../game_addresses.h"
#include <stdio.h>
#include <string.h>

#include "../cpu.h"
#include "../rt.h"

void rt_mb_seen(uint64_t o);  // miniboss.c
int rt_shadow_link_appear(uint64_t o);
int rt_gohma_free(void);
uint32_t rt_gohma_heading(uint64_t gohma, uint32_t track);
int rt_crashout_skip(void);
void rt_dig_hole(Cpu *c);  // digs.c
uint64_t rt_hole_cap(void);
void rt_frame_job_kickoff(uint64_t owner);  // delegate_null.c
void rt_frame_job_finish(uint64_t job);

int rt_hook(Cpu *c, uint64_t site) {
    switch (site) {
    case GA_MB_SEEN_SITE:  // MB_SEEN_SITE - ldr x8, [x21]: an actor seen
        rt_mb_seen(c->x[21]);
        return 0;
    case GA_SHADOW_LINK_APPEAR_SITE:  // SHADOW_LINK_APPEAR_SITE - ldr x9, [x0, #0x230]: his Appear event, none on the Field
        return rt_shadow_link_appear(c->x[0]) ? 2 : 0;
    case GA_CRASHOUT_SITE:  // CRASHOUT_SITE - str wzr, [x0, #0xb8]: Blaino's knock-out, no crash-out on the Field
        if (!rt_crashout_skip()) return 0;
        ST32(c->x[0] + 0xb8, 0);
        return 2;
    case GA_GOHMA_HEADING_SITES_0:  // GOHMA_HEADING_SITES - str w8, [x19, #0x15ec]: heading at Link on the Field
    case GA_GOHMA_HEADING_SITES_1:
        c->x[8] = rt_gohma_heading(c->x[19], (uint32_t)c->x[8]);
        return 0;
    case GA_GOHMA_ARRIVED_SITE:  // GOHMA_ARRIVED_SITE - ldr q0, [x19, #0x1600]: no track end on the Field
        if (!rt_gohma_free()) return 0;
        c->x[0] = 0;
        return 2;
    case GA_MSTALFOS_BODY_SITE:  // MSTALFOS_BODY_SITE - mov w8, #1: body collision off on the Field
        c->x[8] = rt_gohma_free() ? 0 : 1;
        return 1;
    case GA_MSTALFOS_JUMP_SITE:  // MSTALFOS_JUMP_SITE - fsub s0, s0, s1: jump arc from his takeoff height on the Field
        if (rt_gohma_free()) {
            float s0;
            memcpy(&s0, &c->v[0], 4);
            s0 += *(const float *)(uintptr_t)(LD64(c->x[19] + 0x1160) + 0x28c);
            __uint128_t r = 0;
            memcpy(&r, &s0, 4);
            c->v[0] = r;
        }
        return 0;
    case GA_MCHIEF_STORY_SITE:  // MCHIEF_STORY_SITE - tbnz w8, #7: no story check on the Field
        return rt_gohma_free() ? 2 : 0;
    case GA_DIG_HOLE_SITE:  // DIG_HOLE_SITE - ldr x22, [sp, #8]: a hole was dug
        rt_dig_hole(c);
        return 0;
    case GA_HOLE_CAP_SITES_0:  // HOLE_CAP_SITES - cmp x8, #0x51 / cmp x8, #0x50: dug-hole limit
        (void)subs64(c, c->x[8], rt_hole_cap());
        return 1;
    case GA_HOLE_CAP_SITES_1:
        (void)subs64(c, c->x[8], rt_hole_cap() - 1);
        return 1;
    case GA_EMPTY_HANDLER_SITE:  // EMPTY_HANDLER_SITE - bl main+0x108dfc0: an empty std::function would throw (issue #41)
        if (c->x[0]) return 0;
        LOG("[event] empty handler skipped (issue #41)\n");
        return 2;  // (the dispatcher's catch: returns 0)
    case GA_FRAME_JOB_KICKOFF_SITES_0:  // FRAME_JOB_KICKOFF_SITES - a frame job's rebuild waits for its last finish (#19)
    case GA_FRAME_JOB_KICKOFF_SITES_1:
    case GA_FRAME_JOB_KICKOFF_SITES_2:
        rt_frame_job_kickoff(c->x[22]);
        return 0;
    case GA_FRAME_JOB_KICKOFF_SITES_3:
        rt_frame_job_kickoff(c->x[21]);
        return 0;
    case GA_FRAME_JOB_FINISH_SITE:  // FRAME_JOB_FINISH_SITE - ldr x8, [x0]: a frame job's finish (debug delay)
        rt_frame_job_finish(c->x[0]);
        return 0;
    }
    return 0;
}
