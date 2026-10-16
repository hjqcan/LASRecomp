# NSZ import and v1.1.0 support

## Supported inputs

- Extracted v1.0.0 game files remain supported.
- A base-game NSZ can be selected directly under **Game Files → Base game NSZ**.
- **Update NSZ (optional)** applies the supported v1.1.0 update to the base archive. The base archive is still required.
- Both solid and block-compressed NCZ streams are supported. The importer reads the decompressed payload already stored inside NCZ; it does not download game files or keys.
- Encrypted, uncompressed NCA/NSP/XCI import is outside this extension's scope. Use an extracted game folder for those cases.

Only these main executable Build IDs are accepted:

| Executable profile | Build ID |
| --- | --- |
| v1.0.0 | `ae16f71e002af8cb059a9a74c4d90f34ba984892` |
| Updated executable in v1.1.0 | `909e904af78ac1b8deefe97ab2ccdb51968f0ec7` |

The updated executable profile is also known from v1.0.1. A Build ID identifies executable layout, not the complete package version. The update package used for Windows validation has content metadata version `131072` (v1.1.0). The importer identifies the executable profile; it does not decrypt package metadata to determine the marketing version.

## How the extension works

`tools/import_game.py` validates PFS0 file boundaries, decompresses NCZ to temporary files, locates the supported ExeFS/RomFS structures, and applies the update's BKTR relocation extents against the base RomFS section. It writes both the extracted resources and the RomFS image needed by the runtime.

Import is staged beside the destination. An existing installation is replaced only after extraction succeeds, and only when its contents are recognized as importer/runtime files. The source archives are opened read-only. Imports reject path traversal, duplicate filenames, invalid ranges, incomplete compression frames, and unknown executable builds. If the final rename fails, the previous installation is restored. If an old backup cannot be deleted after success, the importer reports its path for manual cleanup.

`tools/game_versions.py` maps the original patch addresses to the updated executable. The updated function at `main+0xdfadc0` grows by `0xd0` bytes; following text moves by that amount. Patch assertions remain enabled, including canonicalized relative branch targets. The changed function interior is not guessed. Runtime calls to three later functions use the same version mapping.

The generated game library records its main Build ID. Both its metadata file and its loaded table are checked against the selected game files, preventing code compiled from one executable version from being reused with another.

## Current limits

- Updated game files support **Vanilla mode**. The Randomizer tab and seed selection remain available with v1.0.0; updated-version randomizer patches are not validated.
- This uses the existing Windows Vulkan renderer. Support for the updated game files does not add Switch 2 HDR features to that renderer.
- Existing upstream beta limitations still apply. A startup/gameplay smoke test is not a full playthrough.
- Restart the application before replacing game files after a game library has been loaded.
- Allow at least 20 GiB of free space for import. Initial game-code compilation is performed locally and takes several minutes.
- NCZ images are limited to 16 GiB and Zstandard windows to 256 MiB. Block sizes from 16 KiB through 4 GiB are streamed. Plaintext sections are checked structurally; encrypted NCA signatures and integrity metadata are not verified.

## Game language

Open the overlay with **F1**, then **Controls → System language → Language**. Settings immediately follows English, Simplified Chinese or Traditional Chinese; other game languages currently use the English settings UI. Game text changes after returning to the start screen and choosing Play again. Chinese selections use `zh-Hans` / `zh-Hant`, as required by the game. Legacy `zh-CN` / `zh-TW` settings are migrated when loaded. Both LF and CRLF configuration files are accepted.

The translated settings include tabs, options, descriptions, control remapping and confirmation dialogs. User-entered names, file paths, technical game identifiers and generator logs retain their original spelling. The UI uses installed CJK fonts; see [font configuration](../fonts/README.md). No proprietary font files are distributed.

## Build and test

Follow the platform build instructions for the native runtime toolchain. The helper also requires `zstandard==0.25.0`; `make gen-helper`, Linux helper packaging, and the Windows setup script include it.

```sh
make -j
make gen-helper
```

The existing Windows helper packaging step `tools/pe_no_cfg.py` is required: the PyInstaller bootloader's CFG setting is incompatible with Unicorn's generated-code jumps. `make gen-helper` already runs that step.

For source-based imports:

```sh
python -m pip install zstandard==0.25.0
python tools/import_game.py --base base.nsz --update update.nsz --output extracted
```

Omit `--update` for v1.0.0. Replacing an existing recognized extraction requires `--replace`.

Synthetic parser/profile tests contain no game data:

```sh
python tools/test_import_game.py -v
```

The same tests run in CI on Windows, Linux and macOS. They cover complete synthetic base/update imports, both NCZ compression modes, multiple relocation buckets, source preservation, rollback, low disk space, corrupt data and patch-address mapping. No game files, keys, or generated game code are included in the repository.

Windows validation with local game files: both main executable profiles pass all patch instruction assertions; v1.1.0 was recompiled, launched and tested through the opening dialogue and movement in the first room using the Vulkan backend. Full playthrough and native macOS/Linux gameplay validation remain outstanding.
