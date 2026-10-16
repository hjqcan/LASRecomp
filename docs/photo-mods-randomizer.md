# Photo mode, resource mods and v1.1.0 randomizer

These features build on the archive importer, version slots and save backups. Use the selected installation's own saves; do not transfer saves between executable versions.

## Photo mode

During gameplay press **F2**, or open **Settings → Photo mode → Enter photo mode**. The next complete scene is frozen. **F2** or **Escape** resumes; **Tab** hides the controls; **F12** saves a PNG under `photos/` in the application's data directory. The panel also has Save and Resume buttons.

Adjust zoom (1–4x), horizontal/vertical framing, exposure and saturation. Photos use the current window resolution and exclude the photo controls and FPS counter. Zoom crops the rendered scene: this is not a free 3D camera, depth-of-field editor or supersampling renderer. Interface hiding recognizes the final scene composite; if a scene lacks that pass, the panel reports that hiding is unavailable. Changing the interface setting renders a fresh frame before freezing again. Save before entering if preserving an exact game state matters; photo mode is not a save-state system.

The presentation and color paths are implemented for Vulkan and Metal. Windows Vulkan has been exercised in game; native Metal rendering still needs hardware validation.

## Resource mods

Restart LAS Recomp and choose an installed version. In **Settings → Mods**, import a folder containing `mod.ini` and `romfs/`, select its row to inspect metadata, then enable it. Changes apply at the next game start. A populated current save is backed up before changing the selection; backup failure prevents the change. Mod changes are unavailable after game code has loaded, including after returning to the start screen.

Example manifest (UTF-8):

```ini
format=1
id=example-resources
name=Example resources
version=1.0
author=Your name
description=Describe the resources this pack replaces.
game_versions=1.0.0,1.1.0
```

Place each replacement under its existing relative RomFS path, such as `romfs/region_common/...`. The template in `examples/mods/template/` contains no game files; add your own compatible resources before importing. Imported files are staged and checked before publication. The source folder and installed game files are preserved. An existing mod ID is not overwritten.

Only replacement files are supported: adding new virtual files/directories, executable patches, scripts and automatic downloads are outside this format. Each target must already exist in the selected game's RomFS. Limits are 64 packs, 4,096 files per pack, 1 GiB per file, 8 GiB per pack and fewer than 16 directory levels. Linked/reparse paths and unsafe filenames are rejected. Resource contents must still be compatible with the game; a manifest alone cannot establish that.

Enable lists are separate for `extracted/`, `games/v1.0.0/` and `games/v1.1.0/`. Two enabled mods cannot replace the same path. Missing/invalid enabled packs stop startup with an error and remain visible so they can be disabled. An explicit unmanaged game directory does not load a managed mod list. Resolution order is **randomizer seed → enabled mod → original resource**, for ordinary resources and movies. Built-in texture identification deliberately reads mod-or-original references, independently of the randomizer seed, to avoid applying texture changes twice.

## Randomizer on v1.1.0

Select v1.1.0 before generating a seed. New seeds record their version in `game-version.txt`; launch and backup lists filter incompatible seeds. Legacy seeds without a marker remain v1.0.0 seeds. Generate a fresh v1.1.0 seed instead of relabeling an older one, and keep each seed's saves with that seed.

Generation validates the original executable bytes for the randomizer code/data patch catalog before emitting alternate compiled code. Runtime patch loading checks the Build ID, directives, offsets, words and duplicates before applying any patch. Unknown or incompatible patches fail startup instead of silently starting Vanilla. The updated executable uses LAS's existing `1.0.1.pchtxt` filename because that file targets its verified Build ID; the user-facing game profile remains v1.1.0.

Updated game data also inserts an `EventFlags.SetFlag(FirstClear)` edge into `SinkingSword.bfevfl`. Generation recognizes that exact extra edge and restores the original loose-item event contract before randomizing it. Unknown event layouts are rejected. This compatibility adjustment is version-specific and does not modify the installed RomFS.

This support is experimental. Local Windows checks cover seed generation, validated patch loading, opening dialogue and movement, not a complete playthrough or every combination of dungeon/entrance settings. Keep backups and report the game version, seed and settings with any progression issue.

## Tests

The following use synthetic inputs and require no game files:

```sh
python tools/test_rando_profiles.py -v
python tools/test_rando_event_compat.py -v
python tools/test_mods.py
python tools/test_photo.py
python tools/test_ui_localization.py --font
```

Native harnesses use `CC`/`CXX` (Clang by default). The event compatibility test runs on all three CI operating systems; the C/C++ harnesses run on Linux/macOS CI and locally with the Windows MSYS2 toolchain. Font checks require a locally installed CJK font.

Gameplay checks must inspect screenshots and confirm a gameplay level in the log. Successful process exit or a title-screen screenshot alone does not establish that a randomized seed is playable. Use independent settings and save folders; never include game resources, generated game code, fonts or personal saves in test fixtures or commits.
