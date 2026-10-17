# Fourth research round: startup seed, save bytes and loading

The [semantic observations](research-semantic-observations.md) established replay divergence despite matching input. This round separates the startup seed from warmup consumption and observes the bytes and I/O activity around scene transitions. All experiments remain restricted to verified LAS executables and isolated Workshop saves. This is not a supported deterministic replay or multiplayer mode.

## Verified startup path

Both supported executables call `time(NULL)` at canonical `main+0x948c`, pass the return value to the RNG initializer at `main+0xa3f0`, and store its four-word state at `main+0x1c341f8`. The updated executable's call instruction differs because the main PLT moved; the guard checks the appropriate version. The complete 168-byte initializer and the SDK's complete 60-byte `time` function are checked as well as the existing RNG address/load/transition guard.

The SDK path is `time` at `sdk+0x4d0780` → `clock_gettime` → `__nnmusl_ClockGetTime` → `nn::os::GetSystemTick` / `ConvertToTimeSpan`. In this runtime, the system tick comes from the monotonic host timer. This specific path is distinct from the observed `StandardUserSystemClock` HLE calendar-time return. In the first baseline pair, the actual startup values were 109,399 and 109,510, not calendar timestamps. This is a local executable/runtime finding, not a general statement about the POSIX `time` API.

The initializer XORs each of four constants with the low 32 bits of the seed, applies right-shift XORs by 21 and 4, and multiplies by `0x4f6cdd1d` modulo 2^32. The analyzer checks its model against the recorded projection and searches the observed RNG sequence. In the first baseline pair, the first dispatcher observation was seven advances from initialization, and each capture gate was 508 advances from initialization. Thus these two runs had different seed inputs with the same inferred pre-gate consumption.

## Controlled experiment

With `--semantics`, the SDK clock function is called normally and its actual result is recorded. The optional `--startup-seed UINT32` control replaces only the return register at the validated, null-argument caller that immediately feeds the RNG initializer. Other `time` callers, kernel entropy, the user calendar clock and file loading run normally. The game itself still runs its initializer; no RNG state is restored at the capture gate, and already-created objects are not reset. This is separate from LAS-Randomizer seed generation.

```sh
python tools/research_run.py --exe /path/lasrecomp --game /path/game \
  --library /path/lasgame --save-copy /path/save_0 --output /new/fixed-record \
  --boundary update --semantics --scenario house-exit --startup-seed 123456789 \
  --updates 4800 --frames 9000 --present fifo

python tools/research_run.py --exe /path/lasrecomp --game /path/game \
  --library /path/lasgame --save-copy /path/save_0 --output /new/fixed-replay \
  --boundary update --semantics --scenario house-exit --startup-seed 123456789 \
  --updates 4800 --frames 9000 --present fifo --replay /new/fixed-record

python tools/research_analyze.py /new/fixed-record /new/fixed-replay \
  --output /new/fixed-comparison.json
```

For the baseline pair, omit `--startup-seed` in both commands and use new directories. The harness fingerprints the control value, probe revision and all existing fixture identities. Replay rejects a different seed setting. A semantic capture requires exactly one recognized startup call and no seed-event truncation; capture success still does not mean state agreement.

## Added evidence

| Artifact / fields | Purpose |
| --- | --- |
| `seed-summary.ini`, `seed-events.csv` | Original/effective clock return, startup-caller flag, guest caller/thread, relative host time and projected initializer state; at most 128 clock observations |
| RNG columns in `phases.csv` | Observe the global RNG before the start gate, as well as during capture |
| `save-windows.csv` | Initial gate plus each post-update copy of the existing 601-byte window `save+0x5388..0x55e0`; availability is explicit |
| `io_started`, `io_completed`, `io_bytes`, `io_failed` | Coherent counters around the actual RomFS IStorage read operation, sampled at callback boundaries |

The raw window is copied once before hashing so the exported bytes and their recorded digest describe the same copy. The analyzer validates every available window against its digest before comparing bytes. This is not an atomic snapshot of every guest field. Raw captures and save bytes remain local and are not committed. Existing capture limits remain; the expanded phase CSV reader accepts at most 120,000 rows and 64 MiB, and the save-window reader accepts at most 12,001 rows and 32 MiB.

The seed analysis searches up to 65,536 RNG advances from initialization to the first observed callback and the start gate. A failed search is reported as unknown rather than evidence of zero consumption. The ordinary per-dispatch search remains bounded to 4,096 advances.

The loading report identifies level-name changes and available player-position observations alongside I/O counters. Zero in-flight reads does not prove that decompression, actor creation, GPU uploads or other asynchronous tasks finished. The existing player locator also deliberately waits 30 presented frames before rescanning after a level change; the reported first available position includes this observer delay and must not be interpreted as a load-completion timestamp.

## Findings

The 1.0.0 baseline's first differing saved bytes were at `save+0x53a0`, at captured update 2,325. Interpreted as an unsigned 64-bit value, that field's observed increments were only zero or 16,666 in both runs. Its full game meaning has not been established, and the bytes did not match the recorded calendar-clock returns. The report therefore identifies the byte region and the measured changes without naming it a wall-clock timestamp.

