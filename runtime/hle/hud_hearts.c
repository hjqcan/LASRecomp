// HUD hearts refresh: the HUD keeps its own copy of Link's health and max health (HUD +0x61c0 / +0x61c2), set when it
// is set up (main+0xea84f0, its vtable slot 6, on every screen load) and then followed one quarter heart at a time; a
// new heart frame only appears as the health fills past the old max (a Heart Container's refill: one frame per fill).
// The Cheats tab and the Save Editor change the hearts at once, so they ask here for the HUD's own set-up to run again,
// as a screen change would: HUD_HEARTS_SITE (tools/patches.py, the prologue of the HUD's normal state update,
// main+0xeaa800, x0 = the HUD) runs it on the game thread the next time that state updates.
#include "../game_addresses.h"

#include "../hle.h"

static volatile int g_refresh;

RT_ON_RESET(hud_hearts_reset) { g_refresh = 0; }

// (any thread: the menus) redraw the HUD's hearts from the save
void hud_hearts_refresh(void) { g_refresh = 1; }

void rt_hud_hearts_tick(Cpu *c) {
    extern void rt_hud_corners_tick(uint64_t hud);
    rt_hud_corners_tick(c->x[0]);  // (runtime/hle/hud_corners.c)
    if (!g_refresh) return;
    g_refresh = 0;
    Cpu saved = *c;
    guest_call(c, GA(HUD_SETUP));  // (x0: the HUD)
    *c = saved;
    LOG("[hud] hearts redrawn\n");
}
