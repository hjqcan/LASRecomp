# Archive import, multiple versions, resumable compilation and save backups

## Supported inputs

- Extracted v1.0.0 game files remain supported.
- A base-game NSP, NSZ, XCI or XCZ can be selected under **Game Files → Base game archive**. The container is identified from its header, not its filename.
- **Update archive (optional)** applies a supported v1.1.0 NSP or NSZ update to the base archive. The base archive is still required.
- Both solid and block-compressed NCZ streams are supported. The importer reads the decompressed payload already stored inside NCZ; it does not download game files or keys.
- Encrypted NSP/NCA2/NCA3 content uses the player's selected local `prod.keys`. Supported key entries are `header_key`, derived `key_area_key_{application,ocean,system}_XX`, and `titlekek_XX`; master-key derivation is not implemented. Common tickets are read from the archive. A separate `title.keys` can supply encrypted title keys when a usable common ticket is absent. Personalized tickets are not decrypted. Keys and selected key paths are never persisted in settings or manifests.
- **Check selection** reads package format, Title ID and content version without extracting or installing. CNMT contents are checked against the expected game and base/update role. NSZ archives without keys can use XML labels, explicitly marked unverified; absent labels remain unknown. Executable Build IDs are always checked during import.
- XCI/XCZ imports read the **secure** HFS0 partition, checking its boundaries and prefix hashes. The gamecard's console-firmware `update` partition is ignored. Encrypted XCI content needs the same local keys as NSP; compressed XCZ program content uses the NCZ reader.
- Standalone NCA files are not accepted. Multi-application packages and combined base/update packages (including modified “super XCI” images) must be split into separate base and update packages.

Only these main executable Build IDs are accepted:

| Executable profile | Build ID |
| --- | --- |
| v1.0.0 | `ae16f71e002af8cb059a9a74c4d90f34ba984892` |
| Updated executable in v1.1.0 | `909e904af78ac1b8deefe97ab2ccdb51968f0ec7` |

The updated executable profile is also known from v1.0.1. A Build ID identifies executable layout, not the complete package version. The update package used for Windows validation has content metadata version `131072` (v1.1.0). The importer reads CNMT metadata when keys are available and accepts content version `0` for the base and `131072` for the update. XML-only NSZ metadata is a declaration, not cryptographic verification. An executable ID alone cannot distinguish v1.0.1 from v1.1.0.

## How the extension works

`tools/containers.py` validates PFS0/HFS0 file boundaries and gamecard hashes. `tools/import_game.py` reads/decrypts NCA sections on demand or decompresses NCZ to temporary files, locates the supported ExeFS/RomFS structures, and applies the update's BKTR relocation extents against the base RomFS section. It writes both the extracted resources and the RomFS image needed by the runtime.

Import is staged beside the destination. An existing installation is replaced only after extraction succeeds, and only when its contents are recognized as importer/runtime files. The source archives are opened read-only. Imports reject path traversal, duplicate filenames, invalid ranges, incomplete compression frames, and unknown executable builds. If the final rename fails, the previous installation is restored. If an old backup cannot be deleted after success, the importer reports its path for manual cleanup.

`tools/game_versions.py` maps the original patch addresses to the updated executable. The updated function at `main+0xdfadc0` grows by `0xd0` bytes; following text moves by that amount. Patch assertions remain enabled, including canonicalized relative branch targets. The changed function interior is not guessed. Runtime calls to three later functions use the same version mapping.

The generated game library records its main Build ID. Both its metadata file and its loaded table are checked against the selected game files, preventing code compiled from one executable version from being reused with another.

## Current limits

- Updated game files support **Vanilla mode**. The Randomizer tab and seed selection remain available with v1.0.0; updated-version randomizer patches are not validated.
- This uses the existing Windows Vulkan renderer. Support for the updated game files does not add Switch 2 HDR features to that renderer.
- Existing upstream beta limitations still apply. A startup/gameplay smoke test is not a full playthrough.
- Restart the application before replacing game files after a game library has been loaded.
- Allow at least 20 GiB of free space for import. Initial game-code compilation is performed locally and takes several minutes.
- NCZ images are limited to 16 GiB and Zstandard windows to 256 MiB. Block sizes from 16 KiB through 4 GiB are streamed. NSP program bytes are SHA-256 checked against CNMT; NCA section-header hashes and PFS0 metadata/ExeFS hash tables are checked. RSA signatures and the full RomFS IVFC hash tree are not verified. Plaintext NCZ payloads retain structural checks.

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

Cache receipts verify generated source and object hashes. Changes to the game executables, generator, compiler, runtime headers or build flags invalidate the cache; corrupt or incomplete objects are rebuilt. An exclusive lock prevents two processes from compiling the same installation. The installed library is replaced only after linking succeeds, with recovery for interrupted publication. A failed build leaves the previous library intact.

