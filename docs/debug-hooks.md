# Debug and test hooks

LAS Recomp reads a set of `LARECOMP_*` environment variables. They are for development and testing, not for players:
scripted test runs, screenshots, warps, logging, and A/B switches for renderer and timing code. Most are read once, at
startup or at first use, so set them before launching. Hooks that take a frame number act at that presented frame
(the count starts at 0 when the app starts and goes on across in-game reboots).

This page follows the current development branch (`beta_<n>`), so a few hooks may be missing from older releases. It lists the hooks a contributor is likely to need. It is not complete: the full list is in the code
(`grep -rn 'getenv("LARECOMP_\|ENV("LARECOMP_' runtime/`), and the comment next to each read explains it.

Unless a row says otherwise, "1" means any value turns the hook on, and "0 / 1" means the value is read as a number
(0 off, anything else on). Paths are relative to the working directory. Test output goes to `build/`.

A typical test run, from a source checkout with the extracted game files in `extracted/`:

```sh
LARECOMP_NO_AUDIO=1 LARECOMP_NO_MOVIE_AUDIO=1 LARECOMP_SAVE_DIR=build/testsave \
LARECOMP_INPUT="400:A:6,520:A:6,900:ZL:8,900:ZR:8,1300:A:6,1600:A:6,1900:A:6,2200:A:6" \
LARECOMP_WARP_AT="2700:Field:Field_11C" tools/shot.sh 3200
```

Put a copy of a save in `build/testsave/save_0/` first (`savedata0.bin`), so the run never touches real saves.
`tools/shot.sh <frame> [max seconds]` runs `./build/lasrecomp extracted` with your `LARECOMP_*` variables, saves the
window at that frame to `build/window.png` and exits.

## Running and test runs

| Variable | Values | What it does |
|---|---|---|
| `LARECOMP_NO_LAUNCHER` | 1 / 0 | Skip the start screen and boot straight into the game. Where the start screen would come up later, the app exits instead. |
| `LARECOMP_FORCE_LAUNCHER` | 1 | Show the start screen even when `LARECOMP_INPUT` is set (which normally skips it). |
| `LARECOMP_LAUNCHER_AUTOPLAY` | 1, `once` | Press PLAY on the start screen at once. `once`: only the first time, so a "Title screen" reboot stays on the start screen. |
| `LARECOMP_LAUNCHER_PLAY_AT` | seconds | Press PLAY on the start screen that many seconds in. |
| `LARECOMP_NO_UPDATE_CHECK` | 1 | Skip the update check on the start screen. |
| `LARECOMP_INPUT` | script (see below) | Scripted controller input for player 1. Also skips the start screen and blocks your own input. |
| `LARECOMP_NO_USER_INPUT` | 0 / 1 | Ignore keyboard, mouse and controllers. On by default when a scripted input hook is set; `0` lets your input through during a scripted run. |
| `LARECOMP_HOST_INPUT` | script (see below) | Scripted host input that goes through the bindings and menus like a player's: keys, controller buttons, typing, mouse, window resize, screenshots. |
| `LARECOMP_BIND_TEST` | `frame:pad\|key:<input>:<host>,...` | Start a rebind capture of `<input>` (a `bind_*` name such as `a`, `zl`) at that frame and press `<host>` 90 frames later (`esc` cancels, `wait` times out). `frame:reset:pad\|key` resets to defaults. |
| `LARECOMP_KBD_AUTO` | text | Answer the game's keyboard dialog (name entry) with this text at once. |
| `LARECOMP_PAD_TYPE` | `xbox`, `playstation` (or `ps`), other = Nintendo | Pretend the controller is of this family (button labels and confirm/back layout). |
| `LARECOMP_SAVE_DIR` | folder | Use this folder for save slots (`save_0/savedata0.bin` ...) instead of the normal save folder. Test runs should always set it to a scratch copy. |
| `LARECOMP_SETTINGS` | file path | Use this settings file instead of `larecomp.ini`, so test runs never change the player's settings. |
| `LARECOMP_DATA_ROOT` | folder | Use this folder as the data folder (game files copy, settings, saves, caches). |
| `LARECOMP_NO_AUDIO` | 1 / 0 | No audio output at all (game and menu sounds). `0` keeps sound. |
| `LARECOMP_NO_MOVIE_AUDIO` | 1 | Play movies without their audio track. |
| `LARECOMP_WINDOW` | `WxH` | Window size in points (default: the remembered size, else 1280x720). Also stops fullscreen. |
| `LARECOMP_WINDOW_POS` | `x,y` | Window position. |
| `LARECOMP_HIDDEN` | 1 | Never show the window: renders off screen without taking focus. The game also keeps running when unfocused. |
| `LARECOMP_SCALE` | number | Force the render height in units of 1080 lines (`0.5` = 540 lines, `1` = 1080, `2` = 2160). Overrides the resolution setting. |
| `LARECOMP_WINDOW_SHOT` | frame | Save the final window image (game and menus) at that frame. |
| `LARECOMP_SHOT_PATH` | file path | Where `LARECOMP_WINDOW_SHOT` (and the start screen shot) is saved. Default `build/window.png`. |
| `LARECOMP_EXIT_AFTER_SHOT` | 1 | Exit right after the `LARECOMP_WINDOW_SHOT` image is saved. |
| `LARECOMP_SHOT_AT` | `frame,frame,...` (ascending) | Save the game image at each frame to `build/frames/frame_N.png`. |
| `LARECOMP_SHOTS` | `frame,frame,...` | Save the window at each frame to `build/shots_<SHOT_ID>_<frame>.png` (a filmstrip of one run). |
| `LARECOMP_SCREENSHOT_EVERY` | frames | Save the game image every N frames to `build/frame_N.png`. |
| `LARECOMP_REBOOT_AT` | `frame:<seed>\|vanilla\|title,...` | At that frame, reboot in place into that randomizer seed or vanilla, or go back to the start screen. |
| `LARECOMP_TEST_CRASH` | `frame:segv\|terminate\|fatal` | Crash on purpose at that frame, to test the crash dialog. |

