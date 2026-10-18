# Animal Crossing: New Horizons

The Multi runtime supports ACNH **1.0.0 and 3.0.1** (application ID
`01006F8002326000`) as Windows betas. The launcher accepts supported base and
update archives or complete extracted folders. Other versions, DLC, online play
and amiibo are unavailable. Early island, interior, dialogue and save/reopen
checks do not establish full-game or additional-platform compatibility.

## Setup and saves

Use **Game Files** to import the base game and, for 3.0.1, its matching update,
then build and play. Exact module Build IDs and initialization order are defined
in [`adapters/acnh/adapter.json`](../adapters/acnh/adapter.json). Unsupported or
mixed module sets are rejected. See [shared installations](multi-game.md) for
cancellation, resume and desktop shortcuts.

| Version | Installation slot | Save folder |
| --- | --- | --- |
| 1.0.0 | `games/acnh/v1.0.0` | `save/acnh/v1.0.0` |
| 3.0.1 | `games/01006f8002326000/v3.0.1` | `save/01006f8002326000/v3.0.1` |

These paths are relative to the application's data folder. To migrate a 1.0.0
save, copy its entire save mount, including headers and `Villager0`, into the
3.0.1 slot and let the game convert it. Retain the original: converted saves
cannot be reopened by 1.0.0. Use the game's **Save and end** before closing.
A save created in another language may show language confirmations at startup.

Complete extracted RomFS folders are supported. The adapter prefers `romfs.bin`
when present and accepts a validated folder when the image is absent. Movie
playback also needs `romfs/Movie/OpeningMovie.webm` from the same game data.

## Optional 60 FPS + SpeedFix

The 3.0.1 setup page offers **Standard 30 FPS** (default) and an experimental
**60 FPS + SpeedFix** mode. Preparing the latter applies the pinned community
patch to a separate `.acnh60` installation and compiles its native library.
Switch modes before launching; restart the application after a library has been
loaded. Both modes use the same 3.0.1 save slot. The 1.0.0 slot is unchanged.

This uses **ACNH 60 FPS + SpeedFix by tmouh**, pinned to
`c3f162eb3e7e76b6cd30d2366d21233840673a9d`; see
[attribution and CC BY-NC 4.0 license](../adapters/acnh/speedfix60/README.md).
The bundled patch contains executable differences and guarded parameter edits,
not the game's original resources. Preparation verifies the original module and
resource hashes; launch verifies all seven patched files. Separate native code
identities prevent mixing normal and modified libraries. Original game files
are preserved.

The correction assumes sustained 60 FPS. Rendering below 60 slows the game;
this is not dynamic frame-rate correction. Reduce internal resolution or use
standard mode if the hardware cannot maintain 60. Local gameplay, tent entry
and exit, manual saving and a movement-distance comparison have passed, but
steady 60 FPS and every gameplay activity are not qualified.

## Shared runtime

ACNH uses the generic generator profile; LAS patches, randomizer hooks, memory
probes and game-specific overlay pages are disabled. The shared runtime provides
audio effects, multimedia decoding, offline service responses and game-managed
saves. The newer SDK uses bounded GNU symbol lookup, RELR relocations, revised
audio records and work-buffer sizing, and continuous-clock shared memory.
Network services report unavailable connections; NFC reports no reader.

Normal presentation honors interval 2 (30 FPS); the validated SpeedFix mode
uses interval 1. [Shared rendering](multi-game.md) covers shader preparation,
depth-copy fallback and scene-dependent compilation waits. Frame rate depends
on hardware and scene. Strict HLE/IPC and Vulkan diagnostics have passed the
tested standard-mode scenes; that coverage is not a promise of complete API
or graphics compatibility.

## Building from source

Follow [the Windows build guide](BUILD-windows.md), then use the extracted game
folder with its `exefs` and RomFS data:

```sh
make GAME_DIR=/path/to/game GEN=generated-acnh gen
make GAME_DIR=/path/to/game GEN=generated-acnh -j4
build/lasrecomp.exe /path/to/game --play --data-root /path/to/private-data
```

For development, use a copied save with `LARECOMP_SAVE_DIR` and enable
`LARECOMP_STRICT_HLE=1` / `LARECOMP_STRICT_IPC=1` to stop at missing services.
The older `ACNH_STRICT_*` names remain aliases. `LARECOMP_TEST_TIME` sets an
advancing guest clock only with an explicit isolated save directory; it does
not alter the host clock. Keep run evidence, game data and saves outside Git.
