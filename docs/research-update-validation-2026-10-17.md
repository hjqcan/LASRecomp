# Update-dispatch validation — 2026-10-17

The guarded [update-dispatch experiment](research-update-boundary.md) reproduced the observed opening-house state through actual movement on both supported LAS versions. Each version's recording and replay completed exactly 2,400 input epochs; all 2,400 sampled game-state comparisons matched within that pair. Equality covers observation/gameplay flags, level, health, available player position and the raw save-region digest; timing is reported separately. Maximum compared position difference was zero.

This is a match of the measured subset in one scene. Complete simulation determinism, RNG/other actors, cross-scene replay, combat, native Metal rendering and networking remain unverified. Every machine-readable result retains `deterministic_replay: not_established`.

## Conditions and coverage

Runs used Windows, Vulkan, an Intel Arc Pro 140T, a 1280×720 window, Vanilla, version-compatible isolated save copies and the `house_v5` driver. Other development game processes were open. Rates below are observations under concurrent graphics workloads, not an isolated performance benchmark.

Each comparison used matching runtime/library hashes, executable identity, source save, settings, script revision and gate parameters. The gate required 240 valid updates in `MarinTarinHouse` and 60 stable position/health observations. The starting position, health, level and raw region digest matched within each pair. Original and copied source saves remained byte-identical in every successful final run, and there was no input/camera/phase capacity truncation.

The harness separately verified movement during four 25-update stick holds. Each direction had 25 available position pairs and 25 changing samples in all six successful final runs. The path lengths during those holds were approximately 1.87500, 1.87500, 1.86913 and 1.49207 world units; total X/Z travel was 7.11120. Screenshots confirmed that the character was out of dialogue. This separate check matters because earlier script calibration could deliver valid input while a dialogue ignored the directional holds.

The initial HID-poll alignment still reports differences during loading and the opening animation. Those observations remain available. The match is at the explicitly gated update boundary; it is not a claim that the entire boot sequence matched.

## Observed rates

These paired measurements preceded the upstream integration check described below; their exact runtime identities are retained in the reproduction section.

Intervals run from the first to last captured post-update sample, so 2,400 samples span 2,399 update intervals. Presentation FPS uses the runtime's presentation counter, not display scanout. The dispatcher delta stayed at `0.0166666675` in every captured update below.

| Version / run | Actual mode | Vulkan pacing heuristic | Emulated vsync Hz | Update Hz | Counter FPS | Host seconds |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| 1.0.0 record | FIFO | on | 59.892 | 55.907 | 47.937 | 42.910 |
| 1.0.0 replay | FIFO | on | 60.015 | 57.752 | 52.336 | 41.540 |
| 1.1.0 record, retry | FIFO | on | 60.000 | 59.701 | 56.242 | 40.183 |
| 1.1.0 replay, retry | FIFO | on | 60.000 | 60.000 | 59.950 | 39.984 |
| 1.0.0 replay, 60 Hz control | immediate | off | 60.000 | 48.710 | 41.502 | 49.250 |
| 1.0.0 replay, 120 Hz control | immediate | off | 119.993 | 66.251 | 41.701 | 36.211 |

Both immediate-mode controls also matched all 2,400 sampled game-state comparisons and inputs. Their host timing differed: for example, the same 25-update downward movement covered 1.875 units in approximately 0.50034 seconds at 60 Hz and 0.36927 seconds at 120 Hz. The observed movement rates were 3.747 and 5.078 path units per host second. Render presentation stayed near 42 FPS in both controls.

These results separate update count from presentation count and show why changing the emulated-vsync clock cannot be treated as a correct 120 FPS feature. The observed dispatcher timestep remained 1/60 second while its host frequency varied. Concurrent workloads prevent attributing every rate difference to the selected clock alone. A supported high-refresh mode still needs controlled, repeated measurements and a validated separation of simulation time from rendering.

## Failed run and remaining stability limit

The first final 1.1.0 movement capture failed with Vulkan result `-4` (`VK_ERROR_DEVICE_LOST`). Its log contained long host/vsync stalls of roughly 68–123 seconds before the failure. Source saves remained unchanged. Buffered research data was not exported, and this run is excluded from the successful comparisons.

A fresh 1.1.0 recording and replay then completed the full scenario and matched every captured update. The device-loss cause was not established or fixed by this change; the concurrent graphics workload is context, not proof of causation. This result does not certify GPU stability or long play sessions.

## Reproduction and checks

The [experiment guide](research-update-boundary.md#commands) provides the recording/replay commands. Run native cases sequentially and keep the same fixture identities. For the two controls, replay the 1.0.0 recording with `--present immediate --pace off` and `--hz 60` / `--hz 120`. Use a fresh output directory each time and compare both `updates.csv` and movement coverage.

Local evidence includes fixture hashes, runtime logs, start states, input tapes, update/HID/phase observations, screenshots and comparison JSON. The 1.0.0 pairs used runtime SHA-256 `3aaffeb93caaad30c55dfee410a4790efe0a5e296ec2d848fe4e3998848a2936`; the 1.1.0 retry used `23a64d9523a78320e06e1739e0c00492bdc90e4233386ac517c0d6792d9f6297`, which additionally rejects an oversized implicit update count. All native runs passed an explicit count of 2,400. Each pair's runtime was identical. Game assets, generated game code, saves and captures are not committed.

The Windows runtime and both game-library identity entries rebuilt successfully. Native synthetic tests passed for both tape formats, instruction guards, start gates, latching, exact completion, neutral input, callback integrity and bounds. Nine Python analysis tests passed, including rejection of static dialogue as movement coverage. The 134 shared addresses, 46 patch outputs, 12 hooks and existing Workshop checks passed. These synthetic checks run in the existing Linux/macOS/Windows Workshop CI jobs; they do not replace native gameplay validation.

With research disabled, the final default-play photo check also passed: two identical 720×720 PNG captures, recipe files and successful resume. The frozen and resumed images were visually checked.

## Upstream integration check

Before publication, this change was rebased onto enhanced merge `1a076d0`, which incorporates upstream `beta_5` through `d561b0d`, including Vulkan memory-budget and rendering fixes. The Windows runtime rebuilt successfully with SHA-256 `59c5d94a4e083d65837b80909c477bcf45fef6a9d7f4658e0f5056653b82f434`.

A fresh 1.1.0 recording/replay pair on that build again completed all 2,400 updates, exercised all four directional holds and matched all sampled game-state fields, with zero position difference and matching starting subsets. Source saves remained unchanged. A fresh 1.0.0 photo check with research disabled also passed, including identical 720×720 exports, recipes and resume; the images were visually checked. No device loss occurred in these integration checks, which does not establish the cause or resolution of the earlier failure.

Next work should expand semantic observations to RNG, other actors and asynchronous tasks, add repeatable scene transitions/combat, and test the same input epochs on native macOS/Linux hardware. The current measured match is a useful prerequisite for that work, not a completed deterministic replay or co-op implementation.