### LARECOMP_INPUT format

A comma-separated list of `frame:BUTTON:length` entries. The button is held from `frame` for `length` frames (default
10 when the length is left out). Entries may overlap to hold several buttons at once.

- Buttons: `A B X Y L R ZL ZR PLUS MINUS LSTICK RSTICK` and the D-pad `LEFT UP RIGHT DOWN`.
- Left stick: `STICK_UP`, `STICK_DOWN`, `STICK_LEFT`, `STICK_RIGHT`, or `STICK_<degrees>` for any angle (`STICK_0` is
  right, `STICK_90` up, `STICK_45` up-right).
- Right stick: `RSTICK_UP`, `RSTICK_DOWN`, `RSTICK_LEFT`, `RSTICK_RIGHT`.

The sequence the test scripts use to get from boot into the first save slot:

```
LARECOMP_INPUT="400:A:6,520:A:6,900:ZL:8,900:ZR:8,1300:A:6,1600:A:6,1900:A:6,2200:A:6"
```

Then walk left for a second and swing the sword:

```
LARECOMP_INPUT="400:A:6,520:A:6,900:ZL:8,900:ZR:8,1300:A:6,1600:A:6,1900:A:6,2200:A:6,3000:STICK_LEFT:60,3070:B:6"
```

### LARECOMP_HOST_INPUT format

Comma-separated entries, each starting with a frame:

- `frame:key:<SDL key name>:frames`, `frame:pad:<button>:frames`: hold a key or controller button (names as in the
  `bind_*` lines of `larecomp.ini`, e.g. `Z`, `Keypad 0`, `south`, `ltrigger`).
- `frame:lstick:<x>/<y>:frames`: hold the left stick at raw SDL units.
- `frame:sdlkey:<key>:frames`, `frame:sdlpad:<button>:frames`, `frame:sdlaxis:<axis>=<value>:frames`: send real SDL
  events, which reach menus and the on-screen keyboard first.
