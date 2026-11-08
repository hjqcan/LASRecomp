// User settings (larecomp.ini next to the executable's working directory), edited from the in-game menu.
#pragma once
#include "cosmetic_colour.h"
#include <stdbool.h>

enum { ASPECT_UNLOCKED = 0, ASPECT_ORIGINAL = 1 };
enum { RES_MATCH = 0, /* 1..8 = 1x..8x */ RES_DYNAMIC = 9, RES_Q75 = 10, RES_Q25 = 11, RES_Q50 = 12 };
// (11 was 67% of the display: larecomp.ini "67%" now loads as RES_Q75)
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1 /* macOS */, UPSCALE_FSR1 = 2 /* Windows: AMD FSR 1 */ };
#ifdef __APPLE__
#define UPSCALE_SHARP UPSCALE_METALFX  // each OS's non-bilinear upscaler (the default)
#else
#define UPSCALE_SHARP UPSCALE_FSR1
#endif
enum { AA_OFF = 0, AA_SMAA = 1 };
enum { AO_SWITCH = 0, AO_HIGH = 1, AO_OFF = 2 };  // the game's ambient occlusion: the Switch's size, up to twice it (output_scale_ao), or none
enum { PROMPTS_AUTO = 0, PROMPTS_NINTENDO = 1, PROMPTS_XBOX = 2, PROMPTS_PLAYSTATION = 3 };
// Which face button confirms in our own menus (runtime/ui/pad_nav.mm); the game's input is never affected
enum { MENU_CONFIRM_GAME = 0, MENU_CONFIRM_CONTROLLER = 1 };
enum { DPAD_NOTHING = 0, DPAD_ITEMS = 1, DPAD_MOVE = 2 };
enum { TIMER_POS_LEFT = 0, TIMER_POS_RIGHT = 1 };  // the run timer's corner (bottom left / right)
enum { CHAOS_LETTERS_OFF = 0, CHAOS_LETTERS_EXPLORED = 1, CHAOS_LETTERS_MAP = 2 /* explored, all with the Map */ };
// System language served to the game (runtime/services/am.c GetDesiredLanguage, services/hid_set.c GetLanguageCode):
// the game picks its text language from it, as it would from the Switch's own system setting, and the menus follow it
// too (runtime/ui/i18n.mm). LANG_AUTO follows the host's language (macOS / Windows: the user's preferred languages;
// Linux: $LC_ALL / $LC_MESSAGES / $LANG); the rest are the codes of the languages the game has text for.
// The ones from N_GAME_LANGUAGES on are the menus' only (pt-BR): the game has no text for them and is told en-US.
enum { LANG_AUTO = 0, N_GAME_LANGUAGES = 12, N_LANGUAGES = 13 };
extern const char *const g_language_codes[N_LANGUAGES];  // what the game is told: "ja", "en-US", "es", ...
extern const char *const g_language_names[N_LANGUAGES];  // ("Auto", "Japanese", ..., "Spanish")
extern const char *const g_language_endonyms[N_LANGUAGES];  // each in its own language, for the menu ("日本語", "Deutsch")

enum { RANDO_PRESETS_MAX = 20 };  // (the Randomizer tab's own presets: settings.rando_presets)

enum { AUDIO_OUT_STEREO, AUDIO_OUT_51, N_AUDIO_OUTS };
enum { FPS_OFF, FPS_TOP_LEFT, FPS_TOP_RIGHT, FPS_BOTTOM_LEFT, FPS_BOTTOM_RIGHT, N_FPS_CORNERS };

