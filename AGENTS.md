# Fork maintenance

- The maintained branch is `multi` and its public name is Multi. The repository remains `hjqcan/LASRecomp`, with `main` as the default. New release tags use `multi-beta*`; retain existing historical tags and internal filenames/data paths as documented in `docs/multi.md`.

- `main` and `beta_5` mirror the corresponding `peiikai/LASRecomp` branches. Update them only by fast-forwarding to upstream. Do not put fork features on either branch.
- `multi` is the maintained fork edition. Merge upstream `beta_6` updates into it; resolve conflicts by preserving the enhancements listed in `docs/multi.md` alongside upstream fixes.
- Develop and commit ongoing work, including Z-A research, directly on `multi` by default. Merge official updates there too. Use temporary branches only when isolation is actually needed or for focused upstream PRs, and delete completed integration branches once their commits are on remote `multi`.
- Do not reset `multi` to upstream, discard features rejected by upstream, or overwrite feature branches backing open PRs. Upstream submissions remain small, independent PRs against `beta_5` and follow the maintainer's scope requirements.
- Public `multi` accepts already-extracted game folders only. Do not add game archive parsing/decompression, key-file handling, NCA decryption, or update-package application to source, helper bundles or UI. Preserve translated UI, version slots and experimental updated-version randomizer support; those fork features stay out of upstream PRs unless upstream changes its policy.
- Multi-game work also belongs on `multi`; follow `docs/multi-game.md`. Keep common runtime fixes reusable, gate title-specific behavior by adapter, and preserve existing LAS behavior and saves. A catalog entry or successful compilation alone must not mark a new game playable.
- Source integration and release qualification are separate: unfinished ports may be checked into `multi` with explicit development gates and documented failures. They do not need to remain on unpublished branches until playable. Keep prerelease acceptance checks in place.
- Verify the Windows package and the regression checklist in `docs/multi.md` before publishing a fork prerelease. Do not package game assets, generated game code, keys, saves, seeds or personal settings.
- Keep documentation focused on current usage, architecture and limits. Do not commit dated validation diaries, per-run reports or machine-specific evidence.
- Prefer focused tests for changed behavior and reuse existing CI jobs. Avoid duplicate smoke scripts, source-text assertions and repeated test runs that add no coverage.
- Subagents, if explicitly authorized, must use GPT-6 or newer models.

# Notes for agents

- **What this is.** LAS Recomp statically recompiles the Switch release of *Link's Awakening* into a native app.
  - `tools/gen.py` turns the game's executables into C, which is built on the player's machine from their own
    extracted game files.
  - A host stands in for the Switch OS and services (`runtime/kernel.c`, `loader.c`, `services/`).
  - The game's NVN graphics are drawn with Metal on macOS and Vulkan on Windows and Linux (`runtime/nvn/`).
  - The randomizer and most features live in `runtime/hle/`, `runtime/ui/` and `tools/rando/`.
  - [README.md](README.md) is the player-facing page. [CONTRIBUTING.md](CONTRIBUTING.md) has the layout, building,
    tests and pull request rules.

- **Required reading.** Before you change anything, read all of [CONTRIBUTING.md](CONTRIBUTING.md) and
  [docs/architecture.md](docs/architecture.md). Then read the doc for the area you are working in from the table
  below ([debug-hooks](docs/debug-hooks.md), [vulkan-gpu-defaults](docs/vulkan-gpu-defaults.md),
  [save-format](docs/save-format.md), the build guides). Don't skim them or work from guesses about how the project
  is set up. If what you find in the code disagrees with a doc, say so in your pull request.

- **Where to start:**

  | Task | Read first |
  |---|---|
  | How the parts fit together | [docs/architecture.md](docs/architecture.md) |
  | Building | README "Building from source", [docs/BUILD-windows.md](docs/BUILD-windows.md), [docs/linux.md](docs/linux.md), [docs/android.md](docs/android.md) |
  | A crash or hang | the log (`lasrecomp.log` in the data folder): the crash line names the guest address (`main+0x...`). Map it with `tools/sym.py` and read the code there before changing anything. Debug switches: [docs/debug-hooks.md](docs/debug-hooks.md) |
  | Rendering | `runtime/nvn/cmdbuf.c` (recording), `renderer.m` / `metal.m` (Metal), `vulkan/exec.c` (Vulkan), `maxwell/translate.c` (shaders); per-GPU Vulkan defaults: [docs/vulkan-gpu-defaults.md](docs/vulkan-gpu-defaults.md) |
  | Game behaviour or a new feature | `runtime/hle/` and the hooks in `tools/patches.py` and `runtime/hle/hooks.c` |
  | Saves | [docs/save-format.md](docs/save-format.md), `runtime/services/fs.c` |
  | Game addresses | CONTRIBUTING "Project layout" (`runtime/game_addresses/`, `GA(NAME)`, `tools/version_map.py`) |
  | Randomizer logic | `tools/rando/` and `third_party/LAS-Randomizer` (local changes marked "LAS Recomp") |
  | Menu text | `python3 tools/menu_strings.py check` |

- **No Nintendo files, ever.**
  - Never commit, attach or paste game files, extracted data, generated code (`generated/`), fonts, assets or seed
    folders (`rando/`).
  - Game files stay outside the repository.
  - Refer to them only as the player's own extracted game files. Don't write about how they are obtained, in code,
    comments, docs, commits or pull requests.

- **Upstream contributions.** Use the highest-numbered upstream `beta_<n>` branch for focused upstream pull requests. Fork development follows the Multi policy above.

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
    (`vk_exec_init`), keyed by vendor ID. Each one has a `LARECOMP_VK_*` switch to turn it off, a comment saying
    which driver it was tested on, and a row in [docs/vulkan-gpu-defaults.md](docs/vulkan-gpu-defaults.md).
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
