# Timing, HID replay and camera findings — 2026-10-17

This iteration adds repeatable, isolated research probes and measures their limits. It does not ship 120 Hz gameplay, deterministic simulation replay, a frozen free camera or co-op. The probes and commands are described in [research-capture.md](research-capture.md).

## Setup and evidence

Eight sequential native runs used Windows, Vulkan, an Intel Arc Pro 140T GPU, a 1280×720 window, Vanilla, compatible version-specific opening saves and a 5,700-presentation capture boundary. The source save and the harness's copied source remained byte-identical in every run; the game wrote to a separate Workshop snapshot. There were no input/camera capacity truncations. Screenshots and logs confirmed the opening house and the exit dialogue. This is a short scene test, not a playthrough or randomizer-parity test.

Each replay reused the exact runtime/library SHA-256, executable identity, source-save hashes and settings of its version's recording. Local artifacts contain `fixture.json`, logs, observations, camera matrices, final HID tapes, screenshots and comparison JSON. Game files and saves are not committed. The capture runtime SHA-256 was `c8b1e4d5861cea58e75ad2c07fc5017d6e25afe79ac8b4e97118aacaebb9d9c4`; a final rebuild followed comment/header cleanup. No generated-game-library ABI change was needed.

## Clock and presentation observations

Rates below use the interval between the first and last gameplay HID observations. FPS means the runtime presentation-counter delta divided by elapsed host time; it is not display scanout or simulation speed. The actual Vulkan swapchain mode was checked in each log.

| Version / run | Actual mode | Requested vsync Hz | Observed vsync Hz | Counter FPS | Gameplay seconds | Gameplay HID polls |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1.0.0 record | FIFO | 60 | 59.998 | 59.016 | 77.386 | 4,572 |
| 1.0.0 replay | FIFO | 60 | 59.998 | 59.661 | 77.253 | 4,614 |
| 1.0.0 replay | FIFO | 30 | 30.000 | 28.036 | 164.468 | 4,930 |
| 1.0.0 replay | FIFO | 120 | 120.002 | 60.014 | 76.648 | 4,661 |
| 1.0.0 replay | immediate | 60 | 59.999 | 52.121 | 90.385 | 5,271 |
| 1.0.0 replay | immediate | 120 | 119.996 | 59.758 | 77.078 | 7,700 |
| 1.1.0 record | FIFO | 60 | 59.999 | 51.468 | 89.085 | 5,212 |
| 1.1.0 replay | FIFO | 60 | 59.998 | 55.170 | 80.153 | 4,705 |

The initial mailbox request fell back to FIFO. Its approximately 60 FPS at 120 Hz therefore cannot establish a game-internal cap. The added immediate-mode control also did not sustain 120 FPS: its two-second log windows varied, including a 91.5 FPS window fully inside the house. This rules out interpreting the average as proof of a hard 60 FPS ceiling. GPU/host work, the existing pacing heuristic, presentation and game timing remain coupled.

The existing Vulkan `vk_pace_frame` heuristic is still enabled and evaluates busy frames against a 16.2 ms threshold. The 120 Hz run logged transitions into its slow-frame behavior. The frame-timer hook reported original intervals 1 and 2, and its call-count delta equalled the presentation-count delta in all eight gameplay spans. That association does not establish that this hook supplies the world-update delta. No conclusion about correct 120 Hz movement, animations, physics or audio follows from these measurements.

HID polling is also distinct from presentation: the immediate 120 Hz run observed 7,700 gameplay polls over 4,606 presentations. An input tape indexed by HID ordinal must not be labeled a simulation-tick recording. A subsequent experiment needs an explicit pacing control and an independently verified update boundary, with repeated trials and equivalent scene segments before any performance comparison.

## Replay divergence on both versions

All consumed source-tape prefixes matched exactly, including all 5,707 recorded inputs in the 1.0.0 replays. The first 1.0.0 60 Hz replay then produced one extra neutral EOF sample. Matching inputs did not produce matching observations.

| Comparison | First mismatch at absolute HID poll (zero-based) | First position difference after aligning gameplay entry (zero-based) | Initial distance | Maximum compared distance |
| --- | ---: | ---: | ---: | ---: |
| 1.0.0 FIFO 60 Hz record/replay | 1,094 | 42 | 0.0125 | 3.0750 |
| 1.1.0 FIFO 60 Hz record/replay | 1,388 | 41 | 0.0125 | 1.7250 |

The initial mismatch is entry into gameplay at different HID ordinals. Aligning the first gameplay observations helps diagnose this but does not align an actual simulation tick or the input sequence relative to that tick. A raw save-region digest diverged one poll before the first available-position difference in each pair. Because that digest includes non-semantic bytes, the position comparison is reported independently.

The 1.1.0 recording contained 6,623 inputs; its replay captured 6,093 before reaching the frame-based endpoint. The compared 6,093-input prefix matched, but the remaining 530 inputs were not replayed in that run. This is an early endpoint, not a complete-tape validation.

All runs accumulated approximately 5.78225 X/Z path units and ended at the house-exit dialogue. Equal total distance and similar final pictures do not establish matching histories. Nor do these tests prove that the game itself cannot be deterministic: the harness has not controlled asynchronous loading, guest time, RNG initialization or host input consumers outside the primary FullKey boundary. Every result remains `deterministic_replay: not_established`.

The next replay milestone is a reproducible post-load barrier and a verified game-update hook on both executable versions. Record semantic state and relevant clocks/RNG at that boundary, then rerun the same fixture and report the first mismatch before attempting synchronization or networking.

## Camera matrix contract

Each run captured 4,383–4,672 first-recognized scene UBOs. Across both versions, the three row-major matrices consistently satisfy `VP = P × V`:

- Maximum relative product error: approximately `8.90e-8`.
- Reversing multiplication to `V × P`: maximum relative error approximately `1.0013`.
- View rotation orthogonality error: at most approximately `1.99e-7`.
- View rotation determinant: approximately `0.99999980` to `1.00000018`.

This is useful evidence for camera transformation work in this scene. It does not validate shadow/culling cameras, inverse matrices, reflections, other scenes or every render pass. Photo mode currently freezes by repeatedly presenting the last rendered texture. Changing a camera matrix cannot redraw that texture. A frozen free camera first needs resource-stable rendering replay with explicit ownership of command memory, UBOs, render targets and synchronization side effects, followed by culling/lighting/post-processing validation.

## Regression checks and scope

- The Windows runtime rebuilt successfully with the new probe modules.
- Native synthetic tests passed for tape format/identity/bounds, transactional parsing, available symlink rejection, capture gates, unsupported titles/rates, default-clock restoration, recording, playback and neutral EOF.
- Four Python analysis tests passed for multiplication order, timing arithmetic, count/mismatch reporting and available-position comparisons.
- Existing Workshop checks passed. Research checks are added to the Linux/macOS/Windows Workshop CI matrix.
- The default-play photo regression passed with research disabled: two identical 720×720 frozen PNGs, settings recipes and successful resume. The images were visually checked. This remains an opening-house check, separate from the research measurements.

The normal vsync period remains 16,666,667 ns. LAS probes require a recognized LAS executable and an isolated Workshop save. Other titles are rejected. Native Linux/macOS gameplay and Metal rendering remain unverified in this iteration; CI's synthetic checks do not replace those runs.