typedef struct {
    int aspect;          // ASPECT_*: unlocked = fill the window (wider/taller views), original = 16:9 with bars
    bool fullscreen;
    int res_mode;        // RES_MATCH, 1..4 (N x the Switch's 1080p), RES_DYNAMIC, RES_Q75/50/25 (share of the display)
    bool res_auto;       // never chosen: the default for this GPU (settings_resolve_defaults); saved as "auto"
    int upscaler;        // UPSCALE_*: how a lower internal resolution reaches the window (MetalFX: macOS, FSR 1: Windows)
    bool upscaler_auto;  // never chosen: the default for this GPU (settings_resolve_defaults); saved as "auto"
    int antialias;       // AA_*: anti-aliasing of the game image before it is scaled to the window
    int ao_quality;      // AO_*: the resolution of the game's ambient occlusion
    float volume;        // master volume, 1 = 100%
    int audio_out;       // AUDIO_OUT_*: speaker layout of the game's sound (runtime/services/audio.c)
    bool reverb;         // room reverb effect
    bool movie_audio;
    bool powerup_music;  // Guardian Acorn / Piece of Power music (hle/powerup_music.c)
    bool menu_sounds;    // LAS Recomp's own menu sounds (runtime/menu_sfx.c)
    bool frame_interp;   // Frame Generation: extra images between the game's frames (runtime/nvn/interp.c)
    bool metal_hud;      // macOS: Apple's Metal Performance HUD over the game (runtime/nvn/metal.m metal_hud_apply)
    int show_fps;        // FPS_*: the frame rate counter's corner (FPS_OFF: none)
    int dpad_mode;       // DPAD_*: what the d-pad (and arrow keys) do in play: nothing, items (runtime/hle/dpad_items.c), move Link
    bool rumble;         // controller vibration from the game (runtime/hle/input.c)
    int rumble_strength; // its intensity, percent (25 / 50 / 75 / 100)
    bool controller_light; // the controller's light bar shows Link's health (runtime/led.h, hle/input.c)
    bool bg_input;       // controllers keep working while the window isn't in front (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS)
    bool move_360;       // analog movement in any direction (the game snaps the stick to 8): runtime/hle/qol.c
    bool blur_removal;   // no tilt-shift blur at the top / bottom of the screen
    bool hide_hero_icon; // no Hero Mode icon on the HUD
    bool pause_unfocused; // the game stops while its window isn't in front (runtime/hle/vi.c)
    bool remember_window; // the window opens at the size it was closed at (runtime/paths.c rt_window_remember)
    int window_w, window_h; // that size, in points (0: none saved yet)
    bool mirror_mode;    // the world mirrored left to right (Gameplay tab): renderers + runtime/hle/input.c
    int language;        // LANG_AUTO or an index into g_language_codes: the game's text language
    int prompts;         // PROMPTS_*: button glyphs (auto = follow the controller SDL reports): runtime/nvn/texpatch.c
    int menu_confirm;    // MENU_CONFIRM_*: game layout (right confirms, bottom goes back) or the controller's convention
    bool face_by_label;  // Xbox / PlayStation pads: the game's A/B/X/Y follow the buttons' labels, not the Switch's
                         // positions (bottom = A, right = B, left = X, top = Y): runtime/hle/bindings.c bind_pad_host
    bool inv_keys;       // randomizer: show every dungeon's keys on the pause menu's Items page
    bool tracker;        // randomizer: check tracker on the pause menu's Map page
    int tracker_filters; // its filter bar (runtime/ui/tracker_overlay.mm bits), -1 = the defaults; no menu row
    int chaos_letters;   // CHAOS_LETTERS_*: room-shuffle doorway letters on the dungeon map
    bool aspect_asked;   // the start screen's one-time "authentic 16:9 or unlocked?" question was answered
    bool menu_tip_seen;  // the start screen's one-time "how to open the menu" note was shown (first PLAY ever)
    bool onedrive_kept;  // the start screen's OneDrive warning (Windows) was answered with Keep here
    // cheats (runtime/ui/cheats.mm), applied every frame while a save is loaded
    bool cheat_hearts;   // health kept full
    bool cheat_ammo;     // bombs / arrows / magic powder kept full (for the bags Link owns)
    bool cheat_rupees;   // rupees kept at 9999
    bool cheat_hookshot; // hookshot latches onto any surface (Gameplay tab; runtime/hle/hookshot.c; seeds made with it: always)
    bool no_buff_drops;  // no Guardian Acorns / Pieces of Power (Gameplay tab; runtime/hle/qol.c; seeds made with it: always)
    // difficulty (runtime/ui/difficulty.mm)
    float diff_damage;   // damage taken multiplier (1 = normal)
    float walk_speed;    // Link's walking speed multiplier (1 = normal; Gameplay tab, runtime/hle/qol.c)
    bool walk_speed_dungeons;  // walk speed also indoors: houses, caves, dungeons (Gameplay tab, under Walk speed)
    bool diff_ohko;      // any hit kills
    bool diff_hero;      // Hero Mode flag forced on in the loaded save (2x damage, no recovery hearts)
    // cosmetics (Cosmetics tab, runtime/ui/menu.mm), live, never part of a seed: colour rows of the shared palette
    // (runtime/cosmetic_colour.h; Default = the game's own, for the tunic also the seed's)
    CosColour tunic, hearts;  // runtime/nvn/texpatch.c (tunic map, heart tints)
    CosColour hair, shield;   // runtime/nvn/recolour_link.c
    CosColour skin;           // runtime/nvn/texpatch.c skin_apply
    CosColour leaves, grass;  // the overworld's foliage, runtime/nvn/recolour_seasons.c
    CosColour sand;           // the beaches, the desert, the sandy south (recolour_seasons.c)
    CosColour water;          // the sea, rivers, ponds, dungeon water (runtime/nvn/recolour_water.c)
    CosColour fences, walls, houses;  // the overworld's wooden fences, cliff walls, houses (recolour_buildings.c)
    int enemy_colours;        // 0 Off, 1 Random per seed, 2 Random every load (runtime/nvn/recolour_enemies.c)
    int screen_filter;        // 0 Off, 1 Game Boy, 2 Game Boy Color, 3 Grayscale (runtime/nvn/screen_filter.h)
    int weather;              // WEATHER_* (runtime/nvn/weather.h): rain / snow / storm / blizzard on the overworld, looks only
    char cosmetics_seed[300];  // the seed folder whose own cosmetics were last copied into the rows (cosmetics_seed.c)
    // speedrun (Gameplay tab; runtime/hle/run_timer.c, runtime/livesplit.c)
    bool timer;            // the in-game run timer in the corner (game time: level loads don't count)
    int timer_pos;         // TIMER_POS_*: which bottom corner it sits in
    bool livesplit;        // drive LiveSplit (its Server component) over TCP: start, splits, game time
    char livesplit_host[64];  // host:port of the LiveSplit Server ("localhost:16834")
    // touch screens (runtime/ui/mobile/touch_controls.mm)
    int touch_mode;      // 0 Auto (while no controller is connected), 1 Always, 2 Off
    bool touch_stick_fixed;  // the touch stick stays in its place (else its base comes to the thumb)
    bool touch_all;      // every play button on screen, not only those the game uses now
    bool touch_hide_xy_hud;  // the game's own X / Y item HUD hidden while the on-screen controls are shown
    float touch_opacity; // 0.15 .. 1
    float touch_size;    // 0.6 .. 1.6 (1 = the default layout)
    // the layout editor (touch_controls.mm): per control, moved (its place: share of the screen), size (x), hidden;
    // saved as touch_ctl_<name> = x, y, size, hidden
#define TOUCH_NCTL 13
    bool touch_moved[TOUCH_NCTL];
    float touch_pos[TOUCH_NCTL][2];
    float touch_scale[TOUCH_NCTL];
    bool touch_hidden[TOUCH_NCTL];
    char rando_seed[128];  // active randomizer seed (rando/<name>/), empty = vanilla
    char rando_options[48];  // the Randomizer tab's New seed options: a seed ID without its random part (empty = defaults)
    // the player's own randomizer presets (rando_preset lines): "<name>|<options key>|<tracker><map letters>", "" = free
    char rando_presets[RANDO_PRESETS_MAX][100];
} Settings;


