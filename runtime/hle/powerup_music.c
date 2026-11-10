// Keep the game's music-only sound-disable bit local: buff inventory and gameplay effects stay unchanged.
#include "../game_addresses.h"
#include "../hle.h"
#include "../settings.h"

static bool g_powerup_music = true;
RT_ON_RESET(powerup_music_reset) { g_powerup_music = g_settings.powerup_music; }

uint32_t rt_powerup_music_flags(Cpu *c, uint32_t flags, bool update) {
    bool on = g_settings.powerup_music;
    if (update && g_powerup_music != on) {
        g_powerup_music = on;
        Cpu saved = *c;
        if (!on) {
            // The game's cleanup restores volume on existing handles; it doesn't restart the area's track.
            guest_call(c, GA(BUFF_MUSIC_STOP));
        } else if (!(flags & 0x10000)) {
            c->x[0] = 0x70; c->x[1] = 1; c->x[2] = UINT32_MAX;
            guest_call(c, GA(HAS_ITEM));
            bool active = c->x[0] & 1;
            if (!active) {
                c->x[0] = 0x6f; c->x[1] = 1; c->x[2] = UINT32_MAX;
                guest_call(c, GA(HAS_ITEM));
                active = c->x[0] & 1;
            }
            if (active) {
                c->x[0] = 0;  // Resume the loop, without the pickup intro.
                guest_call(c, GA(BUFF_MUSIC_START));
            }
        }
        *c = saved;
    }
    return on ? flags : flags | 0x10000;
}
