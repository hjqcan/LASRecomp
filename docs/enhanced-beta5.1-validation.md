# Enhanced Beta 5.1 validation

Windows x64 prerelease, validated on 2026-10-17. Upstream baseline: `beta_5` commit `6e7a15c0b284568df2e992ca50f01a207d6fb37f`, including the latest Vulkan frame-pacing fix. The release's exact source commit and file hashes are recorded in `package-manifest.json`.

## Verified

| Check | Result |
| --- | --- |
| Real NSZ base and base+update imports | Frozen release helper imported both supported versions; all 3,842 ExeFS/RomFS files per version matched the reference installations by SHA-256 |
| NSP / XCI / XCZ and encryption paths | Synthetic fixtures passed on Windows, Linux and macOS; no player NSP/XCI/XCZ archive was used |
| Both executable profiles | Complete code generation with patch/hook instruction guards and native compilation succeeded |
| Packaged compiler | Cancelled after 65 seconds; resume reused both generated modules and 41 compiled game objects; all 42 completed object-file hashes remained identical (the entry object is always rebuilt) |
| Vanilla and Randomizer startup | Both versions reached `MarinTarinHouse` with default movie/game audio and isolated compatible saves; automatic save snapshots were produced |
| Startup concurrency | A repeated save-mount collision was reproduced in both versions, fixed by serializing the complete mount lifetime, then passed 12 consecutive alternating-version starts; a synthetic 50-iteration race test covers concurrency, failures, nesting and reset |
| Photo mode | Both versions froze the opening-room scene, saved two identical PNGs, hid the game's interface in the photos and resumed; images were inspected |
| Mod UI | Imported a local resource pack, enabled and disabled it in the v1.1.0 installation; synthetic tests additionally cover conflicts, invalid packs and version isolation |
| Backup UI | Created a manual v1.1.0 snapshot, changed only a scratch save, restored through the launcher, checked exact SHA-256 restoration and the preserved pre-restore copy |
| Languages and updates | English, Simplified Chinese and Traditional Chinese launcher/settings screenshots inspected; 693 catalog entries and installed CJK glyph coverage checked; official tags ignored and newer enhanced tags detected |
| Clean package | Explicit allowlist, dependency licenses, LGPL FFmpeg binary verification, per-file manifest and ZIP SHA-256; no game executables/assets, generated game library, console keys, saves or personal settings |

## Randomizer generation matrix

The frozen seed helper successfully generated each case for both v1.0.0 and v1.1.0 (10 runs total):

- Default options.
- Combined all-keys keysanity, random-price shopsanity, Trendy Game, digs, boss/miniboss souls, enemies/minibosses, hookshot-anything, rooster/ghost pets, music/environment randomization and dungeon entrances.
- Coherent dungeon room shuffle plus dungeon entrance shuffle.
- Chaos dungeon room shuffle.
- Madness dungeon room shuffle.

Generation success checks executable patches and data editing. Gameplay smoke tests used fresh default seeds; the combined/shuffled configurations have not received complete playthroughs. This is not a claim of randomizer parity across every option or combination. Updated-version Randomizer remains experimental.

## Reproduction and limits

The maintained checklist is in [enhanced.md](enhanced.md#repeatable-release-checks). Portable regression tests live in `tools/test_*.py` and [GitHub Actions](https://github.com/hjqcan/LASRecomp/actions?query=branch%3Aenhanced). `tools/photo_smoke.py` and `tools/compatibility_smoke.py` exercise local game installations with isolated output directories; they never upload game files.

Only Windows Vulkan gameplay was exercised. Native macOS/Linux gameplay, Metal rendering, full playthroughs and real encrypted NSP/XCI/XCZ archives remain unverified. Existing upstream beta limitations still apply. Game resources and personal saves used for local checks are not repository fixtures or release assets.