enum { N_ENEMY_COLOUR_MODES = 3, N_SCREEN_FILTERS = 4, N_WEATHERS = 6 };
extern const char *const g_screen_filters[N_SCREEN_FILTERS];  // (the menu's names)
extern const char *const g_audio_outs[N_AUDIO_OUTS];  // (the menu's names: Stereo, 5.1)
extern const char *const g_weathers[N_WEATHERS];  // (the menu's names: Off, Rain, Snow, Storm, Blizzard, Random)
extern const char *const g_enemy_colour_modes[N_ENEMY_COLOUR_MODES];  // (the menu's names)

extern Settings g_settings;

const char *touch_control_name(int i);  // the on-screen controls' ini names (touch_ctl_<name>), "" past TOUCH_NCTL

void settings_load(void);
void settings_save(void);
// At device init: the defaults that depend on the GPU. integrated (Vulkan: an integrated GPU; never on macOS): plain
// bilinear upscaling instead of the sharp upscaler, whose pass at the window's size costs as much as the game's frame
// there (UHD 770: FSR 1 + SMAA 8 ms of GPU per frame at 1080p, 20 ms at 4K), and Dynamic resolution instead of Match
// display. A player's own choice is kept. Anti-aliasing is off by default everywhere. LARECOMP_INTEGRATED=0/1 (test):
// treat the GPU as dedicated / integrated.
void settings_resolve_defaults(bool integrated);
extern int g_default_upscaler;  // UPSCALE_*: what the menu shows as the default
extern int g_default_res_mode;  // RES_*: likewise
// An integrated GPU (as settings_resolve_defaults was told): the fixed 1x renders 1280x720 there, what the game itself
// picked on such a GPU (runtime/nvn/output_scale.c)
extern bool g_gpu_integrated;
// The language code to serve the game ("es", "en-US", ...): the setting, or the host locale for LANG_AUTO (en-US for a
// language of the menus only)
const char *settings_language(void);
// The menus' language code: the setting's, or the host locale's (pt-BR too, which the game itself doesn't have)
const char *settings_menu_language(void);
// That code's name for the menu ("Spanish")
const char *settings_language_name(void);
