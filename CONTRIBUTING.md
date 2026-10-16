# Contributing to LAS Recomp

Thanks for wanting to help! Bug reports, randomizer logic fixes, testing on different hardware and code
contributions are all welcome.

> [!NOTE]
> This project is developed mostly with AI (see the README). Contributions written by hand, with AI assistance or
> anything in between are equally welcome - what matters is that a change works, is tested, and follows the rules
> below.

## The one hard rule: no Nintendo files

Nothing owned by Nintendo may ever be committed or attached to an issue or pull request:

- no copies of the game in any form
- no extracted game files (`extracted/`), no generated code (`generated/`), no fonts (`.bfttf` / Switch `.ttf`)
- no game assets of any kind: textures, models, music, text, level files - also not "just one file for testing"
- no randomizer seed folders (`rando/`) - they contain modified game files; share the **seed ID** instead

Pull requests that include Nintendo files can't be merged. Screenshots and short clips of the game in bug reports are fine.

## Reporting bugs

Report bugs either on the [Discord](https://discord.gg/DhrgdYqdcY) or as a
[GitHub issue](https://github.com/peiikai/LASRecomp/issues) - whichever you prefer. Please include:

- your platform (macOS / Windows), hardware (CPU, GPU) and window size / aspect ratio
- what you did, what happened, and what you expected
- for randomizer problems: the **seed ID** (Randomizer tab, Copy button), the settings, and where you got stuck
  (area / room, which items you had). A seed ID reproduces the exact seed on the same LAS Recomp version.
- a screenshot or short clip if it's visual
- for crashes: the log the app prints to the terminal (run it from a terminal to capture it)

## Building

See the README's "Building from source" for macOS and [docs/BUILD-windows.md](docs/BUILD-windows.md) for Windows.
In short: set up your own v1.0.0 game files on the start screen, `make gen` once to recompile the game code from
them, then `make -j10`. The randomizer needs its Python environment in
`third_party/LAS-Randomizer/venv`.

## Project layout

- `tools/gen.py` - the recompiler: translates the game's executables into C (`generated/`, never committed).
  `tools/patches.py` holds code patches applied during generation; every patch site checks the original instruction
  word, so a wrong address fails loudly instead of silently.
- `runtime/` - the host side:
  - `kernel.c`, `loader.c`, `services/` - the Switch OS replacement
  - `hle/` - replacements and features (door jumps, keysanity, pots / bushes, souls, warps, input, ...)
  - `nvn/` - the graphics layer: command recording (`cmdbuf.c`), Metal (`renderer.m`, `metal.m`) and Vulkan
    (`vulkan/`) backends, the shader translator (`maxwell/translate.c`)
  - `ui/` - the ImGui overlay menu and the start screen
- `tools/rando/` - randomizer tooling around LAS-Randomizer: the headless driver (`las_headless.py`), dungeon room
  shuffle (`dungeon_gen.py`, `room_logic.py`), entrances, pots / bushes / digs, shop, souls, pets.
- `third_party/LAS-Randomizer` - the vendored randomizer (MIT, with local changes marked "LAS Recomp" in comments).

## Testing your change

- Build cleanly on your platform (`make -j10`). If you touch shared rendering code, say which backend you tested -
  a Metal change usually needs a Vulkan counterpart and the other way round.
- Test in game, and look at the result: a fix isn't verified until you've seen it work.
- Scripted test runs: `tools/shot.sh <frame> [seconds]` runs the game with `LARECOMP_*` environment hooks (scripted
  input, warps, seeds, scratch saves via `tools/fresh_save.sh`) and saves a screenshot. Many debug hooks are
  documented where they're defined - grep for `getenv("LARECOMP_`. Test runs use a scratch copy of the settings
  file; give them a scratch save folder too (`LARECOMP_SAVE_DIR`) so your own saves stay untouched.
- Randomizer changes: generate several seeds with different settings and check they complete (every location
  reachable). For dungeon room shuffle, `tools/doorjump_test.sh <seed>` walks every mismatched doorway.
- State that belongs to a seed must be reset when the player switches seeds in place: use an `RT_ON_RESET` hook
  (see `runtime/hle.h`).

## Code style

Build-cache regression tests use synthetic inputs and the production scheduler:
`python tools/test_build_cache.py` and `python tools/test_gamebuild_resume.py`.
On Windows, also run `python tools/test_windows_processes.py` to check independent
helper process trees, descendant cleanup and crash exit codes. `CC` / `CXX` select
the native compiler (defaults: `clang` / `clang++`). For a release smoke test,
cancel a real game-code build after some objects finish, resume it, inspect the
reused/compiled counts in the log, and launch the resulting game library.

- Match the surrounding code: its naming, structure and comment density. Comments explain *why* (what the game does,
  what an address is, which seed or bug showed the problem), not what the next line does.
- Keep randomizer and game-facing text in the game's own style: pickup messages go through the game's message box,
  not custom pop-ups; settings get short labels, not paragraphs.
- New randomizer options must keep older seed IDs loading (see the seed ID notes in `runtime/ui/rando_tab.mm`).

## Pull requests

- Keep each pull request focused on one change; small is easier to review.
- Describe what changed, why, and how you tested it (platform, seeds, screenshots).
- Don't include personal paths or information in code, comments or commit messages.

## License

By contributing, you agree that your contributions are licensed under the project's [MIT License](LICENSE).
