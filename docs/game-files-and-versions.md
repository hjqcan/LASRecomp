# Game-file setup, multiple versions, resumable compilation and save backups

## Supported inputs

Multi accepts a legally obtained, already-extracted game folder containing `exefs`
and `romfs`. Choose **Game Files → Game files folder**, then **Set up game files**.
The launcher identifies reviewed games and versions from their executable modules.
For an updated version, supply its complete extracted files; separate base/update
packages are not accepted. Multi does not open NSP, NSZ, XCI, XCZ or NCA game
archives, read console keys, decrypt content, or apply update packages.

Existing installed games remain usable. Their installation slots, saves and
resumable compilation are independent of the source-folder selection.

For Link's Awakening, these main executable profiles are supported:

| Executable profile | Build ID |
| --- | --- |
| v1.0.0 | `ae16f71e002af8cb059a9a74c4d90f34ba984892` |
| Updated executable in v1.1.0 | `909e904af78ac1b8deefe97ab2ccdb51968f0ec7` |

The updated executable profile is also known from v1.0.1. A Build ID identifies
executable layout, not the complete content version. Use the reviewed v1.1.0
content with that profile. All required modules must match the adapter's complete
32-byte Build IDs; naming a folder after a supported release is insufficient.
See [shared installations](multi-game.md) for other supported games.

## Installation and validation

Folder setup copies the source into an isolated candidate, validates module
identities and required resource roots, compiles locally, and publishes the
installation only after its library passes verification. A failed copy or build
keeps the previously published installation selected. Source files are not modified.
An existing plaintext `romfs.bin` alongside the extracted folders remains usable
by installations that already contain it; no game archive is processed to create it.

`tools/game_versions.py` maps reviewed patch addresses for the two LAS profiles.
Patch assertions remain enabled. Generated libraries record their module and
adapter identities, preventing a library from being reused with incompatible
game files.

## Current limits

- Updated LAS files support Vanilla mode and experimental Randomizer mode with newly generated, version-specific seeds. See [validation details](photo-mods-randomizer.md#randomizer-on-v110).
- A startup smoke test does not establish full-game compatibility.
- Restart Multi before replacing game files after a library has loaded.
- Allow enough free space for a complete source-folder copy and its local build cache; initial compilation takes several minutes.

## Keep both versions installed

Choose **Game version** on the start screen or in Game Files before importing:

| Selection | Game files | Vanilla saves |
| --- | --- | --- |
| Existing installation | `extracted/` | `save/save_<type>/` |
| v1.0.0 | `games/v1.0.0/` | `save/v1.0.0/save_<type>/` |
| v1.1.0 | `games/v1.1.0/` | `save/v1.1.0/save_<type>/` |

The existing installation is preserved without migration. New slots reject an executable that does not match their version. Each slot keeps its own compiled library and build cache. Randomizer saves remain under their seed folders. The selection is remembered; an explicit game-folder command-line argument still takes precedence. Restart the app before switching installations after any game code has loaded.

Keep saves with the game version that created them. In Windows testing, v1.0.0 aborted while opening a v1.1.0 save; a fresh v1.0.0 save worked. Version slots therefore do not automatically copy or downgrade saves.

## Resume interrupted compilation

During compilation, choose **Cancel compilation** (or press Escape / controller B). Completed object files and generated source stay in the selected installation's `.gamebuild/` directory. Choose **Build or resume** to continue. Closing the app also stops its compiler processes; Windows cancellation terminates only that build's child process trees.

Cache receipts verify generated source and object hashes. Changed game executables or generator inputs trigger regeneration. Unchanged generated sources can reuse their objects; compiler, runtime ABI/header or build-flag changes invalidate compiled objects. Corrupt or incomplete objects are rebuilt. An exclusive lock prevents two processes from compiling the same installation. The installed library is replaced only after linking succeeds, with recovery for interrupted publication. A failed build leaves the previous library intact.

**Clear compilation cache** removes cached build files for the selected installation, keeping the installed library and saves. Use it to reclaim disk space or replace an older unrecognized cache. The source-folder copy must finish before resumable compilation begins.

## Save backups

Each save mount stores snapshots beside itself, for example `save/save_0.backups/` or `rando/<seed>/save/save_0.backups/`. All slots and screenshots in that save area are copied together before existing saves are opened and after the game commits a completed save. SD-card mounts are excluded. Unchanged automatic snapshots are skipped; the newest 20 valid snapshots are retained. A backup error is logged and does not block game saving.

Open **Settings → Saves → Backups** to choose a game version and Vanilla/seed profile, inspect timestamp, executable version, reason and size, or create a manual backup. Manual creation and restore require a fresh start screen, before a game library has loaded. Returning to the title screen alone is insufficient: restart Multi so no old save handles remain open.

Restore validates the complete file list, sizes and CRC32 checksums, stages the selected files, backs up current progress, then swaps the save directory with rollback on installation failure. Corrupt or unsafe snapshots leave current saves intact. CRC32 detects accidental corruption; it is not an authenticity signature. Backups stay on the same drive, so they do not protect against drive failure.

Limits: 1,024 files, 16 MiB per file and 128 MiB per save area. Symlinks/reparse points and traversal paths are refused. Retention leaves malformed or foreign snapshots untouched for manual inspection. No game assets or personal save data are shipped.

## Game language

Open the overlay with **F1**, then **Controls → System language → Language**. Settings immediately follows English, Simplified Chinese or Traditional Chinese; other game languages currently use the English settings UI. Game text changes after returning to the start screen and choosing Play again. Chinese selections use `zh-Hans` / `zh-Hant`, as required by the game. Legacy `zh-CN` / `zh-TW` settings are migrated when loaded. Both LF and CRLF configuration files are accepted.

The translated settings include tabs, options, descriptions, control remapping and confirmation dialogs. User-entered names, file paths, technical game identifiers and generator logs retain their original spelling. The UI uses installed CJK fonts; see [font configuration](../fonts/README.md). No proprietary font files are distributed.

## Build and test

Follow the platform toolchain instructions, then build the runtime and helper:

```sh
make -j
make gen-helper
```

The helper inspects extracted modules and performs reviewed local preparation:

```sh
python tools/gen.py --game-setup --catalog
python tools/gen.py --game-setup --inspect-exefs /path/to/extracted/exefs
python tools/adapter.py probe --exefs /path/to/extracted/exefs --output inventory.json
```

Windows helper packaging runs `tools/pe_no_cfg.py` because the PyInstaller
bootloader's CFG setting is incompatible with Unicorn's generated-code jumps.

Synthetic tests contain no game data or console keys:

```sh
python tools/test_game_setup.py -v
python tools/test_game_catalog.py -v
python tools/test_adapter_probe.py -v
python tools/test_game_versions.py -v
python tools/test_build_cache.py
python tools/test_gamebuild_resume.py
python tools/test_save_backups.py
```

CI checks extracted-module identification, rejected archive/key arguments,
version isolation, installation publication and compiler cancellation/resume.
No game files, generated game libraries or personal saves are distributed.