- `frame:type:<text>`: type text as key events (`{Escape}` presses that key). No `,` or `:` in the text.
- `frame:mouse:<dx>/<dy>`, `frame:mouse:click`: move or click the mouse.
- `frame:shot:<name>`: save the window to `build/<name>.png`.
- `frame:resize:<W>x<H>`: resize the window.

## Warps, saves and seeds

| Variable | Values | What it does |
|---|---|---|
| `LARECOMP_WARP_AT` | `frame:level:locator,...` (see below) | Warp to that level and spawn point at each frame. |
| `LARECOMP_WARP_ON_LOAD` | `level:locator` | Warp once, about a second after a save is loaded. |
| `LARECOMP_WARP_MENU_PICK` | text | Warp through the menu's warp tab to the first place whose name matches the text. |
| `LARECOMP_PLACE` | `frame:x:z[:script]`, several joined with `;` | Put Link at world x, z once the level has been loaded for 60 frames, then run an optional action script (steps `delay:L\|R\|U\|D\|A\|B\|X\|Y:frames` joined with `/`). |
| `LARECOMP_PLACE_Y` | number | With `LARECOMP_PLACE`: also set the height. |
| `LARECOMP_DOORJUMP_TEST` | `frame:cell:side[:centre[:floor]]` | Put Link inside a dungeon doorway to test a door jump; 300 frames later log which cell he is in. See `tools/doorjump_test.sh`. |
| `LARECOMP_GIVE_ITEM` | `frame:id:count:index[:equip],...` | Give items through the game's own add-item function at that frame. |
| `LARECOMP_SET_FLAGS` | `i,j,...` or `i@frame` | Force those event flags (0-2047) on in the loaded save, optionally from a frame on. |
| `LARECOMP_CHEATS` | any of `hearts,ammo,rupees` | Turn those cheats on for this run (not saved). |
| `LARECOMP_RANDO_SEED` | seed folder name | Play this randomizer seed (`rando/<name>/`) instead of the one in the settings. Saves go to the seed's own folder unless `LARECOMP_SAVE_DIR` is set. |
| `LARECOMP_RANDO_ID` | seed ID | Open the randomizer tab with that seed ID's options. |
| `LARECOMP_RANDO_GEN_TEST` | `1` or `name[:seed ID]` | Generate a seed as soon as the randomizer tab is shown. |
| `LARECOMP_RANDO_GEN` | file path | Use this seed generator program instead of the bundled one. |

### LARECOMP_WARP_AT format

A comma-separated list of `frame:level:locator` entries in frame order. The level is the game's level name and the
locator is a spawn point in it, as listed in `runtime/ui/warp_data.h`. The warp happens on the first frame at or after
the given one, once a save is in play.

```
LARECOMP_WARP_AT="2700:Lv01TailCave:Lv01TailCave_08D"
LARECOMP_WARP_AT="2400:Field:Field_11C,4200:Field:Field_03E,6000:Field:Field_07M"
```

Use it with `LARECOMP_INPUT` (to get into a save) and `LARECOMP_SAVE_DIR` (so the run does not touch real saves).

## Gameplay features and menus

