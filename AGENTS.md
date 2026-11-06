# Notes for agents

- **What this is.** LAS Recomp statically recompiles the Switch release of *Link's Awakening* into a native app.
  - `tools/gen.py` turns the game's executables into C, which is built on the player's machine from their own
    extracted game files.
  - A host stands in for the Switch OS and services (`runtime/kernel.c`, `loader.c`, `services/`).
  - The game's NVN graphics are drawn with Metal on macOS and Vulkan on Windows and Linux (`runtime/nvn/`).
  - The randomizer and most features live in `runtime/hle/`, `runtime/ui/` and `tools/rando/`.
  - [README.md](README.md) is the player-facing page. [CONTRIBUTING.md](CONTRIBUTING.md) has the layout, building,
    tests and pull request rules. Read it before your first change.

- **Where to start:**

  | Task | Read first |
  |---|---|
  | Building | README "Building from source", [docs/BUILD-windows.md](docs/BUILD-windows.md), [docs/linux.md](docs/linux.md) |
  | A crash or hang | the log (`lasrecomp.log` in the data folder): the crash line names the guest address (`main+0x...`). Map it with `tools/sym.py` and read the code there before changing anything |
  | Rendering | `runtime/nvn/cmdbuf.c` (recording), `renderer.m` / `metal.m` (Metal), `vulkan/exec.c` (Vulkan), `maxwell/translate.c` (shaders) |
  | Game behaviour or a new feature | `runtime/hle/` and the hooks in `tools/patches.py` and `runtime/hle/hooks.c` |
  | Game addresses | CONTRIBUTING "Project layout" (`runtime/game_addresses/`, `GA(NAME)`, `tools/version_map.py`) |
  | Randomizer logic | `tools/rando/` and `third_party/LAS-Randomizer` (local changes marked "LAS Recomp") |
  | Menu text | `python3 tools/menu_strings.py check` |

- **No Nintendo files, ever.**
  - Never commit, attach or paste game files, extracted data, generated code (`generated/`), fonts, assets or seed
    folders (`rando/`).
  - Game files stay outside the repository.
  - Refer to them only as the player's own extracted game files. Don't write about how they are obtained, in code,
    comments, docs, commits or pull requests.

- **Branch.** `main` holds the last release and is not where work happens. Base your work on the highest-numbered
  `beta_<n>` branch and open the pull request against it. Check that branch first: the change you have in mind may
  already be there.

- **Fix the cause, not the symptom.**
  - Before changing code, find why it happens: the faulting instruction, the value that went wrong and who wrote
    it, and the NVN / Metal / Vulkan rule the host gets wrong. Fix that place.
  - A stub, a skipped call, a clamp, a retry or a longer timeout is a workaround. Use one only when the cause is
    known and can't be fixed where it lies (usually the game's own code). Say so in a comment.
  - Patch the game's own code minimally rather than replacing whole systems.

- **Addresses and patches.**
  - Every game address goes through the version tables and `GA(NAME)`. The game ships as v1.0.0, v1.0.1 and
    v1.1.0, and unknown build IDs are refused.
  - Every patch in `tools/patches.py` asserts the original instruction word, so a wrong address fails loudly.
    Keep it that way.
  - Run `python tools/test_game_addresses.py` after touching addresses.

- **Both graphics backends.**
  - A rendering fix on Metal usually needs the Vulkan counterpart, and the other way round. Say in the pull
    request which backend you tested.
  - Workarounds for one GPU or driver go in the per-GPU defaults in `runtime/nvn/vulkan/exec.c`
    (`vk_exec_init`), keyed by vendor ID. Each one has a `LARECOMP_VK_*` switch to turn it off, and a comment
    saying which driver it was tested on.
  - macOS is Metal only. Don't add a MoltenVK path.
  - Minimums: macOS 15 on Apple Silicon; Windows and Linux with Vulkan 1.3; x86 CPUs with AVX2 and FMA.

- **Testing.**
  - A fix is verified only when you've seen it work in game. A clean build or a quiet log is not enough.
  - `tools/shot.sh <frame> [seconds]` runs the game with scripted input (`LARECOMP_INPUT`), warps
    (`LARECOMP_WARP_AT`) and seeds (`LARECOMP_RANDO_SEED`), and saves a screenshot. Many more debug hooks are
    documented where they are defined (grep `getenv("LARECOMP_`).
  - For test runs, use a scratch save folder (`LARECOMP_SAVE_DIR`, `tools/fresh_save.sh`) so the user's saves
    and settings stay untouched. Mute them with `LARECOMP_NO_AUDIO=1` and `LARECOMP_NO_MOVIE_AUDIO=1` unless the
    test is about sound.
  - Never close or restart a copy of the game the user is playing. Run one test window at a time (`shot.sh` takes
    a lock).
  - Randomizer changes: generate several seeds with different settings and check each one completes. For dungeon
    room shuffle, run `tools/doorjump_test.sh <seed>`.
  - The unit tests are listed in CONTRIBUTING "Tests".

- **Randomizer rules.**
  - Older seed IDs must keep loading (see `runtime/ui/rando_tab.mm`).
  - State that belongs to a seed resets on an in-place seed switch through `RT_ON_RESET` (`runtime/hle.h`).
  - Item pickup text goes through the game's own message box, never a custom pop-up.

- **Menus and text.**
  - Settings get short labels and vanilla defaults: no presets, no paragraphs.
  - New text is written in English only. Don't edit the translation tables (`runtime/ui/lang/`): we fill them for
    every language before each release, and untranslated text shows in English until then. A fix to an existing
    translation from a native speaker is welcome as its own pull request.
  - Player-facing text (menus, README, release notes, issue replies) is plain and short.

- **Code.**
  - Match the surrounding code: naming, structure, comment density.
  - Comments explain why (what the game does, which bug or seed showed it), not what the next line does.
  - Code copied from other projects must be MIT or CC0, and credited. GPL or unlicensed code can be read for
    ideas but not copied.

- **Pull requests.**
  - One change per pull request.
  - Say what changed, why, and how it was tested (platform, GPU, seeds, screenshots).
  - No personal paths, names or other personal information in code, comments, commits or pull requests.
