# RNG, actor and external-return observations

The [update-boundary experiment](research-update-boundary.md) compared a player/save subset. A new read-only probe exposes differences that subset could miss. It is a developer tool, disabled in ordinary play, and does not restore state, replace random values or implement network play.

The [fourth research round](research-fourth-round.md) adds startup-source tracing, byte-level save observations and RomFS read counters, with a separate opt-in control for the one startup seed input. Default semantic observation remains read-only.

## Evidence and scope

Local inspection of both recognized executables found a four-word global state at `main+0x1c341f8`. Instructions at `main+0x2a5f8` calculate that address, read its words and apply the xorshift128 shifts 11, 8 and 19. The 40-byte instruction span and the four initial data words are identical in 1.0.0 and 1.1.0. Startup code calls imported `time(NULL)` and passes its result to the state initializer at `main+0xa3f0`. This identifies one RNG state; it does not establish that every game subsystem uses it or that every subsequent difference is caused by the startup seed.

The first native 1.0.0 comparison captured 2,400 updates with the existing four-direction opening-house script. All earlier player/save comparisons still matched, but the RNG states differed at the start gate and at every compared post-update sample. Inferred advance counts also differed: the recording had 2,277 two-step spans and 113 three-step spans; replay had 2,276 and 114 respectively. Each had two four-step and eight five-step spans. All 2,399 intervals between dispatches preserved this observed RNG state. These are counts inferred from the sampled state, not observed function-call counts.

