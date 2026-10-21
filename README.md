# Multi

**Windows multi branch:** [Download prereleases](https://github.com/hjqcan/LASRecomp/releases) · [Features and setup / 功能与使用说明](docs/multi.md)

Multi is hjqcan's independently maintained multi-game branch of the [LAS Recomp fork](https://github.com/hjqcan/LASRecomp/tree/multi), based on [peiikai/LASRecomp](https://github.com/peiikai/LASRecomp). It is not an official LAS Recomp release. Development happens on `multi`; `main` and `beta_5` mirror upstream. Report branch-specific issues to [this fork](https://github.com/hjqcan/LASRecomp/issues).

Multi Beta 5.4 adds ACNH 3.0.1 and an optional experimental 60 FPS + SpeedFix
mode (Windows beta). Standard 30 FPS remains the default. The correction needs
sustained 60 FPS; lower performance slows the game. See the support limits below.
Multi Beta 5.4 新增动森 3.0.1 及可选的实验性 60 FPS＋速度修正（Windows 测试版），
默认仍为标准 30 FPS。修正模式低于 60 FPS 时会变慢。

New releases use `multi-beta*` tags. Older clients need one manual download to
move to this update channel; [upgrade instructions](docs/multi.md#names-and-compatibility--名称与兼容性).
新版本使用 `multi-beta*` 标签，旧客户端首次需要手动下载升级。

The multi branch supports separate installations and desktop shortcuts
for Link's Awakening 1.0.0/1.1.0 and Animal Crossing: New Horizons 1.0.0/3.0.1
(Windows beta). The launcher identifies supported releases from already-extracted
folders containing `exefs` and `romfs`, copies them and builds their native game libraries.
See [multi-game setup](docs/multi-game.md) and the
[ACNH support and limits](docs/ACNH-port.md).

Pokémon Legends: Z-A development work is maintained directly on `multi`.
It remains development-only and is not playable; see the
[current probe results and blockers](docs/pokemon-za-port.md).

Multi extends the upstream static recompilation runtime with game adapters, separate installations and local development tools. The published fork builds are Windows betas. macOS/Metal and Linux build paths remain available, but their native gameplay is not certified by these Windows releases. See [support and limits](docs/multi.md).

Development on this branch is AI-assisted. Feature support and experimental limits are documented in the linked game guides.

![Mabe Village in a 21:9 window: the unlocked aspect ratio shows more of the island](docs/ultrawide.jpg)

> [!NOTE]
> **No game files are included.** Multi needs your own legally obtained copy of *Link's Awakening*:
> already-extracted **v1.0.0 / v1.1.0** game files. Game archives, decryption keys and update packages are not accepted.
> For extracted files, select the game folder containing `exefs` and `romfs`.
> Both versions support Vanilla mode. v1.1.0 Randomizer support is experimental and requires newly generated, version-specific seeds.
> See [game-file setup, saves and v1.1.0 support](docs/game-files-and-versions.md) for accepted builds and input limitations.
> Instructions on how to obtain game files will **NOT** be provided.
> On first launch the app sets up what it needs, including the actual recompilation locally. No game files, assets or fonts are included.
> The screenshot shows the start screen after setup; no Nintendo artwork is included until provided by the user.

![The start screen: choose vanilla or a randomizer seed, set up your game files, settings](docs/start_screen.jpg)

## Status

This is a beta release. That means bugs are likely, crashes could occur, randomizer seeds could possibly be unsolvable (dungeon room shuffle modes are especially experimental at this point) and some graphics issues remain (especially in wide and ultrawide aspect ratios). This is a work in progress, hence the beta designation. To report a bug or a possible randomizer logic issue, open a [GitHub issue](https://github.com/hjqcan/LASRecomp/issues) - see [CONTRIBUTING.md](CONTRIBUTING.md#reporting-bugs) for what to include.

### Known issues

Known issues and their status are on the [GitHub issues page](https://github.com/hjqcan/LASRecomp/issues).

## Features

The gameplay and randomizer options below apply to Link's Awakening. Other games have their own adapter support and limits in [multi-game setup](docs/multi-game.md).

**Runs natively**
- **This is not emulation.** Runs natively on Windows, macOS and Linux using static recompilation of the Switch code.
- Unlocked aspect ratio: wider (or taller) windows show more of the island, up to ultrawide; houses and dungeons
  keep their framing. The original 16:9 framing is one option away.
- Start screen: play vanilla or any randomizer seed, set up your own extracted game files, import fonts.
  - Switch games use fonts built into the Switch operating system for some text. Sensible replacements are used by default, but you may optionally set up the Switch fonts for an authentic look. Switch fonts are Nintendo IP; you must provide them from your own hardware.
- In-game overlay menu (F1, or Minus + Plus on a controller).
- [Photo mode](docs/photo-mode.md) (F2): freeze the scene, adjust framing/color and save a PNG (F12).
- [Resource mods](docs/resource-mods.md): import a local replacement folder, inspect metadata and enable it with conflict checks.
- Switch between vanilla and any randomizer seed without restarting the app.
- Keyboard and controller support, with every button and key remappable (Controls tab / start screen Settings >
  Controls; menus keep a fixed layout, so a remap can never lock you out).
- Button prompts for your controller: Xbox or PlayStation icons in the game's own UI and text when SDL reports
  that controller (or pick Nintendo / Xbox / PlayStation yourself); the icons follow your controller remapping.
- A controller SDL doesn't recognise can be added with your own SDL mapping line (as in
  [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)) in `gamecontrollerdb.txt` in the data
  folder (see "Where files live" below); the log names it once it works: `[input] gamepad: ...`.
- Optional 360-degree movement, blur removal and a hidden Hero Mode icon.
- Optional run timer in the corner: real time from the start of gameplay to the ending (set it before starting a
  save), and LiveSplit support through its Server component: auto-start, a split for each instrument and the ending.
- Apple MetalFX upscaler on macOS (the default there).
- Internal resolution adjustments: raise or lower the resolution in steps, match the display, or use dynamic resolution. Combined with the MetalFX upscaler, even a MacBook Air should handle this easily with the right settings. On Windows and Linux the AMD FSR 1 upscaler is available.

**Randomizer**
- In-game seed generator.
- Seed IDs for sharing or reproducing exact seeds.
- Per-seed save games.
- Full item shuffle.
- Trade Quest shuffle.
- Disable Acorn / Piece of Power drops.
- Bombs / Magic Powder shuffle.
- Hookshot anything - the hookshot latches onto any wall or object, not just hookshot targets (in logic where enabled).
- Environment Randomizer - randomizes in-game environmental effects.
- Enemy Randomizer - shuffles overworld enemies, optionally including mini-bosses. Not all mini-bosses are included
  yet; more will be added as they're made to work outside their dungeons.
- Music Randomizer.
- Escape Bottle - every seed gives you this gold bottle from the start (it replaces one bottle in the pool). Use it to
  warp to the dungeon entrance when in a dungeon, or to Marin and Tarin's house anywhere else. Never required by the logic.
- Pickup text in the game's own message box: dungeon items name the dungeon they belong to ("You got a small key for
  Tail Cave!").
- Potsanity - every pot is a check. Pots are golden until checked, so you can see at a glance which pots you've checked.
- Bushsanity - every cuttable bush (not the grass) is a check. Like pots, bushes are golden until checked for the first
  time. Bushes over a hole or staircase stay normal, so items can't fall in.
- Digsanity - a truly insane setting that makes every diggable tile on the overworld a check (about 5,000). Holes stay
  dug permanently so you know where you've dug. Made for shovel enthusiasts.
- Keysanity - keys can be found anywhere for any dungeon.
- Shopsanity - the shop's 6 items are checks, at vanilla or random prices, or just steal them. A bought or stolen slot
  shows a sold-out sign.
- Trendsanity - full Trendy Game randomization: every prize, including the figures, is a check.
- Pets - companions as items in the pool:
  - Rooster - in logic for Feather checks (including in dungeons if companions are allowed there).
  - Marin - you need to find Marin to get into the desert; choosing open desert removes Marin from the item pool.
  - Ghost - find the Ghost to unlock his grave check.
  - Switch companions from the pause menu, or send yours away.
- Boss souls - a boss doesn't appear, can't be beaten and its reward stays locked until you find its soul.
- Mini-boss souls - the same for dungeon mini-bosses (a separate option).
- Triforce hunt - collect Triforce pieces (3, 12, 24 or any number up to 50, optionally with extra pieces hidden) instead
  of waking the Wind Fish; the last one needed rolls the credits.
- Entrance Randomizer - shuffles dungeon entrances, houses and caves, and connector caves. Includes a decoupled option.
- Dungeon Room Shuffler - shuffles all dungeon rooms, creating unique custom dungeons every seed. Three modes:
  - Coherent: dungeon rooms are shuffled but connecting rooms are lined up. To keep this workable in logic, dungeons
    sometimes use duplicate rooms (as few as possible).
  - Chaos: dungeon rooms are shuffled but don't need to make sense. Walking north may lead you to a door from another
    direction.
  - Madness: dungeon rooms are shuffled in a way that doesn't need to make sense AND the doors are decoupled, creating
    nightmares to navigate.
  - Labyrinth (planned): an option for all dungeon room shuffles where doors in dungeons can lead into other dungeons,
    essentially making one giant dungeon with numerous wings.
  - Optionally the map shows what's connected to what as you explore, or in full once you find the dungeon's map.
- The inventory screen tracks keys, dungeon items, trade items and souls for each dungeon, so you can see at a glance
  what you have keys for and which trade items you still hold.
- Built-in check tracker on the overworld map (optional).
- Race mode - the spoiler log stays locked until the credits, and the title / file select screen shows a seed hash of
  five item icons so racers can check they have the same seed.

## Building from source

Every platform needs your own supported game files: build first, then use the start screen's game-file setup.
Choose **Game files folder** and select a complete, already-extracted folder containing `exefs` and `romfs`,
then **Set up game files**. The executable modules identify the supported game and version.
Provide a complete extracted folder for the desired version; Multi does not decrypt, unpack or apply game update packages.
Choose the desired **Game version** slot before setup. The existing installation and its saves stay available.
**Settings → Saves → Backups** lists automatic snapshots and lets you restore them before starting a game.
`make gen` then recompiles the game code from your own game files into `generated/`, and `make` builds it into its own
library next to the app (`build/lasgame.*`); without it the start screen builds that library itself, as it does for
players. Release builds hold none of the game's code: the start screen builds it on each player's machine from their
own game files, with the bundled generator (`make gen-helper`) and compiler (`tools/fetch_zig.sh`).

### macOS

Requirements: Apple Silicon, Xcode command line tools (clang), [Homebrew](https://brew.sh) `sdl3`, `ffmpeg` and `opus`,
Python 3.12.

1. Build: `make -j10` -> `build/lasrecomp`. Set up your game files (above), then `make gen` and `make -j10` again.
2. Randomizer: create the Python environment for LAS-Randomizer, from `third_party/LAS-Randomizer`:
   `python3.12 -m venv venv && venv/bin/pip install -r requirements.txt`.
3. Run: `./build/lasrecomp extracted` (start screen) or `./build/lasrecomp extracted --play` (straight into the game).
4. Package: `make deps` (once: SDL3, FFmpeg, libopus, keystone and Python from pinned sources, built for macOS 15),
   `make rando-helper` (the seed generator as a standalone helper, so players need no Python), `make gen-helper` and
   `tools/fetch_zig.sh` (what the start screen builds the game code with), then `make app` ->
   `build/Multi.app` (macOS 15+, Apple Silicon) and `make dmg` -> `build/dist/Multi-<version>-macOS.dmg`.

### Windows

Requirements: Windows 10+ x86-64, a CPU with AVX2 and a GPU with Vulkan 1.3. More detail in
[docs/BUILD-windows.md](docs/BUILD-windows.md).

1. One-time setup, in PowerShell from the repo root: `powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1`.
   It installs MSYS2 (UCRT64: clang, make, SDL3, FFmpeg, libopus, Vulkan, shaderc), Python 3.12 for the tools and the
   randomizer (`third_party\LAS-Randomizer\venv`) and switches the clone to LF line endings.
2. Build, from an MSYS2 UCRT64 shell (or Git Bash with `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"`):
   `make -j` -> `build/lasrecomp.exe`. Set up your game files (above), then `make gen` and `make -j` again.
3. Run: `./build/lasrecomp.exe extracted`, from a shell with `C:\msys64\ucrt64\bin` on PATH (for the DLLs).
4. Package: `make deps-windows` (once: LGPL FFmpeg), `make rando-helper RANDO_PY="<path to python.org Python 3.12>"`
   (MSYS2's Python has no oead wheel), then `make dist` -> `build/dist/Multi-<version>-windows.zip`.

### Linux

Requirements: x86-64 with AVX2 (or aarch64, experimental), a GPU with Vulkan 1.3, clang, SDL 3, FFmpeg, libopus, shaderc and
Vulkan Memory Allocator from your distribution, Python 3.12. Packages, cross builds and details in
[docs/linux.md](docs/linux.md).

1. Build: `make -j` -> `build/lasrecomp`. Set up your game files (above), then `make gen` and `make -j` again.
2. Run: `./build/lasrecomp extracted`.
3. Package: `make appimage` (this machine's arch; `ARCH=aarch64` cross-compiles) or `make appimages` (both) ->
   `build/dist/Multi-<version>-linux-<arch>.AppImage`, built in Ubuntu 22.04 containers (podman). `make smoketest`
   checks the x86-64 AppImage before a release.

Where files live (game files, saves, seeds, settings, caches, imported fonts):
- macOS app: `~/Library/Application Support/com.peiikai.lasrecomp/` (the app itself is never written to).
- Windows: next to `lasrecomp.exe`, so put the `Multi` folder somewhere writable (not Program Files).
- Linux: `~/.local/share/lasrecomp/` (`$XDG_DATA_HOME/lasrecomp`).
- Dev / portable mode: if the folder you start from has `extracted/` (a source checkout), that folder is used, as above.
- `LARECOMP_DATA_ROOT=<dir>` overrides all of these.
- The log (started without a terminal) is `lasrecomp.log` in that folder, always the latest run; up to five recent
  runs are kept as `lasrecomp-backup-*.log`. For a bug report attach `lasrecomp.log`, or if the problem was in an
  earlier run, that run's backup (by date modified).

### Save backups

The **Saves → Backups** tab on the start screen lists snapshots of each save area
(all slots together). Existing saves are backed up before play and after completed
game saves. Unchanged saves are skipped; the latest 20 snapshots are retained.
Vanilla and each randomizer seed keep separate histories beside their save folders,
for example `save/save_0.backups` and `rando/<seed>/save/save_0.backups`.

To restore, restart Multi, select the profile and snapshot, then confirm
**Restore selected backup**. The current files are backed up before replacement.
Manual backups and restores are available before the game library is loaded.
These local snapshots protect against accidental save changes; copy the save and
backup folders elsewhere if you also need protection against disk failure.

### Resuming game-code builds

The game-code build keeps completed pieces in the game folder's `.gamebuild/`
directory. **Cancel (B / Esc)** stops the compiler helpers; **Resume game code**
appears after cancellation or interruption and continues from verified completed
pieces. An interrupted build also resumes when Multi starts again. Generated
source and object contents are checked before reuse. After success, only objects
and their cache records are kept; generated C, logs, link files and compiler caches
are removed. A generator update recompiles only changed pieces. Moving the app
keeps reusable objects; changed compiler contents or flags require a full rebuild.

**Game Files → Clear build cache** removes cached objects and temporary files,
including the older `.zigcache` folder, while keeping the installed game library,
game files and saves. Legacy build caches are upgraded automatically. Failed or
cancelled builds keep the previous installed library until a replacement is ready.

## Credits

**[peiikai/LASRecomp](https://github.com/peiikai/LASRecomp)** supplies the original recompiler, runtime and Link's Awakening implementation on which this branch is based. The upstream authors and contributors retain their original credit; Multi is maintained separately by hjqcan.

**Contributors** - thank you to everyone who has sent code (GitHub handles, in order of first contribution):
- **[@dvilelaf](https://github.com/dvilelaf)** - the System language setting (#15), packaging fix for the game code
  generator helper (#10), and the task forwarder fix for the level-transition crash (#34, issue #19)
- **[@Tensorlong](https://github.com/Tensorlong)** - Metal half-precision fix for the white Link on M1 Macs (#18)
- **[@mansarip](https://github.com/mansarip)** - support for unrecognised controllers through a simple mapping file (#26)
- **[@we11adam](https://github.com/we11adam)** - shared fonts served per language instead of one Japanese font (#27),
  and the zh-Hans / zh-Hant codes for Chinese (#25)
- **[@Maganra](https://github.com/Maganra)** - Linux page write tracking (userfaultfd + PAGEMAP_SCAN) for the Vulkan
  vertex cache and texture checks (#42)
- **[@hjqcan](https://github.com/hjqcan)** - centralized game patch and runtime addresses (#57); CRLF settings
  parsing and Traditional Chinese locale detection (#53); Windows game thread stacks reserved instead of committed,
  about 9 GB less memory committed (#107); the Vulkan fence fix for reused submission slots (#108); x86 rounding
  without x87 state changes (#115); movie memory freed on close, source change and reboot (#112); Vulkan stopping
  cleanly on GPU timeouts (#127); a strict Vulkan validation mode for automated tests (#128); Windows session logs:
  `lasrecomp.log` always the latest run, with five backups (#114); resumable game-code builds: a cancelled or interrupted setup picks up where it stopped (#56); randomizer check flags kept with committed saves (#170); save write fixes that keep a failed save from losing data (#169, merged in #182)
- **[@gitarre1234rr5](https://github.com/gitarre1234rr5)** - controller rumble: the game's vibration on your controller
  (#104), and a regression test for the kernel's timed condition-variable wait (#88)
- **[@rkammoun](https://github.com/rkammoun)** - diagnosed and patched the Metal 16-sampler limit behind the black walls in the Ukuku Prairie cave (#215, fixed in #216)

Thanks also to everyone who reports issues and tests builds; your logs and screenshots find most of the bugs.

**LAS-Randomizer** - the randomizer at the heart of this project's randomizer features, vendored (with local
changes) in `third_party/LAS-Randomizer` under the MIT license. Huge thanks to its authors and community:
- **Owen_Splat** - LAS-Randomizer (https://github.com/Owen-Splat/LAS-Randomizer)
- **Glan** - created the original early builds of the randomizer (https://github.com/la-switch/LAS-Randomizer)
- **ProfessorLaw** - additional programming
- and everyone who has reported bugs, given feedback or contributed to LAS-Randomizer

**Community mods** - some options re-implement ideas from these GameBanana mods (re-implemented in the recomp;
none of their files are used):
- **360 Controls (And More!)** by Owen_Splat - 360-degree movement, blur removal (blur removal patch by theboy181)
  (https://gamebanana.com/mods/659802)
- **XBOX UI** by fellow_demo - Xbox button prompts (https://gamebanana.com/mods/540069)
- **PS4 UI Buttons** by messiahgov - PlayStation button prompts (https://gamebanana.com/mods/324458)
- **No Hero Mode Indicator** by Boo/Calla - hide the Hero Mode icon (https://gamebanana.com/mods/441172)

**Libraries and tools**
- [SDL 3](https://libsdl.org) - windowing, input, audio (zlib license)
- [FFmpeg](https://ffmpeg.org) - movie playback (LGPL)
- [Dear ImGui](https://github.com/ocornut/imgui) - overlay menu and start screen (MIT)
- [Baloo 2](https://github.com/EkType/Baloo2) by Ek Type - the start screen's font (SIL Open Font License 1.1, `third_party/fonts/Baloo2/OFL.txt`)
- [DejaVu Sans](https://dejavu-fonts.github.io) - the game text's fallback font when no other is available (Bitstream Vera / DejaVu license, `third_party/fonts/DejaVu/LICENSE`)
- [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) - Windows and Linux renderer (MIT)
- [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego
  Gutierrez - the anti-aliasing option (MIT, `third_party/smaa/LICENSE.txt`)
- [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) - the Windows and Linux upscaler
  option, ported to GLSL (MIT)
- [oead](https://github.com/zeldamods/oead) and [evfl](https://github.com/zeldamods/evfl) - Nintendo file formats, used by LAS-Randomizer
- [PyInstaller](https://pyinstaller.org) - packs the seed generator into a standalone helper (GPL 2.0+ with the
  bootloader exception, which allows distributing the packed program). The helper bundles CPython (PSF License),
  oead / evfl (GPL 2.0+), [Keystone](https://www.keystone-engine.org) (GPL 2.0; Python bindings BSD), quicktex
  (LGPL 3), Pillow (MIT-CMU), PyYAML (MIT) and appdirs (MIT)
- [Zig](https://ziglang.org) - its C compiler (`zig cc`: Clang and LLD) builds the game code on the player's
  machine during setup (MIT; LLVM, Clang, LLD: Apache 2.0 with LLVM exceptions)
- The game code generator (`tools/gen.py`) is packed by PyInstaller into a standalone helper too. It bundles CPython,
  [Capstone](https://www.capstone-engine.org) (BSD 3-clause; disassembly), [Unicorn](https://www.unicorn-engine.org)
  (GPL 2.0; Python bindings BSD; runs small pieces of code to resolve jump tables) and
  [python-lz4](https://github.com/python-lz4/python-lz4) (BSD 3-clause, with the LZ4 library: BSD 2-clause)
- [envytools](https://github.com/envytools/envytools) - NVIDIA GPU documentation used for the shader translator
- **Ryujinx** by the Ryujinx Team and Contributors (MIT, `third_party/ryujinx/LICENSE.txt`) - the audio renderer's
  reverb is a port of its ReverbState / reverb command and the mixer is modelled on its AudioRenderer; its shader
  instruction decoders were the bit-layout reference for parts of the shader translator
- **Dusklight** by the TwilitRealm team (https://github.com/TwilitRealm/dusklight) - design inspiration for the start
  screen (Play / Settings / Game Files layout)
- **SuperMarioWonderRecomp** by BelmanteGu (https://github.com/BelmanteGu/SuperMarioWonderRecomp) - its
  ahead-of-time shader approach is what the first-launch shader preparation follows (a shipped list of shader variants,
  built on the player's machine from their own game files)
- The Switch homebrew community's documentation ([Switchbrew](https://switchbrew.org))

## Legal

Multi is a fan project. It is not affiliated with, endorsed by or sponsored by Nintendo. *The Legend of Zelda*
and *Link's Awakening* are trademarks of Nintendo. No game files, game code, assets or fonts are included - not in
this repository and not in release builds. You must provide your own legally obtained copy of the game files to play.

## License

Multi's own code (the recompiler tools, the runtime, the randomizer additions) is open source under the
[MIT License](LICENSE). Third-party components keep their own licenses: LAS-Randomizer (MIT), code ported from Ryujinx (MIT), Dear ImGui (MIT),
Vulkan Memory Allocator (MIT), SMAA (MIT), AMD FSR 1 (MIT), the Baloo 2 font (SIL OFL 1.1), the DejaVu Sans font (Bitstream Vera / DejaVu license), SDL 3 (zlib), libopus (BSD 3-clause), FFmpeg (LGPL 2.1+, linked dynamically), the
Zig compiler (MIT; its LLVM / Clang / LLD: Apache 2.0 with LLVM exceptions; its C library headers keep their own
licenses), and the generator helpers' bundled libraries listed under Credits (oead / evfl / Keystone in the seed
generator and Unicorn in the game code generator are under the GPL, versions in the table below: each helper is a separate program the game runs, and its
sources are this repository plus those projects' published sources). The license covers Multi's code only - it grants
nothing for Nintendo's game, which you must supply yourself.

### Source code of bundled third-party components

Release builds include these components; their source code for the exact versions shipped is available here:

| Component | Version | License | Source |
|---|---|---|---|
| Keystone (seed generator) | 0.9.2 | GPL 2.0 | https://github.com/keystone-engine/keystone/releases/tag/0.9.2 |
| oead (seed generator) | 1.3.0 | GPL 2.0+ | https://github.com/zeldamods/oead/releases/tag/v1.3.0 |
| evfl (seed generator) | 1.2.0 | GPL 2.0+ | https://github.com/zeldamods/evfl/releases/tag/v1.2.0 |
| quicktex (seed generator) | 0.2.1 | LGPL 3.0 | https://github.com/drewcassidy/quicktex/releases/tag/v0.2.1 |
| PyInstaller (packs the seed and game code generators) | 6.22.3 | GPL 2.0+ with bootloader exception | https://github.com/pyinstaller/pyinstaller/releases/tag/v6.22.3 |
| CPython (seed and game code generators) | 3.12.14 (macOS), 3.13.7 (Multi Windows), 3.12.15 (Linux) | PSF | https://github.com/python/cpython/releases/tag/v3.12.14, https://github.com/python/cpython/releases/tag/v3.13.7, https://github.com/python/cpython/releases/tag/v3.12.15 |
| Zig (builds the game code during setup; includes LLVM, Clang, LLD) | 0.17.0 | MIT; LLVM: Apache 2.0 with LLVM exceptions | https://ziglang.org/download/0.17.0/zig-0.17.0.tar.xz |
| Capstone (game code generator) | 5.0.9 | BSD 3-clause | https://pypi.org/project/capstone/5.0.9/ |
| Unicorn (game code generator) | 2.1.4 | GPL 2.0 (Python bindings: BSD) | https://pypi.org/project/unicorn/2.1.4/, https://github.com/unicorn-engine/unicorn |
| python-lz4 (game code generator; includes LZ4) | 4.4.5 | BSD 3-clause; LZ4: BSD 2-clause | https://pypi.org/project/lz4/4.4.5/ |
| FFmpeg (LGPL configuration, macOS, Windows and Linux builds) | 9.0.2 | LGPL 2.1+ | https://ffmpeg.org/releases/ |
| libopus (fork hwopus service) | 1.6.1 | BSD 3-clause | https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz |
| SDL 3 | 3.4.18 (macOS, Windows), 3.4.16 (Linux) | zlib | https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18, https://github.com/libsdl-org/SDL/releases/tag/release-3.4.16 |
| shaderc (Windows, Linux: GLSL to SPIR-V; Linux: with glslang, SPIRV-Tools inside) | 2026.4 (Windows), 2026.3 (Linux) | Apache 2.0 | https://github.com/google/shaderc |
| libiconv (Windows: used by SDL 3) | 1.19 | LGPL 2.1+ | https://ftp.gnu.org/pub/gnu/libiconv/libiconv-1.19.tar.gz |
| GCC runtime: libgcc, libstdc++ (Windows) | 16.2.0 | GPL 3.0 with the GCC Runtime Library Exception | https://gcc.gnu.org/ |
| winpthreads (Windows, mingw-w64) | 14.0.0 | MIT / BSD | https://www.mingw-w64.org/ |
| Baloo 2 font (SemiBold) | 1.640 | SIL OFL 1.1 | https://github.com/EkType/Baloo2 |
| SMAA (shader and lookup textures, built in) | 2013 (repository master) | MIT | https://github.com/iryoku/smaa |

The exact build scripts that fetch and build these (with checksums) are in this repository: `tools/build_deps_macos.sh`,
`tools/build_deps_windows.sh`, `tools/linux/build_deps.sh` and `tools/rando/rando_gen.spec`. The Windows build takes SDL 3, shaderc, libiconv and the
compiler runtime from MSYS2's UCRT64 packages (https://packages.msys2.org, which links each package's source).