**Clear compilation cache** removes cached build files for the selected installation, keeping the installed library and saves. Use it to reclaim disk space or replace an older unrecognized cache. Import/extraction must finish before resumable compilation begins; interrupted archive decompression itself is not resumed.

## Save backups

Each save mount stores snapshots beside itself, for example `save/save_0.backups/` or `rando/<seed>/save/save_0.backups/`. All slots and screenshots in that save area are copied together before existing saves are opened and after the game commits a completed save. SD-card mounts are excluded. Unchanged automatic snapshots are skipped; the newest 20 valid snapshots are retained. A backup error is logged and does not block game saving.

Open **Settings → Saves → Backups** to choose a game version and Vanilla/seed profile, inspect timestamp, executable version, reason and size, or create a manual backup. Manual creation and restore require a fresh start screen, before a game library has loaded. Returning to the title screen alone is insufficient: restart LAS Recomp so no old save handles remain open.

Restore validates the complete file list, sizes and CRC32 checksums, stages the selected files, backs up current progress, then swaps the save directory with rollback on installation failure. Corrupt or unsafe snapshots leave current saves intact. CRC32 detects accidental corruption; it is not an authenticity signature. Backups stay on the same drive, so they do not protect against drive failure.

Limits: 1,024 files, 16 MiB per file and 128 MiB per save area. Symlinks/reparse points and traversal paths are refused. Retention leaves malformed or foreign snapshots untouched for manual inspection. No game assets or personal save data are shipped.

## Game language

Open the overlay with **F1**, then **Controls → System language → Language**. Settings immediately follows English, Simplified Chinese or Traditional Chinese; other game languages currently use the English settings UI. Game text changes after returning to the start screen and choosing Play again. Chinese selections use `zh-Hans` / `zh-Hant`, as required by the game. Legacy `zh-CN` / `zh-TW` settings are migrated when loaded. Both LF and CRLF configuration files are accepted.

The translated settings include tabs, options, descriptions, control remapping and confirmation dialogs. User-entered names, file paths, technical game identifiers and generator logs retain their original spelling. The UI uses installed CJK fonts; see [font configuration](../fonts/README.md). No proprietary font files are distributed.

## Build and test

Follow the platform build instructions for the native runtime toolchain. The helper also requires `zstandard==0.25.0` and `cryptography==50.0.2`; `make gen-helper`, Linux helper packaging, and the Windows setup script include it.

```sh
make -j
make gen-helper
```

The existing Windows helper packaging step `tools/pe_no_cfg.py` is required: the PyInstaller bootloader's CFG setting is incompatible with Unicorn's generated-code jumps. `make gen-helper` already runs that step.

For source-based imports:

```sh
python -m pip install zstandard==0.25.0 cryptography==50.0.2
python tools/import_game.py --base base.nsz --update update.nsz --output extracted
```

For NSP or XCI, use `--base base.xci --update update.nsp --keys /path/to/prod.keys`; add `--title-keys /path/to/title.keys` only if needed. Add `--inspect` (and omit `--output`) for a read-only metadata check. `--expected-version 1.0.0` or `1.1.0` rejects mismatched slot contents before installation.

Omit `--update` for v1.0.0. Replacing an existing recognized extraction requires `--replace`.

Synthetic parser/profile tests contain no game data:

```sh
python tools/test_import_game.py -v
python tools/test_nsp_import.py -v
python tools/test_xci_import.py -v
python tools/test_build_cache.py
python tools/test_gamebuild_resume.py
python tools/test_save_backups.py
```

Archive tests run in CI on Windows, Linux and macOS; native harnesses run on Linux/macOS in CI and Windows with the MSYS2 toolchain locally. They cover synthetic base/update imports, solid/block NCZ, gamecard partition hashes, relocation buckets, rollback, corruption, patch mapping, version isolation and actual compiler cancellation/resume. `tools/test_windows_processes.py` additionally checks Windows child-process cleanup. No game files, console keys, or generated game code are included in the repository. Encryption fixtures use arbitrary test bytes. Real encrypted NSP and XCI/XCZ imports have not yet been validated with player archives.

For local two-version regression, run `python tools/compatibility_smoke.py --exe PATH --base-root PATH --updated-root PATH --output NEW_DIRECTORY`. Both installations must already have compiled libraries. The script isolates settings and saves, captures images and writes a JSON result. Optional `--base-save-copy SAVE_MOUNT` and `--updated-save-copy SAVE_MOUNT` use separate, version-compatible save copies without modifying their sources; `--menu` captures the Controls overlay. Inspect the images as well as the exit status.

Windows validation with local game files: both main executable profiles pass all patch instruction assertions; v1.1.0 was recompiled, launched and tested through the opening dialogue and movement in the first room using the Vulkan backend. Full playthrough and native macOS/Linux gameplay validation remain outstanding.
