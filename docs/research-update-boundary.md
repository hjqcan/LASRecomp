# Update-dispatch input experiments

The [initial research captures](research-findings-2026-10-17.md) showed that startup HID ordinals and presentation counts did not provide a repeatable starting point. This extension observes a more specific LAS callback and can record input once per complete callback after an explicit start gate. It remains a developer experiment, disabled in ordinary play.

The [native validation record](research-update-validation-2026-10-17.md) contains the dual-version movement comparisons, clock observations and one excluded GPU failure.

## Observed boundary

Four callback entries are recognized in the two supported LAS executables. Their canonical addresses live in the shared address table; runtime initialization also checks each entry's first four instructions and resolves the original generated function. A mismatch stops the experiment before guest threads start.

| Canonical entry | Probe label | Observed purpose |
| --- | --- | --- |
| `main+0x17e30` | `update_dispatch` | Updates a counter and delta field, then dispatches a callback group |
| `main+0x15d80` | `controller_update` | Reads the configured controllers, including the primary FullKey state |
| `main+0x15730` | `task_callback` | A task-related callback in the group; its complete ownership is not established |
| `main+0xc910` | `frame_timer` | Existing timer callback containing the swap-interval observation |

The research wrapper calls each original function exactly once. It does not replace its implementation, change CPU registers or write the guest clock/state fields. The common dispatch cache selects these wrappers only for an armed, identity-checked LAS experiment. Generated game code is unchanged; adding shared addresses changes its build identity, so existing libraries need a normal rebuild/relink.

Before/after records include guest thread, callback object/caller, update counter, dispatcher delta, two callback context flags, HID and presentation counts, and the existing player/save-region observations. Pairing uses thread and callback identity. A final screenshot exit can leave an unfinished callback; the analyzer reports and excludes unpaired events.

A 1.0.0 opening-house observation contained 3,257 complete gameplay dispatches. Every one advanced the independent counter once and enclosed one primary HID poll. The delta field was consistently `0.0166666675`; the same span contained about 2,714 presentations. Player position changed within 139 complete dispatcher spans. These measurements establish a useful boundary in this scene, not a universal claim that every game subsystem or asynchronous job finishes at its return.

## Start gate and input epochs

`--boundary update` enables the wrappers and waits for an observed starting condition:

1. The selected level must be active. Once it appears, the bootstrap controller input is suppressed.
2. Valid player position observations must persist for at least `--warmup` updates, default 240. Invalid observations, another level or the observed blocking flags reset this count.
3. Position and health must also remain unchanged for `--stable` consecutive updates, default 60. The first qualifying callback entry becomes update zero.
4. Each captured update latches input at its first primary-controller poll. Further polls in that update receive the same value without consuming more tape records. The state observation is taken after the original update callback returns.

The gate does not freeze the game, restore process memory, dismiss dialogue or guarantee that all loading/animation work has finished. Its initial observable state is exported so comparisons can check whether the selected starting condition actually matched. It is not a full-state synchronization barrier.

The harness drives capture with a bounded opening-house scenario: fifteen A pulses through update 847, then four 25-update directional holds starting at updates 1,200, 1,250, 1,300 and 1,350. The default length is 2,400 updates. A shorter requested capture may end before movement; inspect the images and the separate `scenario_movement` report. It counts available position changes during each directional hold. Dialogue can ignore movement even when input delivery is correct; capture success does not imply movement coverage. The bootstrap script is limited to reaching gameplay, and the same keyboard automation/settings are used on both runs.

## Commands

```sh
python tools/research_run.py --exe /path/lasrecomp --game /path/game \
  --library /path/lasgame --save-copy /path/save_0 --output /new/update-record \
  --boundary update --updates 2400 --present fifo

python tools/research_run.py --exe /path/lasrecomp --game /path/game \
  --library /path/lasgame --save-copy /path/save_0 --output /new/update-replay \
  --boundary update --updates 2400 --present fifo --replay /new/update-record

python tools/research_analyze.py /new/update-record /new/update-replay \
  --output /new/update-comparison.json
```

The runtime/library hashes, executable, source save, settings, scenario revision and gate configuration must match. Repeat for each game version using that version's library and save. `--start-level`, `--warmup` and `--stable` configure the observed gate; their limits are validated before startup.

Update-mode runs stop after the exact requested update count, then use the next presentation for the final screenshot/export. `--frames` remains a fallback endpoint if the gate never becomes ready. Such an early stop fails the capture checks. The harness requires a complete source tape of exactly the requested length, successful completion and no integrity error. Nested update callbacks, thread changes, a counter advance other than one, missing input or a poll outside the captured callback fail the experiment. After completion or an integrity failure, primary input is neutral.

For clock research, keep the actual presentation mode and pacing policy explicit. `--pace on|off` controls the existing Vulkan pacing heuristic; `--present immediate` can avoid swapchain FIFO waiting when supported. These switches are diagnostic controls and do not implement correct high-refresh gameplay. Report other active graphics workloads and repeat equivalent segments before drawing performance conclusions.

## Artifacts and compatibility

All output remains under the isolated Workshop run:

| Artifact | Meaning |
| --- | --- |
| `inputs.lastape` | `LASINPUT2`, executable identity, `boundary=las_update_dispatch_v1`, and contiguous `update,buttons,lx,ly,rx,ry` rows |
| `updates.csv` | One post-callback observation per captured update, including before/after counters, delta and HID poll count |
| `start-state.ini` | Observable state at the qualifying callback entry, before its first captured input |
| `update-summary.ini` | Gate counts, exact completion, source consumption and integrity diagnostics |
| `phases.csv` | Bounded before/after observations of all four callbacks |
| `observations.csv` | The original per-HID observations, including bootstrap/warmup; distinct from `updates.csv` |

The original `LASINPUT1` HID format remains supported in HID mode. Update mode and HID mode reject each other's tapes. Parsing retains the executable, bounds, ordinal, linked-path and transactional checks. There are at most 12,000 captured updates and 120,000 phase records; truncation is explicit. A crash or forced termination can still lose buffered observations.

The analyzer compares the starting subset, exact input prefixes, every post-update observation and available player positions. It reports dispatcher frequency separately from presentation frequency and measures path length/host duration during held left-stick segments. It retains the raw digest's limitations. Even a complete match only covers the observed subset: NPC state, RNG, job ordering, guest clocks and other input consumers are not fully controlled. `deterministic_replay` remains `not_established`; this does not enable network co-op.

## Checks

`tools/test_research.py` covers both tape boundaries, the gate, instruction rejection, input latching, exact completion, neutral input and integrity failures. `tools/test_research_analyze.py` covers thread-aware callback pairing, unpaired exits and update timing as well as the earlier camera/state analysis. The existing address and Workshop checks remain required. Native evidence is recorded separately; synthetic CI does not certify native Metal rendering or a complete playthrough.
