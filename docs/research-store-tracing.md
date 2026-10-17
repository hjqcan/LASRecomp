# Fifth research round: observed RNG write traces

This round turns the [fourth round's](research-fourth-round.md) differing inferred RNG consumption into source-level write observations. The deliverable is a bounded diagnostic tool for a fixed-seed, identical-input experiment. It does not repair determinism or change task scheduling.

## Why a separate diagnostic library

The guarded fragment at `main+0x2a5f8` is inside a larger function. Its generated body contains multiple inlined xorshift transitions. Same-module direct calls also bypass runtime dispatch. Hooking one dispatcher entry would therefore miss relevant operations.

`tools/research_instrument.py` copies an existing local generated-code tree into a **new** directory. It selects functions explicitly materializing the page containing the known global RNG, then replaces their generated `ST8/16/32/64/128` operations with helpers that perform the original write and optionally observe overlap with the 16-byte RNG state. Source files in the original tree remain untouched. Strings and comments are not rewritten. The helpers evaluate addresses and values once and preserve the value of the store expression.

The selection is deliberately reported as **partial coverage**. Pointer-only functions, runtime/HLE operations, atomic stores and inline assembly are outside this selection. The two validated local source trees each selected 395 functions and 13,901 scalar/vector store sites in 125 files. Most selected operations do not touch the RNG; the runtime address check filters them. These counts describe these generated trees, not every possible generator version.

Sites identify an original generated-C file and line plus its containing guest function. They are **not guest instruction PCs or RNG call counts**. A translated pair-store instruction can yield two observed C stores; one RNG advance can involve several writes in different orders. The manifest retains source hashes and site identities, with no generated game code included in this repository.

The CSV's `caller` field is the live guest X30/LR value at the observed store. It is not an unwound stack or proof of the containing function's caller; intervening calls can have changed that register.

The generated main table exports the manifest's SHA-256 through a separate optional symbol. Existing game-library ABI and identity checks still apply. The runtime refuses store tracing without this capability; the harness checks that the supplied manifest matches the loaded library's export. Ordinary generated libraries have no store helpers or this export.

## Reproduce

Use already-extracted game files and an existing generated tree for the same verified executable:

```sh
python tools/research_instrument.py --generated /path/normal-generated \
  --game /path/game --output /new/diagnostic-generated

# Build in a separate directory with the project's normal platform prerequisites.
make B=build/research GEN=/new/diagnostic-generated

python tools/research_run.py --exe /path/lasrecomp --game /path/game \
  --library /path/diagnostic-lasgame --save-copy /path/save_0 \
  --output /new/trace-record --boundary update --semantics \
  --startup-seed 123456789 --scenario house-exit --updates 4800 --frames 9000 \
  --present fifo --store-manifest /new/diagnostic-generated/research-stores.json \
  --store-window 0:4800 --store-capacity 131072
```

Replay with the same arguments, a new output directory and `--replay /new/trace-record`. Analyze the pair with `tools/research_analyze.py`. The window is expressed in zero-based captured updates, with an exclusive end. Maximum window end is 12,000 and maximum buffer capacity is 262,144 writes. An overflowing or incomplete trace remains on disk with explicit failure metadata.

For a recording-off control, omit `--store-window` in both runs but keep the same diagnostic library, seed, scenario and manifest. This compares the recording toggle within one compiled library. It does not remove the diagnostic library's address checks or establish that instrumentation has no timing effect. Compare normal-library operation separately when needed. Each replay must match its own recording's runtime/library hashes, source save, settings, seed and trace configuration.

## What the evidence means

| Artifact | Interpretation |
| --- | --- |
| `stores-manifest.json` | Local source receipt and mapping from numeric sites to original generated-C locations; explicitly partial coverage |
| `stores-summary.ini` | Window, capacity, recorded/dropped counts, pending pairs, overlap, writes outside the owner update, and completeness |
| `stores.csv` | Before/after observations of the 16-byte region, actual store address/width, source site, guest LR, thread, owner update and observer sequence |
| `store-boundaries.csv` | Update-entry/exit state and sequence, used to reconcile observed writes with the existing update samples |

No mutex is held around the original guest write. The recorder allocates its bounded buffer before guest execution and exports later. Its bookkeeping lock can still affect host scheduling. Before/after memory copies are not atomic snapshots; overlapping observations, missing records and changes outside the stated store are reported, not repaired or silently discarded. Thread IDs and observed order do not themselves establish causal task relationships.

The analyzer validates source-site identity, bounds, ordering, before/after byte ranges, event pairing and the state chain from each update's entry through its observed writes to its exit. A gap makes that segment incomplete. Even a continuous chain does not prove complete coverage: unobserved intermediate changes could restore the same observed state.

Pair comparisons report the first **observed write-trajectory difference within the selected window and coverage**, with nearby source locations. Raw guest thread IDs are retained as evidence but are not used as cross-process identity keys. A differing write order with the same final state remains a write difference. No extra random call or root cause is inferred from it. Consumption reconstruction remains explicitly `not_established`; it requires separately validated algorithm boundaries and write coverage.

## Windows observations, 2026-10-17

The runtime includes the enhanced fork's upstream merge `8b7bfc6` (through upstream `552ef7c`) plus this round's changes. This includes scheduler and Vulkan fixes after the fourth round; these results must not be attributed solely to enabling the new recorder. Each condition uses the same runtime/library within its own record/replay pair, seed `123456789`, copied save, 4,800 update-indexed inputs, FIFO presentation and the existing frame-pacing heuristic. Traced windows cover `[0, 4800)` with capacity 131,072.

The first 1.1.0 trace difference is at update **1,993**, before the first differing post-update RNG state at **2,000**. Both replays of the same recording reproduce these two indices. At update 1,993, both sides record 16 stores. Their first two groups come from functions `0x7000129bd0` and `0x700011caf0` in opposite orders; the observed RNG state transitions and final state still match. The first differing source sites are `main/main_016.c:13766` (site 824) and `main/main_015.c:27319` (site 820) in the original local generated tree. Both first stores have the same address, width, before-state and after-state. They differ in source site and live LR.

At update 2,000, the first replay has 12 observed stores versus the recording's eight; four stores in `0x700011caf0` precede the shared writes in `0x70000cb9e0`. These are raw write counts, not reconstructed call counts. The earlier ordering difference shows why an equal global RNG state at an update boundary is insufficient to establish equal execution paths or equal assignment of random values to consumers. It does not establish why those paths changed, or prove that update 1,993 is the first difference in the entire process.

The three 1.1.0 traces contain 78,564, 78,900 and 79,000 writes, respectively. Every trace reconciles all 4,800 entry/write/exit chains with no drops, pending pairs, overlapping observations, unowned writes or invalid records. The recording-off pair first differs in post-update RNG state at update 2,301. The difference between these conditions is evidence to retain observer-effect and run-variation caveats, not a measurement of recorder overhead or a causal effect estimate.

The 1.0.0 trace likewise finds a source-path difference before the first unequal post-update RNG state: update 2,316 versus 2,320. Its first differing sites are `main/main_296.c:4822` in function `0x7000e493c0` and `main/main_009.c:18084` in function `0x70000cb9e0`; the first stores' addresses, widths and before/after states match. Both traces reconcile all 4,800 updates. The recording-off pair first differs in post-update RNG state at update 3,027.

All indices below are zero-based captured updates. The repeated 1.1.0 replay uses the original trace recording again, so these two comparisons share a recording and are not independent pairs.

| Version / condition | First observed write-path difference | First unequal post-update RNG state | Observed writes, record / replay |
| --- | ---: | ---: | ---: |
| 1.0.0, recording off | Not recorded | 3,027 | Not recorded |
| 1.0.0, trace | 2,316 | 2,320 | 78,708 / 78,688 |
| 1.1.0, recording off | Not recorded | 2,301 | Not recorded |
| 1.1.0, trace | 1,993 | 2,000 | 78,564 / 78,900 |
| 1.1.0, second replay of trace | 1,993 | 2,000 | 78,564 / 79,000 |

All nine Windows runs finish exactly 4,800 captured updates (43,200 total), preserve their source saves, and complete the scripted house/field movement and return. Every pair compares 4,800 identical input samples. All five traces pass the observed state-chain checks; their total is 393,860 writes. This is a small diagnostic sample on one host with adaptive frame pacing, not a statistical estimate or a performance benchmark.

The experiment's frozen artifact hashes are:

| Artifact | SHA-256 |
| --- | --- |
| `lasrecomp.exe` | `baf67196a7da131eaaeeb770d60d16841ce83915be7ca82dfd08373b151a390f` |
| 1.0.0 diagnostic `lasgame.dll` | `4cb066e26850cd19142bb26ac565392ab3f43f3670afdba1f55d0d7ceffd1551` |
| 1.1.0 diagnostic `lasgame.dll` | `abce2b57113c78245b95e5a622086bae4e533a7a6b93eefe4be1859fe4e24327` |
| 1.0.0 source manifest | `117b07055a5c74a78da40ba8bd06b09e50c03e74db038d712c39b55e15a0270d` |
| 1.1.0 source manifest | `8f4685b930a017b00d23fc3e805aa011ea96f2b5692130d89bba2326d0e4a100` |

## Validation and next decision

Synthetic checks cover preserved writes and expression values, disabled behavior, bounded capture, malformed configuration, missing library capability, complete/incomplete windows, overflow, unpaired/overlapping operations, owner attribution, source receipts, original-file preservation, missing records, state-chain gaps and invalid site/sequence data. Existing research and Workshop checks remain required.

Local validation passes seven research executables (tape, runtime, update boundary, actors, startup seed, store recorder and store helpers), 26 Python analysis/instrumentation tests, Workshop checks, and 141 shared addresses, 46 patch outputs and 12 hooks. The capture harness rejects an incomplete observed state chain as well as recorder overflow or pairing errors; this requirement does not promote partial coverage into complete coverage. Both original 645-file generated trees still match their source receipts, and all recorded runtime/library fingerprints match the frozen artifacts.

With research disabled and the ordinary 1.0.0 library, the same runtime passes the photo smoke check: two byte-identical 720x720 square images, their settings recipes and a successful gameplay resume. The photo, resumed image and both versions' final gameplay captures were visually inspected. This is an opening-room regression check. Native Linux/macOS gameplay, combat and longer-session behavior remain unverified.

The follow-up decision is whether an observed writer and its incoming state support a specific, falsifiable upstream-cause hypothesis. Verified task-event markers can be added when their meaning is established. A stable source location alone does not justify changing the scheduler, and successful capture does not certify game replay, high-refresh simulation or co-op.
