# Shared game installations

The enhanced runtime uses `tools/game_catalog.py` and its generated native
catalog to identify each reviewed game/version by every module's full 32-byte
Build ID. LAS 1.0.0 and 1.1.0 use two modules; ACNH 1.0.0 uses three. ACNH is
still marked as development while the gameplay and save gates in
[ACNH-port.md](ACNH-port.md) remain open.

Archive inspection accepts NSP, NSZ, XCI and XCZ. The default is automatic game
identification. Readable content metadata provides a preliminary identity;
metadata-free NCZ packages are inspected through their executable modules.
Filenames do not select the game or version. Import validates module identities,
base/update roles and RomFS structure before publishing the installation.
The development override is intended for local port testing:

```
python tools/import_game.py --base /path/to/game.nsz --inspect
python tools/import_game.py --base /path/to/game.nsz --output /path/to/install --allow-development
```

Encrypted content uses locally selected player key files. No keys, extracted
game code, game assets, generated sources, game libraries or saves belong in
source commits or distributed runtime packages.

## Managed locations

| Launch ID | Game folder | Vanilla save folder |
| --- | --- | --- |
| `existing` | `extracted` | `save` |
| `las/1.0.0` | `games/v1.0.0` | `save/v1.0.0` |
| `las/1.1.0` | `games/v1.1.0` | `save/v1.1.0` |
| `acnh/1.0.0` | `games/acnh/v1.0.0` | `save/acnh/v1.0.0` |

Locations are relative to the application's data folder. Existing LAS locations
and setting values remain valid. A custom ACNH folder uses a save namespace
containing its program ID and executable Build ID. Test runs can explicitly
override the save directory.

The launcher inspects a selected archive and selects the matching managed slot.
The native importer checks the expected title as well as version, so two games
that both use version 1.0.0 cannot overwrite each other's slot. Generated-source
and object caches include the adapter and all modules; an incompatible SDK or
module set is rejected before compilation replaces a working library.

## Desktop launch

On Windows, successful setup creates a shell link on the desktop returned by
`SHGetKnownFolderPath`, including redirected/OneDrive desktops. It records the
game's stable launch ID and absolute data folder:

```
lasrecomp.exe --play --game acnh/1.0.0 --data-root "C:\Games\Recomp"
```

Existing installations can use `--create-shortcut` or the launcher's shortcut
button. A shortcut is updated only when its description identifies the same
installation; unrelated same-name files are preserved. `icon.ico` in an
installation supplies its icon when present; otherwise the runtime icon is used.
Windows 10 1903 and newer use the embedded UTF-8 process manifest for native
file paths and command-line arguments. Shell-link paths use wide APIs directly.

Tests use `LARECOMP_SHORTCUT_DIR` to keep fixture links off the real desktop.
`tools/test_game_shortcut.py` reads the resulting links through the Shell API and
checks Unicode paths, arguments, duplicate updates and preservation of unrelated
files. `tools/test_game_profiles.py` checks exact managed locations, launch IDs
and distinct save folders. These tests do not replace real import, compilation,
desktop launch and gameplay verification.
