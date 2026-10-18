# Resource mods

The Mods tab imports a local folder of replacement resources for the selected installation. Multi game setup supports folders and archives; see [import and versions](NSZ-and-v110.md). Resource packs themselves contain replacement files, not executable patches.

## Install and enable

1. Set up your own game files through Game Files and restart Multi.
2. Open **Settings → Mods → Import mod folder**. Choose a folder containing `mod.ini` and `romfs/`.
3. Select a row to inspect the pack's metadata, file count and size. Enable its checkbox, then start the game.

Changes are locked after game code loads, including after returning to the start screen; restart Multi before changing the selection. Imported packs are disabled initially. Disabling a pack restores the original resource on the next game start. Back up saves before trying resource changes: the manager validates file paths and conflicts, not the contents' compatibility with the game.

## Pack format

`mod.ini` is UTF-8 (an optional BOM and CRLF are accepted):

```ini
format=1
id=example-resources
name=Example resources
version=1.0
author=Your name
description=Describe the resource changes here.
game_versions=1.0.0,1.1.0
```

`format`, `id` and `name` are required. IDs begin with a lowercase ASCII letter and contain only lowercase letters, digits, `-` and `_`, with at most 63 bytes. Metadata has bounded lengths; duplicate/unknown fields, invalid UTF-8 and control characters are rejected. Displayed metadata uses the existing UI font's glyph coverage.

Put replacements under their exact existing RomFS paths, for example `romfs/region_common/...`. Only existing regular files can be replaced. Adding virtual files/directories, scripts and code patches is outside this format. Resource formats and sizes must still be acceptable to the game.

Limits: 64 installed packs, 4,096 files per pack, 1 GiB per file, 8 GiB per pack, fewer than 16 directory levels and an 8 KiB manifest. Symlinks/reparse paths, traversal and unsafe filenames are refused. Imported resources are copied through a staging directory and checked before publication. The source folder and installed game files are preserved; an existing mod ID is never overwritten.

## Storage and conflicts

Packs live under `<data root>/mods/<id>/`; separate enable lists are maintained for the existing installation, v1.0.0 and v1.1.0. Each installation uses its selection for Vanilla and compatible randomizer seeds. Two enabled packs cannot replace the same path; comparisons fold ASCII case so a conflict is detected consistently across platforms.

Read priority is **randomizer seed → enabled mod → installed game** for the guest RomFS and movies. Built-in texture identification uses mod-or-original references, retaining its existing seed-independent cache; mod selection stays fixed until restart. A seed's additional virtual files remain managed by the existing randomizer code.

Missing or invalid enabled packs cause startup to fail with an error, instead of silently changing resources. Restart, open Mods and disable the invalid row. If an enable list is malformed, close Multi and remove only the affected list to disable its packs; installed resources remain intact. Changing selection requires a successful backup when current saves exist.

## Shareable presets

**Mods → Mod presets** saves and imports `.lasmods` selections. Files live in `<data root>/mod-presets/` and use the same lowercase naming rules as mod IDs. Up to 64 presets of at most 16 KiB are accepted; existing names are not overwritten.

```ini
format=1
game_version=1.1.0
mod.example-resources=1.0
```

Each `mod.<id>` value must match the installed pack's `version`. An empty set is valid and disables every pack. Import validates the preset syntax without installing resources. Apply checks the selected game version, all installed packs, versions, original paths and cross-pack conflicts before replacing the enabled list. Failure leaves the existing selection unchanged. Applying also uses the existing save-backup gate. Presets contain no resources, downloads or executable commands. Matching version labels are not content hashes; resource compatibility still depends on the packs themselves.

## Validation

`python tools/test_mods.py` compiles the real store with `CXX` (Clang by default) and uses synthetic resources. It covers import/source preservation, conflict rejection without changing the saved selection, disable/reload, invalid manifests/configuration, UTF-8 and size bounds, linked paths where supported, independent data roots and missing-pack recovery. CI runs on Linux, macOS and Windows; the Windows runtime itself is built with MSYS2 Clang.

For manual testing, use independent settings/saves and a local compatible replacement. Import through the folder dialog, inspect metadata, enable it, verify the replacement in game, restart and disable it, then verify the original resource. Import a second pack targeting the same path and confirm its conflicting checkbox is unavailable. Check randomizer priority and ensure the source/game files remain unchanged. Keep game assets and personal saves out of commits and fixtures.
