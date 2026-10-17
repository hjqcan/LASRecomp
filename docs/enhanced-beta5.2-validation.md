# Enhanced Beta 5.2 validation

Windows x64 prerelease, validated on 2026-10-17. Upstream `beta_5`
baseline: `b28ca75ea0fdf0db343d21f5b45908cb4278d30d`. The release package records
its exact source commit and every redistributed file's SHA-256.

This release integrates the completed enhanced features, managed installation
fixes and shared runtime improvements. The native checks below used the release
runtime whose SHA-256 is
`75d603d62cace08e42b68f6d9c01d3116533e8ba95be635ee95f8a1fac0b0ca1`.
Test installations, settings, seeds and saves were isolated from player data.

## Verified

| Check | Result |
| --- | --- |
| Source and CI | All seven workflows passed for runtime source `d1f2a9c`: CI, import/settings, save backups, photo state, resource mods, resumable builds and workshop/frame statistics. The shared CI builds Windows, Linux and macOS; these builds do not certify native gameplay on every platform. |
| Branch preservation | Upstream mirrors match `beta_5` at `b28ca75` and `main` at `136f679`. Open upstream PR #54 remains at `bee0b9d`; #56 remains at `da490c0`. Completed ACNH/shared-runtime feature commits are included in `enhanced`. |
| Real archive import | Frozen helpers imported the owned NSZ base and base+update archives. All 3,842 ExeFS/RomFS files in each version matched reference data by SHA-256. No real NSP/XCI/XCZ or encrypted archive was used in this release run; synthetic tests cover those formats and encryption paths. |
| Both executable profiles | Full generation with patch/hook instruction guards and compilation succeeded for 1.0.0 and 1.1.0. Managed publication produced matching module identities, content receipts and version-specific shortcuts. |
| Cancel/resume | The minimized 1.0.0 launcher cancelled a real packaged build, kept it unpublished, then resumed successfully. Twenty completed object files were reused with unchanged SHA-256 hashes; the final library contained 646 compiled units. |
| Interrupted 1.1.0 installation | The initial build cancelled with 27 completed objects. Recovery used the same candidate and a newer package directory. Because the compiler path is part of its identity, recovery rebuilt all 646 units; this is not counted as object-cache reuse. Publication and shortcut creation then succeeded. |
| Vanilla and fresh default seeds | Both versions reached `MarinTarinHouse` with default movie/game audio enabled. Each fresh seed loaded 20 validated code words and one data patch. The opening scenes and exported images were inspected. |
| Photo freeze/export/resume | Both vanilla versions and both fresh default seeds exported two byte-identical frozen PNGs with settings recipes, then resumed. The 1.0.0 square exports were 720 x 720; 1.1.0 cinema exports were 1280 x 536. |
| Native saves and shortcut startup | Both versions progressed from the house to `Field` and produced game-managed saves and automatic snapshots. Each managed-installation shortcut was launched through the Windows shell, loaded its newly generated autosave, and returned to `Field`/Mabe Village. Original source saves remained unchanged. |
| Backup and mod UI | The Simplified Chinese launcher created a manual backup, restored a changed scratch save to its exact original SHA-256, and preserved the pre-restore copy. Two local packs were imported; overlapping paths were shown, the conflicting pack stayed disabled, and both packs were disabled at the end without changing source resources. |
| Local co-op | Native 1.0.0 Link + BowWow tests passed: a P2 attack defeated the controlled enemy, and a house/field transition reacquired companion ownership across three scene epochs. Original saves were unchanged. Physical gamepads and full-game co-op are not certified. |
| Workshop and exports | A 60-second native 1.0.0 challenge timed out correctly, retained its isolated save, recorded 540 route samples with 15 distinct positions, opened the route viewer, and exported one frame-time capture. This checks export functionality, not a hardware performance benchmark. |
| Shared-runtime checks | Local synthetic tests covered shader snapshots, required-job waiting, actual Vulkan dispatch/stage reuse, compute shared memory, guest nonlocal returns, SIMD rounding, title guards, virtual P2 input/focus handling and long-path/reparse rejection. The packaged compiler passed the guest nonlocal-return ABI check. |
| Package inspection | Packaging uses explicit distributable inputs, verifies the LGPL FFmpeg binary, rejects private/game paths, and records every file hash. The host executable's symbol scan found no generated guest functions. ZIP and SHA-256 are published together. |

The first native attempt was interrupted during severe host memory pressure.
After the host recovered, the same runtime binary passed the native checks.
Some checks overlapped compilation or another isolated test process; no FPS
improvement or clean-machine benchmark is inferred from these runs.

## Randomizer generation matrix

The frozen seed helper generated all 12 cases for each version (24 runs):
default, keysanity, shops, digs, souls, minibosses, entrances, decoupled entrances,
coherent rooms, chaos rooms, madness rooms and combined options. Required outputs
and requested options were checked. Corresponding `logic.json` and `spheres.json`
matched between versions for this matrix and seed prefix `EnhancedBeta52`.

Only fresh default seeds received native startup/photo smoke coverage in this
release run. Generation and sampled logic parity do not establish full
playthrough parity or correct behavior for every shuffled setting combination.

## Reproduction and limits

Follow the [release checklist](enhanced.md#repeatable-release-checks). The local
entry points are `tools/validate_packaged_setup.py`, `tools/photo_smoke.py`,
`tools/rando_matrix.py`, `tools/coop_run.py`, `tools/research_run.py` and
`tools/workshop_smoke.py`. Use a fresh output/data directory, the packaged
helpers/compiler, each version's own compiled library and a compatible save
copy. Source fixtures and player saves must remain unchanged. These tools do
not upload game files.

When upgrading from Beta 5.1, ABI 4 requires a one-time game-library rebuild.
Keep existing installations, saves, seeds and settings; rebuild in the launcher.

The 1.1.0 randomizer and local Link + BowWow co-op remain experimental. ACNH is a
development adapter with normal installation disabled. Full playthrough parity,
online play, deterministic replay and native macOS/Linux gameplay are not
certified. Background preparation preserves required initialization commands;
new shader variants can still wait at first use. No game assets, generated game
libraries, keys, saves, seeds, test screenshots or personal settings are included
in the release assets.
