# Third/fourth-round development validation

Local validation on Windows x64/Vulkan, 2026-10-17, continuing from Enhanced commit `92a0097`. This records source-branch development after the published Beta 5.1 package; no new release archive is implied.

## Delivered scope

- Photo aspect ratios and cropped PNG export, composition guides, contrast/vignette, monochrome/vivid presets and PNG settings recipes.
- Shareable `.lasmods` enable presets with complete game/pack-version and conflict validation before selection replacement; conflicting resource paths are visible.
- Shareable destination/time/damage challenge presets, read-only source-save snapshots, isolated run saves and results.
- Observed route/health/controller capture and a local 2D timeline viewer.
- Presentation-interval capture with CSV, percentile and 1% low statistics.
- A repeatable 12-case, two-version randomizer generation/logic-parity matrix.

## Checks completed

| Check | Result |
| --- | --- |
| Windows runtime build | Passed with the MSYS2 Clang/Vulkan toolchain |
| Photo state/framing/color harness | Passed, including aspect crop, bounded input, neutral/reset behavior and freeze/HUD logic |
| Mod store harness | Passed, including conflict details, preset round-trip, empty selection, missing/version-mismatched packs and failure without changing selection |
| Workshop/frame-statistics harness | Passed: rule transitions, malformed presets, bounded source-preserving snapshots, route round-trip/rejection and known percentile/1% low results |
| UI localization | 764 catalog entries, stable IDs/live language changes and Simplified/Traditional Chinese glyph coverage passed |
| Save mount lifecycle | 50 concurrent transactions plus failure, nesting and reboot-reset cases passed |
| Save backups | Round-trip, retention, deduplication, corruption/traversal/limit checks and pre-restore protection passed |
| v1.0.0 photo gameplay | Two identical frozen 720×720 square PNGs; recipe files and normal resume verified; image inspected |
| v1.1.0 photo gameplay | Two identical frozen 1280×536 cinema PNGs; recipe files and normal resume verified; image inspected |
| v1.0.0 challenge timeout | 30-second attempt, 258 route samples (255 valid positions), one benchmark export; source-save hashes unchanged |
| v1.1.0 challenge timeout | 30-second attempt, 265 route samples (262 valid positions), one benchmark export; source-save hashes unchanged |
| v1.1.0 challenge success | Opening-house destination recognized immediately, successful result exported; source-save hashes unchanged |
| v1.0.0 movement/replay | 90-second attempt, 818 samples, 814 valid positions and 16 distinct rounded X/Z positions; Chinese route viewer screenshot inspected; source-save hashes unchanged |
| Chinese launcher controls | Save preset, arm isolated snapshot and disarm exercised manually; persisted preset and cancelled result inspected |

Native gameplay checks use independent output directories and copies of compatible saves. The immediate-success test verifies goal detection on the starting level, not travel to that destination. The longer route check uses scripted controller input in the opening house. Performance exports were exercised while other tests could be running, so their FPS numbers are not representative benchmarks.

## Randomizer matrix

Fresh seeds were generated for every row below in both v1.0.0 and v1.1.0. All 24 generations passed marker/output/settings checks. Within each row, the two versions' `logic.json` and `spheres.json` were semantically identical.

| Group | Options exercised |
| --- | --- |
| Default | Default settings |
| Keys | All dungeon items in keysanity |
| Shops | Random prices and Trendy Game checks |
| Digs | Digsanity |
| Souls | Boss and mini-boss souls |
| Mini-bosses | Enemy randomization including mini-bosses |
| Entrances | Tier 2 entrances with shuffled dungeons |
| Decoupled entrances | Tier 2 decoupled entrances with shuffled dungeons |
| Coherent rooms | Coherent room shuffle with shuffled dungeons |
| Chaos rooms | Chaos room shuffle |
| Madness rooms | Madness room shuffle |
| Combined | Keys, shops, digs, souls, enemy mini-bosses, hookshot-anything and shuffled dungeons |

The matrix records `gameplay=not_run` for each generated seed. Logic equality and successful generation do not prove all runtime interactions, combat encounters or a complete playthrough. v1.1.0 randomizer support remains experimental.

## Remaining limits

Native Metal/macOS and Linux gameplay need hardware validation. Photos still crop the rendered image; free 3D camera, true depth of field and supersampling are not added. Resource packs remain replacement-file packs without a universal per-mod settings or preview format.

The challenge editor defines timed destination goals, not arbitrary new levels or scripted encounters. Route playback visualizes observations and never drives the game. High-refresh simulation, deterministic replay and real-time co-op remain research items described in [the workshop guide](workshop.md). Combat/damage edge cases, every loading transition and long playthroughs are not certified by these opening-room checks.

Game resources, generated game code, libraries containing game code, saves, keys, native screenshots and generated seeds are excluded from the commit. The added example preset and CI fixtures are synthetic.