| Variable | Values | What it does |
|---|---|---|
| `LARECOMP_SETTING_AT` | `frame:key=value,...` | Change an in-game menu setting at that frame. Keys: `move_360`, `blur_removal`, `frame_interp` (0 / 1), `dpad_mode` (0-2), `enemy_colours` (0-2), `screen_filter` (0-3), and the colour rows `tunic`, `hearts`, `hair`, `shield`, `skin`, `leaves`, `grass`, `sand` (palette name or `#RRGGBB`). |
| `LARECOMP_COSMETICS` | `frame:tunic:hearts,...` | Set the tunic and heart colours (palette name or `#RRGGBB`) at that frame. |
| `LARECOMP_DAMAGE` | multiplier, or `ohko` | Override the damage taken setting. |
| `LARECOMP_WALK_SPEED` | multiplier | Override the walk speed setting. |
| `LARECOMP_MOVE_360` | 0 / 1 | Override the 360-degree movement setting. |
| `LARECOMP_HOOKSHOT_8WAY` | 0 / 1 | Override the hookshot 8-direction aim setting. |
| `LARECOMP_HOOKSHOT_LOG` | 1 | Log hookshot hits and each shot's direction before and after the 8-direction aim. |
| `LARECOMP_BLUR_REMOVAL` | 0 / 1 | Override the blur removal setting. |
| `LARECOMP_PROMPTS` | `xbox`, `playstation`, `nintendo`, other = auto | Force the button icon style. |
| `LARECOMP_NO_BUFF` | 0 / 1 | Force the "no buff drops" option on or off. |
| `LARECOMP_ESCAPE` | 0 / 1 | Force the Escape Bottle on or off (also in vanilla). |
| `LARECOMP_HINT_SHEET` | 0 / 1 | Force the Hint Sheet on or off. |
| `LARECOMP_ENEMY_RANDOM` | 0 | Turn a seed's enemy randomizer off. |
| `LARECOMP_WEATHER` | `off`, `rain`, `snow`, `storm`, `blizzard`, `random` | Override the weather setting. |
| `LARECOMP_WEATHER_CYCLE` | 0 / 1 | Turn the weather cycle on. |
| `LARECOMP_WEATHER_HARM` | 1 | Lightning shocks Link, as the randomizer option will. |
| `LARECOMP_WEATHER_PUSH` | 1 | Blizzard wind pushes Link, as the randomizer option will. |
| `LARECOMP_MENU_AT` | frame | Open the overlay menu at that frame. |
| `LARECOMP_MENU_TAB` | tab number | The tab the overlay menu opens on. |
| `LARECOMP_FONT` | font file | Use this file for the system fonts the game asks for (.ttf, .otf, .ttc, .bfttf). |

## Logging and diagnostics

