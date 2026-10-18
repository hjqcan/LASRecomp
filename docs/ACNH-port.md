# Animal Crossing: New Horizons

The enhanced runtime supports ACNH 1.0.0 (application ID `01006F8002326000`)
as a Windows beta. The launcher accepts supported base archives or complete
extracted folders. Updates, DLC, online play and amiibo are unavailable.

## Modules and runtime

| Module | Guest base |
| --- | --- |
| main | `0x7000000000` |
| sdk | `0x7004000000` |
| subsdk0 | `0x7006000000` |

The exact 32-byte Build IDs and initialization order are defined in
[`adapters/acnh/adapter.json`](../adapters/acnh/adapter.json). Other executable
versions are rejected. ACNH uses the generic generator profile; LAS patches,
randomizer hooks, memory probes and game-specific overlay pages are disabled.

The shared runtime provides the multimedia decoder, audio effects, offline
service responses and game-managed saves. Network services report unavailable
connections; NFC reports no reader. A beta designation does not imply full-game
or additional-platform compatibility.

## Setup and saves

Use **Game Files** in the launcher to select and import your base game, then
build and play. Managed installs use `games/acnh/v1.0.0` and separate saves under
`save/acnh/v1.0.0`. See [shared installations](multi-game.md) for cancellation,
resume and desktop shortcuts.

Complete extracted RomFS folders are supported. The adapter prefers `romfs.bin`
when present and accepts a validated folder when the image is absent. Movie
playback also requires `romfs/Movie/OpeningMovie.webm` from the same game data.

Keep the entire save mount together, including file headers and `Villager0`.
Use the game's **Save and end** action before closing. Do not interchange saves
with LAS or unsupported ACNH versions. A save created in another language may
show language-change confirmations before the title screen.

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
The older `ACNH_STRICT_*` names remain aliases. Report the selected version and
runtime error when diagnosing a failure; game data and saves do not belong in Git.