With startup seed `123456789`, both 1.0.0 runs had the same modeled initial RNG state and the same inferred 508 advances to the gate. They matched for 3,022 consecutive post-update RNG states before diverging at update 3,022. The older player/save subset first differed at update 3,016, when one run reported `Field` and the other still reported the house. Raw byte differences at `0x54c0` and `0x5500` contained the level/locator strings. The corresponding field-entry indices were 3,016 and 3,017.

At those entries, both runs had completed 791 observed RomFS reads totaling 898,372,784 bytes, with zero outstanding reads and zero observed read failures. They still reached the scene at different update indices. Later NPC positions also diverged. Fixing this one startup input improves the measured prefix in this pair; it does not establish complete determinism or make I/O-idle a sufficient synchronization barrier.

The fixed-seed 1.1.0 pair diverged earlier, at update 2,323 while both runs still reported the house. The pre-update RNG states matched; the bounded model inferred four advances during the recorded update and five during replay. Player positions and saved bytes still matched at that point, as did I/O counters (652 completed reads, 610,877,326 bytes, no outstanding reads or failures). The first saved-byte difference followed at update 2,325, again at `0x53a0`. This is direct evidence of differing consumption within a dispatch, but does not yet identify the extra caller or its trigger. These two runs subsequently entered Field and returned to the house at identical captured indices despite differing RNG and NPC state.

All eight Windows runs completed exactly 4,800 captured updates. Each pair had 4,800 identical recorded/replayed input samples, intact source saves, one recognized startup call, and 4,801 available save windows whose digests validated. The scripted house and field movements and round trip completed. Every run's inferred first-observation and gate consumption was seven and 508 advances, respectively. Baseline seeds were the actual clock returns; fixed pairs used `123456789`.

Indices below are zero-based captured updates. Entry pairs are record / replay; matching counts compare the two runs within one condition.

| Observation | 1.0.0 baseline | 1.0.0 fixed | 1.1.0 baseline | 1.1.0 fixed |
| --- | --- | --- | --- | --- |
| Actual startup clock returns | 109,399 / 109,510 | 109,663 / 109,772 | 109,916 / 110,026 | 110,140 / 110,251 |
| Consecutive matching post-update RNG states | 0 | 3,022 | 0 | 2,323 |
| Total matching RNG states / 4,800 | 0 | 3,023 | 0 | 2,324 |
| First differing save-window update | 2,325 | 3,016 | 2,310 | 2,325 |
| Matching save windows / 4,801, including gate | 2,326 | 3,794 | 2,311 | 2,327 |
| Field entry | 3,035 / 3,016 | 3,016 / 3,017 | 3,018 / 3,019 | 3,018 / 3,018 |
| House return | 4,023 / 4,023 | 4,023 / 4,022 | 4,024 / 4,022 | 4,022 / 4,022 |
| Largest selected NPC component-position difference | 7.856014 | 6.843062 | 6.286901 | 4.600739 |

Position observations need care around transitions. The 1.1.0 baseline's raw maximum player-position difference was 132.541760 at update 3,959: replay was already blocked and exposed `(0,0,0)` while the record still exposed its Field coordinates. This value is a transition-observation mismatch, not established evidence that active Link positions moved that far apart. The analyzer retains the raw measurement and states this availability limit; filtering it away would hide useful timing evidence.

## Reproduction and limits

The Windows runtime was built from `0dd1b61` plus this round's changes. Both local game libraries were relinked for the updated runtime identity; generated game code and its imported API were unchanged. The frozen artifact hashes used for the experiment are:

| Artifact | SHA-256 |
| --- | --- |
| `lasrecomp.exe` | `1b3a4a792092d5e438cb6ecce8be3263cd770335d83a4bab4ca288eaf866be37` |
| 1.0.0 `lasgame.dll` | `b83d9e101abfea2a84ef0990da119664d1030ddcc910eaaf9c8223520360935e` |
| 1.1.0 `lasgame.dll` | `fcfbac0359d4a52fb600a2ca3c6a18108dd29fa2faed232feadda288b7f0d6d4` |

Local synthetic checks passed five research executables (input tape, runtime, update boundary, actors and startup seed), 16 Python analysis tests, the Workshop tests, 139 shared addresses, 46 patch fixtures and 12 hooks. Startup-seed tests include disabled operation, unrelated callers, uint32 bounds, initialization, export limits and version-specific caller guards.

With research disabled, the 1.0.0 photo smoke check passed using the same runtime: two byte-identical 720x720 PNG exports, settings recipes and a successful resume. The photo, resumed image and both versions' final gameplay captures were visually inspected. This covers the opening room; native Linux/macOS play, combat and longer-session behavior remain unverified.

Each condition has one record/replay pair per game version. This is a small diagnostic sample, with other software running on the host, not a performance benchmark or statistical estimate. The next useful probes are RNG caller identities around the first unequal consumption, the writer of `save+0x53a0`, and scene-load task completion/activation order. Those should be observed before introducing a synchronization barrier or changing the normal scheduler. The current evidence does not identify every RNG, establish complete state equality, or certify deterministic replay, high-refresh simulation or co-op.