The selected external observations contained four entropy returns and two user-clock returns per run, with different values across runs. `GetInfo(11)` is the process entropy query, consistent with [libnx's SVC definitions](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/kernel/svc.h). No causal link between those entropy returns and the identified game RNG is assumed. The user-clock observations occurred after the start gate; they are not a record of the startup `time(NULL)` result.

Read-only inspection of the opening-house actor registry found placed NPC entries for Marin and Tarin, with stable layout keys and named position-bearing components. The collector now follows that existing registry layout, which the runtime also uses for companion handling. It selects names beginning with `Npc` or `Enemy`, validates the name's hexadecimal suffix against the registry key, and samples the component referenced at actor `+0x80`, with coordinates at component `+0x80`. These are component coordinates, not a complete actor state or a validated representation of every actor class.

## Running the probe

```sh
python tools/research_run.py --exe /path/lasrecomp --game /path/game \
  --library /path/lasgame --save-copy /path/save_0 --output /new/semantic-record \
  --boundary update --semantics --updates 4800 --scenario house-exit --present fifo --frames 9000

python tools/research_run.py --exe /path/lasrecomp --game /path/game \
  --library /path/lasgame --save-copy /path/save_0 --output /new/semantic-replay \
  --boundary update --semantics --updates 4800 --scenario house-exit --present fifo --frames 9000 \
  --replay /new/semantic-record

python tools/research_analyze.py /new/semantic-record /new/semantic-replay \
  --output /new/semantic-comparison.json
```

Use the same runtime, game library, executable, source save, settings, gate and scenario revision within each pair. Existing libraries need their normal identity rebuild/relink after the shared address-table addition. The harness includes the semantic probe revision in its fixture fingerprint.

`--scenario house` retains the earlier 2,400-update script. `house-exit` (`house_exit_v2`) first approaches Tarin, initiates the shield conversation and uses B pulses to advance it without initiating another conversation. It then walks to the exit, with four 40-update field holds starting at 3,600, 3,700, 3,800 and 3,900. The last hold can bring Link back into the house. A requested script is not proof of coverage: inspect `scenario_transition`, its separate field movement windows, and the screenshot. Capture completion, movement coverage and state agreement are separate results. The earlier calibration script reached the exit without the shield and was blocked by dialogue; its missing Field coverage was detected and it is excluded from transition validation.

## Data and validation

`--semantics` requires update mode, an identity-checked LAS executable and the isolated Workshop save. Initialization checks the RNG instruction span and initial data before guest startup. Every original callback and observed entropy/clock operation still runs normally; the probes only copy their results.

The added artifacts and fields are:

| Artifact | Observations |
| --- | --- |
| `start-state.ini` | Four RNG words at the qualifying start gate and an availability flag |
| `updates.csv` | Four RNG words before and after each captured update, with separate availability flags |
| `external.csv` | Selected entropy/user-clock returns, call site, guest thread, sub-index, host time and completed-update count |
| `actor-samples.csv` | Every tenth captured update: level, completeness, issue code, registry size and selected actor count |
| `actors.csv` | Complete selected rosters keyed by layout identifier, with class address, component name and available coordinates |

The actor collector accepts only pointers inside the current main guest heap. It bounds the registry walk to 2,048 entries and each selected roster to 64 actors, checks the declared count and re-reads the registry header. It sorts by stable layout key rather than allocation address or hash-map iteration order. Invalid pointers, excessive counts, cycles, duplicate selected keys, malformed names or a changed header produce an incomplete sample. Partial roster data is never exported as a complete roster. Missing or non-finite component positions remain unavailable. The interval yields at most 1,200 actor samples and 76,800 actor rows within the existing 12,000-update limit.

Actor issue codes are `0` complete, `1` unavailable/unreadable, `2` limit, `3` layout/count inconsistency and `4` changed registry header. An unavailable gameplay observation also produces an incomplete actor sample. Complete means the registry walk passed its checks, not that scene loading finished or every actor was active. Header stability does not make all actor fields atomic: other guest work may run between reads, and objects can change without changing that header.

External recording is bounded to 4,096 events; overflow is reported and fails the harness completeness check. It observes `GetInfo(11)` and the existing `StandardUserSystemClock::GetCurrentTime` HLE path only. Other clock/entropy paths are outside this probe. Raw game data, saves and captures remain local.

The analyzer reports player, RNG and actor comparisons separately and lists their observed divergences together. RNG advance inference searches at most 4,096 steps per span and rejects an all-zero state as evidence of consumption. An unexplained transition can reflect more steps, a reset, concurrent changes or a different transition; the tool does not assign a cause. Actor comparisons require complete samples and match keys, class/component identity, availability and component coordinates with a `1e-4` position tolerance. Missing coverage is never treated as equality.

The measured subset still omits AI, animation state, unregistered/dynamically created actors, other RNGs, job ordering and most clock consumers. `deterministic_replay` remains `not_established` even when an individual comparison matches.

## Native transition validation — 2026-10-17

The final `house_exit_v2` recording/replay pairs both completed on Windows/Vulkan for 1.0.0 and 1.1.0. Each of the four runs captured and consumed exactly 4,800 updates, moved in all four house windows and all four field windows, and returned to the house. Input tapes matched within each version's pair. Source saves and the source copies remained byte-identical; the game's writes stayed in the separate Workshop run saves. No capture capacity was exceeded.

The runtime SHA-256 was `53cc7b65159591d7fd672b153c8fc4211894607ea47979f116fd4f596c989116` in all four runs. Library hashes were `9cfca39a56a11d4225dfb4bc5858e106b4ed6370394cc4505338ff5d71333c28` for 1.0.0 and `86b5897400f5c3b4f671205b4f30abfe5b8653efd3f1e82a967234d4b131af6a` for 1.1.0. Fixture manifests, input tapes, logs, observations, screenshots and comparison JSON remain in the local validation outputs. An additional development game process was open; this was not an isolated performance benchmark.

All indices below are zero-based captured updates. Distances are game world units, comparing available positions in the same level; actor distances also require matching class and component identities.

| Observation | 1.0.0 record / replay | 1.1.0 record / replay |
| --- | --- | --- |
| First observed Field update | 3,026 / 3,021 | 3,020 / 3,022 |
| Return to house | 4,023 / 4,023 | 4,035 / 4,022 |
| Matching post-update RNG states | 0 / 4,800 comparisons | 0 / 4,800 comparisons |
| First raw save-region digest difference | Update 2,254 | Update 2,314 |
| First player position difference | Update 3,060 | Update 3,060 |
| Maximum player position difference | 0.756637 | 0.746246 |
| First selected actor position difference | BowWow, update 3,180 | BowWow, update 3,150 |
| Maximum selected actor position difference | Fox, 8.064351 at update 3,630 | Fox, 7.271041 at update 3,390 |

The starting player position, health and raw save-region digest matched within each pair, while the starting RNG state did not. Every within-dispatch RNG transition in these four runs was explained within the bounded search; all 4,799 between-dispatch intervals in each run preserved the observed state. This local evidence supports using the dispatcher to investigate this RNG, but does not establish ownership of every asynchronous operation.

Each run produced 480 actor samples with structurally complete registry walks. The 1.0.0 pair had 5,934 comparable component-position pairs; the 1.1.0 pair had 5,972. Unavailable component positions and different rosters were reported separately. The first player/save-subset disagreement involved only the raw digest while position still matched; its individual byte/field cause is not established by a digest. Similarly, seed initialization, RNG consumption, loading timing and actor behavior are observed differences, not a proven single causal chain.

Local validation also passed the four native research test executables, 13 Python analysis tests, 136 shared addresses, 46 patch fixtures, 12 hooks and the Workshop checks. With research disabled, the 1.0.0 photo smoke check passed: two identical 720×720 PNG exports, recipes and successful resume. The frozen/resumed images and the final gameplay images were visually inspected. Native Linux/macOS play, combat, longer sessions and complete replay determinism remain unverified.

The next experiments should isolate the startup seed source and warmup consumption, observe the relevant saved fields instead of only their digest, and trace scene-load completion independently of update count. Replacing the RNG alone would not establish that already-created actors or asynchronous work start in the same state.
