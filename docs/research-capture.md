# Isolated timing, HID replay and camera research

This is a developer experiment on `enhanced`, disabled during normal play. It does not enable high-refresh gameplay, deterministic game replay, a free camera or networking.

Measured results and the current acceptance gaps are in the [2026-10-17 findings](research-findings-2026-10-17.md).

The subsequent [update-dispatch experiment](research-update-boundary.md) adds guarded callback observations, a configurable observed start gate, latched input and exact update-count capture. The commands below continue to describe the original HID mode.

## What the probe records

The probe attaches to existing LAS runtime boundaries without changing the generated game library ABI:

- **Final FullKey HID polls:** the button mask and four stick axes actually returned to the game, after the existing host input transformations. Every primary-controller poll gets an ordinal. This ordinal is not asserted to be a simulation tick.
- **Observations at that poll:** gameplay/level/health, an available collision-component position, presentation count, host elapsed time, emulated-vsync count and the existing frame-timer hook's call count/original interval.
- **A raw region digest:** FNV-1a 64 over live-save bytes `0x5388` through `0x55e0`, inclusive. This is a change detector, not a cryptographic identity or a complete/semantic game-state hash. The region can include fields unrelated to the visible player state. The analyzer also compares positions independently of this digest.
- **Scene cameras:** the first recognized 320-byte vertex-slot-5 scene UBO per presented frame contributes its first three 4×4 matrices (view, projection and VP). Capture checks the existing affine/perspective shape and finite values. It does not identify every camera, shadow pass, culling volume or inverse matrix.

Input replay substitutes the recorded final HID values at the same boundary. At end-of-tape it returns neutral input, never the last held direction. Host-side consumers outside this boundary—such as movie input, software keyboards, companion menus and scripted warps—are not replayed by this mechanism. It deliberately remains a diagnostic input tape.

## Safety and bounds

`research_boot` requires a current isolated Workshop save and a recognized LAS executable identity. Unknown titles, missing isolation, conflicting modes and unsupported rates fail before guest threads start. This preserves the multi-game boundary: these LAS-specific probes must not be enabled merely because another title has a catalog entry.

Normal timing remains the existing 16,666,667 ns period. Only an explicitly armed research run can select 30, 60 or 120 Hz emulated vsync. Finishing capture restores the default period, including on reboot. Changing this clock is an experiment, not a supported gameplay speed or refresh-rate setting.

Limits are 120,000 HID records, 12,000 camera samples and a 16 MiB input tape. Truncation is reported in metadata. Linked/reparse input paths, invalid ordinals, unsupported button bits, out-of-range axes, embedded NULs and mismatched executable IDs are rejected. Parsing is transactional: a malformed tape never partially replaces the replay sequence. Output stays under the newly created isolated run's `research/` directory.

Capture is bounded in memory and written at normal process exit, game teardown or the explicit screenshot-exit test hook. A crash or forced termination can lose the capture. Game resources and save contents are not included in the tape; observations still contain gameplay routes and controller input, so share these artifacts intentionally.

## Repeatable experiment

Use an existing compiled runtime/library and a compatible `save_0` directory. Every command requires a new output directory and copies the source save before running. The source and copied source are hashed before/after; guest writes use a separate Workshop snapshot.

```sh
python tools/research_run.py --exe /path/lasrecomp --game /path/game \
  --library /path/lasgame --save-copy /path/save_0 --output /new/record --hz 60

python tools/research_run.py --exe /path/lasrecomp --game /path/game \
  --library /path/lasgame --save-copy /path/save_0 --output /new/replay \
  --replay /new/record --hz 60

python tools/research_analyze.py /new/record /new/replay --output /new/comparison.json
```

Replay through this harness requires matching runtime/library SHA-256 hashes, the runtime's executable Build ID, source-save file hashes and settings. The harness uses Vanilla in a fresh data root. Keep the same extracted assets and host environment; it does not hash the entire RomFS. The default 5,700-frame input script exercises the opening house and walking toward its exit. Dialogue or collision can limit motion. It is not a playthrough.

Repeat replay with `--hz 30` or `--hz 120` to investigate clock coupling. Run native experiments sequentially so they do not compete for the GPU. `--frames` changes the capture boundary, not the input tape's ordering. EOF or an early capture boundary can yield different record counts; both are visible in the report.

On Vulkan, `--present mailbox|fifo|immediate` requests a presentation mode; the report also records the actual mode from the runtime log. Unsupported requests can fall back to FIFO. Compare 60/120 Hz with the same actual mode, including an immediate-mode control when available, before attributing a frame-rate limit to the game. The request does not control Metal presentation, the desktop compositor or display scanout. Neither host GPU performance nor presentation rate establishes simulation speed.

`--pace on|off` explicitly selects the existing Vulkan frame-pacing heuristic. `--phases` adds the guarded callback observations without changing HID tape ordering. These controls are recorded in the report.

## Output and interpretation

`fixture.json` identifies the harness inputs. Inside `workshop/runs/run-…/research/`:

| File | Content |
| --- | --- |
| `inputs.lastape` | `LASINPUT1`, executable identity and contiguous `poll,buttons,lx,ly,rx,ry` records |
| `observations.csv` | Observed state and clock/counter values at HID polls |
| `camera.csv` | Presented frame, elapsed time and row-major `v0…v15,p0…p15,vp0…vp15` |
| `summary.ini` | Mode/rate, consumed input count, EOF and truncation flags, observation boundary and digest definition |

The analyzer compares input prefixes, reports the first observation mismatch, and separately aligns the first gameplay observations. It also compares available positions in the same level and tests matrix multiplication order. Its presentation rate is the frame-count difference divided by host time across the observed gameplay span; its vsync rate is measured independently. These values are not GPU time or simulation speed.

Even identical observations cannot establish complete determinism: RNG state, asynchronous events, unobserved actors, animation and many services are absent. `deterministic_replay` therefore remains `not_established`. A successful harness exit means capture/identity/isolation checks passed, not that replay matched.

## Reproducible checks

```sh
python tools/test_research.py
python tools/test_research_analyze.py
python tools/test_workshop.py
```

The native synthetic checks cover tape parsing/bounds/identity/link rejection, missing-isolation and unsupported-title gates, allowed clock selection/default restoration, capture, replay and neutral EOF. Python checks cover matrix order, mismatch/count reporting and timing arithmetic. These run alongside Workshop CI on Linux, macOS and Windows; native game validation is separate.

## Next milestones

1. Find and verify the actual game-update boundary on both LAS versions. Add a reproducible post-load barrier, then distinguish simulation-tick input from HID polling/presentation. Trace guest-facing clocks and random initialization before attempting to control them.
2. Check bounded semantic state at that boundary, with RNG and asynchronous-load ordering included. A saved game plus final HID polls is not a process snapshot. Keep an explicit divergence report as the acceptance criterion.
3. For a frozen free camera, design a resource-stable render replay. Photo mode currently holds `nvnQueuePresentTexture` and repeatedly presents the last texture; changing a matrix cannot redraw that frozen scene. Replaying guest command handles alone is unsafe because command memory, UBOs, synchronization commands and render targets have lifetimes and side effects. Validate culling, lighting/shadows and screen-space effects before exposing controls.
4. Investigate 120 Hz presentation/simulation separation only after the clocks and update boundary are known. Real-time co-op additionally requires authoritative state and ownership; no network transport or synchronization is added here.