All of these write to the log (`lasrecomp.log` in the data folder, or the run's redirected output).

| Variable | Values | What it does |
|---|---|---|
| `LARECOMP_FPS_INTERVAL` | seconds | How often the fps line is logged (default 2). |
| `LARECOMP_LOG_HITCH_MS` | ms | Log every gap between presents longer than this. |
| `LARECOMP_SLOW_FRAME_MS` | ms | Log a `[slow]` line for frames slower than this, with what the renderer did (default 50, 0 off). |
| `LARECOMP_HANG_DUMP` | seconds | Write a `[hang]` dump when the game or renderer is stuck this long (default 30, 0 off). |
| `LARECOMP_LOG_FILES` | 1 | Log each game file read. |
| `LARECOMP_LOG_LEVELREQ` | 1 | Log each level change request (level and spawn point). |
| `LARECOMP_LOG_LINK` | frames | Log Link's position every N frames. |
| `LARECOMP_LOG_FLAGS` | 1 | Log event flag changes in the loaded save. |
| `LARECOMP_LOG_INTERVAL` | 1 | Log each change of the game's present interval (its 60 / 30 fps mode). |
| `LARECOMP_LOG_DYNRES` | 1 | Log every dynamic resolution decision, not only the steps. |
| `LARECOMP_FNPROF` | `from:to` (frames) | Sample which game functions run between those frames, to `build/fnprof.txt`. `0:<to>` starts at process start. |
| `LARECOMP_DEBUG_FRAME` | frame | Dump that frame's render passes to `build/pass_NNN.png` and log its draws (F9 does the same for the next frame). |
| `LARECOMP_DEBUG_FRAMES` | count | With `LARECOMP_DEBUG_FRAME` (Vulkan): dump that many frames in a row. |
| `LARECOMP_LOG_FSHASH` | 1 | In the debug frame: log each fragment shader's hash (for `LARECOMP_SKIP_FSHASH`). |
| `LARECOMP_SKIP_FSHASH` | `hash,hash,...` (hex) | Drop draws that use these fragment shaders, to find which draw makes what. |
| `LARECOMP_DUMP_SHADERS` | 1 | Write each program's game shader code to `build/shaders/` (Metal). |
| `LARECOMP_SAVE_MSL` | 1 | Write translated shaders to `build/msl/` (Metal) or `build/glsl/` (Vulkan), with the source pc as comments. |
| `LARECOMP_HLE_SKIP` | text | Turn off the host replacement of game functions whose name contains this text. |

## Rendering (shared)

These work on both Metal and Vulkan unless noted.

| Variable | Values | What it does |
|---|---|---|
| `LARECOMP_RENDER_THREAD` | 0 | Translate and submit the game's command lists on the game's GPU thread instead of a separate render thread. |
| `LARECOMP_GPU_OVERLAP` | 0 / 1 | Let the CPU run a frame ahead of the GPU (on by default where supported). |
| `LARECOMP_OVERLAP_WATCH` | 0 | Turn off the page-write tracking that GPU overlap uses to catch the game rewriting memory the GPU still reads. |
| `LARECOMP_CHUNK_PASSES` | number | Submit a command buffer every N render passes (default 8; 0 = one per frame). |
| `LARECOMP_SYNC_SHADERS` | 1 | Compile shaders on first use and wait for them, instead of in the background (repeatable tests). |
| `LARECOMP_SHADER_PREP` | 0 / 1 | Run the shader preparation at boot. |
| `LARECOMP_SHADER_TOUR` | frames per spot | Warp through every warp spot so its shaders get compiled, then exit. |
| `LARECOMP_INTEGRATED` | 0 / 1 | Treat the GPU as dedicated (0) or integrated (1) when picking default video settings. |
| `LARECOMP_DYN_TEST` | number | Pin Dynamic resolution at this render height (units of 1080 lines). |
| `LARECOMP_GAME_DYNRES` | 1 | Turn the game's own dynamic resolution back on (it is off by default). |
| `LARECOMP_NO_SCALE` | 1 | Keep all render targets at the game's own size (no resolution scaling). |
| `LARECOMP_AO_SCALE` | 1 to 4 | Draw the game's ambient occlusion at this many times the Switch's size, whatever the setting and render scale. |
| `LARECOMP_AO_SCALED` | 1 | Scale the ambient occlusion with the frame, as before issue #356 (shows the old haze and dither). |
| `LARECOMP_SHADOW_SCALE` | 0.5 to 4 | Draw the game's shadow map at this many times the Switch's size, whatever the setting. |
| `LARECOMP_SHADOW_BIAS_RAW` | 1 | Keep the game's polygon offset slope in the shadow map as it is, instead of scaling it with the map's size (A/B tests). |
| `LARECOMP_AO_NO_GRID` | 1 | Leave the ambient occlusion shaders on the game's own pixel grid when the AO is larger than the Switch's. |
| `LARECOMP_NO_FRAME_CLEAR` | 1 (Vulkan: 0 / 1) | Don't clear scene render targets at their first use each frame. On Vulkan the default is no clears on integrated GPUs. |
| `LARECOMP_TEX_FULLHASH` | 1 | Hash every texture in full each time instead of the faster partial checks. |
| `LARECOMP_NO_WIDESCREEN` | 1 | Turn the widescreen view off (16:9 framing everywhere). |
| `LARECOMP_WIDE_TEST` | ratio | Pretend the window has this shape, relative to 16:9 (e.g. `1.3125` for 21:9). |

## Metal

| Variable | Values | What it does |
|---|---|---|
| `LARECOMP_METAL_ARCHIVE` | 0 / 1 | Use a Metal binary archive for pipelines (off by default; off removes the archive). |
| `LARECOMP_SHADER_SALT` | text | Tag shader sources so Metal's disk cache misses, to measure cold compiles. |
| `LARECOMP_DISPLAY_SYNC` | 0 | Turn the layer's display sync off. |
| `LARECOMP_PRESENT_DEADLINE` | 0 | Present without the one-frame deadline. |
| `LARECOMP_MTL_COUNTER` | `cpu` | Give the game's GPU timing counters the host clock only. |
| `LARECOMP_SYNC_SUBMIT` | 1 | Wait for the GPU after each command list submit (old behaviour). |
| `LARECOMP_ASYNC_SHOTS` | `from,every,count` | Save game frames without stalling, to `build/aframes/`. |

## Vulkan

The per-GPU default switches are explained, with the GPUs they were tested on, in
[vulkan-gpu-defaults.md](vulkan-gpu-defaults.md). The log names each choice near the start (`[vk] ...` lines).

| Variable | Values | What it does |
|---|---|---|
| `LARECOMP_GPU` | index | Use this GPU (the index from the `[vk] gpu N:` log lines) instead of the first discrete one. |
| `LARECOMP_VK_PIPELINE_CACHE` | 0 | Start with an empty Vulkan pipeline cache (the saved one is written over as usual). The Android emulator hangs loading a large one. |
| `LARECOMP_VK_VALIDATION` | 1 | Turn the Khronos validation layer on (it must be installed) and log its warnings and errors (first 500). |
| `LARECOMP_VK_VALIDATION_STOP` | 1 | As `LARECOMP_VK_VALIDATION`, and exit at the first validation error. |
| `LARECOMP_VK_PRESENT` | `mailbox`, `fifo`, `immediate` | Present mode (default mailbox, fifo with frame generation). |
| `LARECOMP_VK_STRUCTURE` | 0 / 1 | Structured shaders (no jump loop). Default on for Mali and Adreno. |
| `LARECOMP_VK_STATIC_STACKS` | 0 / 1 | Static branch stacks in shaders. Default follows `LARECOMP_VK_STRUCTURE`'s GPU default. |
| `LARECOMP_VK_PUSH_CB` | 0 / 1 | Hot constant buffer words as push constants. Default on for Mali and Adreno. |
| `LARECOMP_VK_INLINE_C1` | 0 / 1 | Inline constant buffer 1 into shaders. Default on for Mali and Adreno. |
| `LARECOMP_VK_SPLIT_BARRIER` | 0 / 1 | Split pass barriers. Default on for Mali. |
| `LARECOMP_VK_STACK_ARRAYS` | 0 / 1 | Shader branch stacks as arrays (1) or variables (0). Default variables on Intel, arrays elsewhere. |
| `LARECOMP_VK_PLACEHOLDER_COLOR` | 0 / 1 | Add a placeholder colour attachment to depth-only passes. Default on for Intel. |
| `LARECOMP_VK_HEAP_DMABUF` | 0 / 1 | Guest heap as driver memory (Linux). Default on for RADV and PanVK. |
| `LARECOMP_VK_NO_GMEM` | 1 | Don't bind the guest heap to shaders as storage buffers. |
| `LARECOMP_VK_GMEM_LAZY` | 0 / 1 | Import the heap to the GPU in chunks on demand. Default on where one import can't hold it. |
| `LARECOMP_VK_NO_HOST_IMPORT` | 1 | Act as if the driver can't import host memory (tests the copy path on any GPU). |
| `LARECOMP_VK_RECORDER` | 0 / 1 | Record Vulkan commands on a separate thread. Default on, except on Mali. |
| `LARECOMP_VK_PIPELINE_BINARY` | 0 / 1 | Our own pipeline binaries cache. Default on for AMD drivers only. |
| `LARECOMP_VK_DYN_VERTEX` | 0 | Keep the vertex layout in each pipeline instead of dynamic vertex input. |
| `LARECOMP_VK_OPT_FS` | 0 / 1 | Run spirv-opt on fragment and compute shaders. Default on for Mali. |
| `LARECOMP_VK_NO_SPIRV_CACHE` | 1 | Don't use the SPIR-V disk cache. |
| `LARECOMP_VK_SHADER_STATS` | 0 / 1 | Log each pipeline's driver statistics (registers, spills). |
| `LARECOMP_VK_D24` | 0 | Use D32S8 instead of D24S8 depth. |
| `LARECOMP_VK_LEAN_USAGE` | 0 / 1 | Fewer image usage flags. Default on for integrated GPUs. |
| `LARECOMP_VK_SMAA_STENCIL` | 0 / 1 | Stencil-masked SMAA. Default on for integrated GPUs. |
| `LARECOMP_VK_RING_SYSMEM` | 1 | Put the upload ring in system memory (tests a GPU without resizable BAR). |
| `LARECOMP_VK_GPU_TIMER` | 0 | No GPU timer. Dynamic resolution then goes by frame times. |
| `LARECOMP_VK_COUNTER` | `cpu`, `frozen`, `legacy` | How the game's GPU timing counters are filled. |
| `LARECOMP_VK_PROF` | 1 | Log time per present stage. |
| `LARECOMP_GPUPROF_AT` | `frame,frame,...` | Log a GPU pass profile of those frames. |
| `LARECOMP_LOG_SKIPS` | 0 / 1 | Log skipped draws and dispatches and why. |
| `LARECOMP_VK_DEVLOST_TRACE` | 0 / 1 | Log the steps before a device lost. |
| `LARECOMP_WRITE_TRACKING` | 0 | Linux: turn off write tracking of guest memory. |

## Frame pacing and performance

| Variable | Values | What it does |
|---|---|---|
| `LARECOMP_PACE` | 0 | Turn the frame pacer off (it holds the game's 30 fps mode when frames don't fit 60). |
| `LARECOMP_SIM_SLOW_FRAME_MS` | ms | Sleep this long every frame after frame 2600, to simulate a slow machine. |
| `LARECOMP_SLOW_FRAME_UNTIL` | frame | With `LARECOMP_SIM_SLOW_FRAME_MS`: stop at that frame (recovery tests). |
| `LARECOMP_SPIKE_MS` | ms | Stall the presenting thread this long every N frames. |
| `LARECOMP_SPIKE_EVERY` | frames | How often `LARECOMP_SPIKE_MS` stalls (default 60). |
| `LARECOMP_SPIKE_FROM` | frame | First frame for spikes (default 2700). |
| `LARECOMP_TICK_SCALE` | number | Scale the game's clock (default 1). |
| `LARECOMP_INTERP` | `on`, `off` (or `0`), `measure` | Override the frame generation setting. `measure` logs and matches without extra images. |
| `LARECOMP_INTERP_HZ` | Hz | Use this display refresh rate for frame generation instead of the one reported. |
| `LARECOMP_PCORE` | 0 | Linux / Windows: no pinning to performance cores. |
| `LARECOMP_PCORE_GUEST` | 0 | Linux: let game threads run on all cores. |
| `LARECOMP_THREAD_QOS` | 0 | macOS: don't raise game threads' QoS class. |

## Game-code build

The game code library is built from the player's own extracted game files (`make gen` and `make` in a source checkout,
the start screen's setup otherwise).

| Variable | Values | What it does |
|---|---|---|
| `LARECOMP_GAME_LIB` | file path | Load the game code library from this path first. |
| `LARECOMP_GEN` | file path | Code generator to use instead of the bundled `lasgen`. |
| `LARECOMP_CC` | file path | Compiler to use instead of the bundled zig. |
| `LARECOMP_GUEST_BASE` | hex address | Builds with `DYN_BASE=1` only: start the guest window here (4 GB aligned, ending below 2^39). Without it a desktop `DYN_BASE` build uses `0x7100000000`, so the game runs away from its own addresses. |

## Tool scripts

Read by scripts in `tools/`, not by the app.

| Variable | Values | What it does |
|---|---|---|
| `LARECOMP_ROMFS` | folder | The extracted game's romfs folder, for the Python tests that read game files (`test_field_regions.py`, `test_room_crossing.py`, `test_room_ledges.py`). |
| `LARECOMP_MIN_FREE_GB` | GB | `tools/game_slot.sh`: free memory to wait for before a test copy starts (default 10). |

Narrow one-off probes (memory dumps and watches at fixed addresses, single-shader skips, per-feature tuning, menu
screenshot helpers and extra trace switches) are left out here. They are documented in the comment where each one is
read.
