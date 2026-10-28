// The settings menu's sounds (in game and on the start screen): the game's own menu sounds, read from the player's
// extracted files (romfs/region_common/audio/Richard.bfsar) and played on an audio stream of their own
// (runtime/menu_sfx.c). Missing or unreadable files: silent (one log line). LARECOMP_NO_AUDIO=1: no sound at all.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// (in rising importance: a sound drops a less important one started just before it, and is dropped after a more
// important one - one press, one sound)
enum { MENU_SFX_CURSOR, MENU_SFX_TAB, MENU_SFX_DECIDE, MENU_SFX_BEEP, MENU_SFX_CANCEL, MENU_SFX_OPEN, MENU_SFX_CLOSE,
       MENU_SFX_N };

void menu_sfx_play(int id);  // any thread
// The selected installation's game folder (its romfs/ holds the sounds); a change drops the previous sound cache.
void menu_sfx_set_game_dir(const char *dir);
// After an ImGui frame of the menu (its thread, before ImGui::Render): the controller's cursor moving plays the cursor
// sound, A / Enter / a click on something the decide sound, B / Esc (back_ok) the cancel sound.
void menu_sfx_frame(bool back_ok);
// The item just drawn is a menu tab: a click or A on it plays the tab sound (as L / R), not decide. Each frame.
void menu_sfx_tab_item(void);
// Nothing played for a while: the stream pauses (menu_sfx_frame calls it).
void menu_sfx_idle_check(void);
// The test hook (LARECOMP_MENU_SFX_TEST=1, main.c): load the sounds now (LARECOMP_NO_AUDIO too) and log each one.
void menu_sfx_load_now(void);

#ifdef __cplusplus
}
#endif
