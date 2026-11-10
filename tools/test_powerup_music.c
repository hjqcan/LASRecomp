// Exercise the real music policy; guest calls stand in for the game's audio and inventory routines.
#include <assert.h>
#include "../runtime/hle/powerup_music.c"

Settings g_settings = {.powerup_music = true};
static const GameAddrs addresses = {.BUFF_MUSIC_START = 1, .BUFF_MUSIC_STOP = 2, .HAS_ITEM = 3};
const GameAddrs *g_ga = &addresses;
static unsigned starts, stops;
static bool power, acorn;

void guest_call(Cpu *c, uint64_t address) {
    uint64_t result = 0;
    if (address == GA(HAS_ITEM)) {
        assert(c->x[1] == 1 && c->x[2] == UINT32_MAX);
        assert(c->x[0] == 0x70 || c->x[0] == 0x6f);
        result = c->x[0] == 0x70 ? power : acorn;
    } else if (address == GA(BUFF_MUSIC_START)) {
        assert(c->x[0] == 0);  // Re-enable the loop, without replaying the pickup intro.
        starts++;
    } else {
        assert(address == GA(BUFF_MUSIC_STOP));
        stops++;
    }
    memset(c, 0xa5, sizeof *c);
    c->x[0] = result;
}

int main(void) {
    Cpu cpu, before;
    memset(&cpu, 0x5a, sizeof cpu);
    before = cpu;
    const uint32_t flags[] = {0, 0x8000, 0x10000, 0x20000, UINT32_MAX};
    for (int on = 0; on <= 1; on++) {
        g_settings.powerup_music = on;
        powerup_music_reset();
        for (size_t i = 0; i < sizeof flags / sizeof *flags; i++) {
            uint32_t original = flags[i];
            assert(rt_powerup_music_flags(&cpu, original, false) == (on ? original : original | 0x10000));
            assert(flags[i] == original && !memcmp(&cpu, &before, sizeof cpu));
        }
    }
    assert(!starts && !stops);
    // Disabling a currently playing buff restores the existing normal-music handle once.
    g_settings.powerup_music = true;
    powerup_music_reset();
    g_settings.powerup_music = false;
    assert(rt_powerup_music_flags(&cpu, 0x8000, true) == 0x18000);
    rt_powerup_music_flags(&cpu, 0, true);
    assert(stops == 1 && !starts && !memcmp(&cpu, &before, sizeof cpu));
    // Either buff, or both together, resumes the loop on re-enable; no buff starts nothing.
    for (unsigned buffs = 0; buffs < 4; buffs++) {
        power = buffs & 1;
        acorn = buffs & 2;
        for (int sound_disabled = 0; sound_disabled <= 1; sound_disabled++) {
            g_settings.powerup_music = false;
            powerup_music_reset();
            g_settings.powerup_music = true;
            unsigned old = starts;
            uint32_t f = sound_disabled ? 0x10000 : 0;
            assert(rt_powerup_music_flags(&cpu, f, true) == f);
            rt_powerup_music_flags(&cpu, f, true);
            assert(starts == old + (buffs != 0 && !sound_disabled));
            assert(!memcmp(&cpu, &before, sizeof cpu));
        }
    }
    // A reboot adopts the current setting without carrying a pending audio transition.
    g_settings.powerup_music = false;
    powerup_music_reset();
    rt_powerup_music_flags(&cpu, 0, true);
    assert(stops == 1);
    puts("Power-up music flags, live toggles, buff combinations, CPU preservation and reset passed");
}
