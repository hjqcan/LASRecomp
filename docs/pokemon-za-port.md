# Pokemon Legends: Z-A integration probe

Status on 2026-10-18: **base 1.0.0 passes the earlier nested-table failure at `0x166df70`; early captures remain black and startup now stops at missing main target `0x1cfcc34`. Surface-write shaders are also unsupported; not playable**.
This is third-title framework evidence, not a completed port. No Z-A release has
been added to the installable catalog. Update 2.0.2 remains an inventory-only
input. Z-A source development is maintained directly on `enhanced`, alongside
official `beta_5` merges. Development status and the failures below restrict
playability claims and prerelease qualification, not source integration.
Tests used local files on Windows, starting
from enhanced commit `871dc34` plus the archive-probe/NCZ changes accompanying
this document. No game files, generated code, libraries or keys are included.

## Observed inputs

The archive labels are base 1.0.0 and update 2.0.2. Their readable XML reports
application `0100f43008c44000`, base content version 0 and update content version
393216. XML is unverified metadata; the filenames do not establish identity.
Both executable inventories contain `main`, `sdk` and `rtld`:

| Module | Base full Build ID | Update full Build ID |
| --- | --- | --- |
| main | `7222e13ecf6adb3249b5dd5c57fa40b102245fce000000000000000000000000` | `b1f12fd919eae86ab8a978317677e64bce443d1f000000000000000000000000` |
| sdk | `df7a3960c74c3a4803f826c5871532523aa9cf2a000000000000000000000000` | same |
| rtld | `88fb7786a17b61768163ad3d8052c93cefc61d81000000000000000000000000` | same |

The main image sizes are 100,114,432 and 102,215,680 bytes; the SDK image is
12,967,936 bytes. An additional supplied DLC NSP could not be read without a
local header key. DLC loading was not tested.

## Initial framework results (2026-10-17)

- Before adapter registration, normal archive inspection rejected the application before
  installation. The developer probe read both complete executable inventories
  into a separate test directory and produced deliberately incomplete adapter
  templates. It did not change existing installations, settings or saves.
- The update has **169 NCZ counter ranges**. The old 64-section limit rejected
  it before decompression. The limit now permits up to 4096 entries while also
  checking that the complete table fits in the archive. Both solid and blocked
  synthetic inputs cover the larger table and malformed/truncated counts.
  NCZ counter ranges are distinct from NCA filesystem sections; the probe also
  permits executable files spanning adjacent counter ranges.
- Full generic generation completed for base main (280,549 functions, 927
  chunks), base SDK (49,410 functions, 122 chunks) and update main (288,324
  functions, 964 chunks). The update used the newly frozen Windows generator.
  Diagnostic bases were `0x7000000000` for main and `0x7008000000` for SDK;
  these were diagnostic bases, subsequently retained by the development adapter.
- Six generated translation units compiled with the packaged Zig compiler and
  current runtime headers: base main chunks 000/463/926, SDK chunks 000/115 and
  the SDK function table. This was an object sample, not a complete linked game
  library. At that stage no game startup or gameplay was attempted; the native
  follow-up below records the subsequent full build and startup result.
- Archive probe, NSZ/NSP/XCI imports, catalog and adapter contract tests passed.
  The frozen-helper regression imported both existing adapters using synthetic
  NCZs with 170 counter ranges and passed its real tiny ARM64 generation check.

The NCZ table interpretation was checked against the
[NSZ reference decompressor](https://github.com/nicoboss/nsz/blob/master/nsz/IndependentNczDecompressor.py).

## Compatibility work still required

The initial base generation emitted unsupported-instruction traps at 13,672 main-module
sites: 1,862 `LDXP`, 3,642 `STXP`, 3,316 `LDAXP`, and 4,852 `STLXP` sites.
The SDK also has one `STG` site. Initial update main reported 13,881 unsupported sites.
These are static generation observations; gameplay reachability has not been
established. Successful C compilation includes those explicit traps and does
not establish executable compatibility.

The subsequent shared-CPU change implements `LDXP`, `LDAXP`, `STXP` and `STLXP`
for both register widths, with address/width reservations and atomic 64/128-bit
compare-and-swap stores. Fresh full generation reports **zero unsupported main
instructions** for both inputs: base now has 929 chunks, update 966; function
counts remain unchanged. The SDK memory-tagging instruction still needs a
separately reviewed runtime policy or reachability evidence. Its observed code
is guarded; it has not been silently ignored or declared safe.

`tools/test_exclusive_pairs.py` passes 128 Unicorn differential sequences,
four-thread counter/complement contention tests for both widths, an
acquire/release handoff, read-only loads, alignment faults, register aliases,
SP/zero operands, CLREX and reservation width/address failures. Existing
single-register success/failure behavior is also covered. Frozen-helper tests
exercise the actual pair decoder. Windows Clang and packaged Zig both compile
and execute the helpers; the packaged DLL link recipe resolves 128-bit CAS
without a separate atomic library. Cross-compiled AArch64 Clang output uses
native LL/SC. Actual macOS/Linux execution still requires CI validation; it
was not performed on this Windows host.

The paired-exclusive step built the full Windows runtime, compiled base main
chunks 000/464/928 with the packaged compiler, and passed native nonlocal-return,
adapter identity and production build/cancel/resume/publication tests. Full
Z-A linking and startup were tested in the subsequent step below.

Reservations retain the runtime's existing **value-based** model: a write that
restores identical bits (ABA) is not detected. This limitation is recorded in
`cpu.exclusive_pairs` as `value_cas`; it is not a complete hardware reservation
model. The CPU structure still occupies 1024 bytes with unchanged existing
field/register offsets. New reservation fields consume reserved padding. Common
code/header identities change, so existing game libraries require rebuilding;
save identities and the module-table ABI remain unchanged.

The implementation follows the pair access and atomic-store requirements in
the [Arm instruction reference](https://documentation-service.arm.com/static/67e40f3398aa3c3b6eea6a85).

## Native adapter and loader follow-up

Base 1.0.0 now has a development-only descriptor. Its main module occupies about
100 MiB, so the SDK is placed at `0x7008000000`, after main's complete image and
BSS; the existing LAS SDK base would overlap it. Main depends on `nnSdk.nss`,
and initialization runs SDK before main. Its RomFS has 22 files (4,276,739,531
bytes) under `arc`, `audio` and `system`; all three roots and the file count are
required. A real standalone development import passed those content checks.

Reading the SDK's static NVN fallback function table and emulating its actual
`DeviceGetInteger` version queries returned **55.15**. Existing titles retain
their default major 53. A new optional major-version descriptor field feeds the
same shared query handler. NVN 55 remains an unavailable qualification capability,
so changing a release flag alone cannot enable normal installation. The adapter
does not claim that the renderer already implements every NVN 55 operation.

The executable modules use `DT_RELR`, which neither the previous generator nor
native loader handled. Their compact tables identify 346,629 main and 14,682
SDK relative relocations. Both paths now decode address/bitmap entries and
implicit addends, alongside ordinary RELA entries. Native validation completes
before writing any RELR target. Tests cover malformed tables, unaligned even
targets, overflow/range errors, bitmap chains and RELA coexistence, following
the [ELF relative relocation format](https://gabi.xinuos.com/elf/06-reloc.html#relative-relocation-table).
Fresh generation discovers 280,731 main functions and 49,412 SDK functions;
main still has no unsupported-instruction diagnostics.

The remaining SDK `STG` matches the tagged-stack cleanup guarded by `stage2`
and `mteTaggedFrame` in LLVM's
[unwinder](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.0/libunwind/src/DwarfInstructions.hpp).
The [CIE parser](https://github.com/llvm/llvm-project/blob/llvmorg-20.1.0/libunwind/src/DwarfParser.hpp)
sets that flag for augmentation `G`. An inventory of the supplied base main,
update main and SDK unwind tables found only `zR` and `zPLR`: respectively
222,376, 228,334 and 32,097 FDEs, with zero tagged frames. This supports attempting
startup with the trap retained. It does not certify dynamically registered
unwind metadata or add MTE emulation.

## Initial native build and startup (2026-10-18)

A private development package built the complete base game with its bundled
generator and Zig compiler: **1,052 objects, 302,430,720-byte DLL, exit 0**, in
about 562 seconds. Native identity validation accepted the library. This READY
result certifies the build pipeline, not gameplay. Generated code and the DLL
stay outside the repository.

Strict startup then exposed three shared runtime gaps, now covered by synthetic
native tests:

- The SDK exports the five-argument `nn::init::Start` variant. Entry selection
  now supports both variants, passes the third callback in `x4`, and finalizes
  modules in reverse initialization order without duplicating `DT_FINI` arrays.
  The legacy four-argument path remains preferred when both symbols exist.
- `GetInfo(28)` now returns the actual alias-extra size, zero for this host
  layout, with invalid-handle and invalid-subtype errors. The meaning and
  validation follow the [kernel implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_info.cpp).
- Service-manager requests use TIPC. The shared codec now decodes its compact
  unaligned payload and result-first response, PID, copy/move handles, returned
  objects and A/B/W map-alias buffers. Object handles precede explicit move
  handles as consumed by the [libnx TIPC client](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/sf/tipc.h).
  Bounds are checked against the 256-byte TIPC command buffer. CMIF requests,
  domain conversion, object responses and object close retain regression coverage.

At that stage, startup reached `nninitStartup` and opened several SDK
services. Some applet commands still use logged default responses, and `ectx:aw`
has only a logging stub; service compatibility is not qualified. Startup stopped
in `StandardAllocator::Initialize` at SDK offset `0x134154`:
the game passes a null backing buffer and requests **`0x300000000` (12 GiB)**
from main caller `0x19e8760`. The SDK calls `IsVirtualAddressMemoryEnabled`,
whose implementation checks `GetInfo(16)` (`SystemResourceSize`). That value
is zero in the current runtime, so the SDK rejects this allocator mode.

The requested size is a virtual address reservation, not evidence that the
game needs 12 GiB of physical RAM. The original alias region was only 4 GiB, and
the physical-map handlers did not supply the partial unmap and resource
accounting needed by this allocator. Enabling it requires a reviewed address
layout and mapping lifecycle, including concurrent mappings, subrange removal,
zeroing/reuse and reset. Returning a nonzero resource size alone is insufficient.
`kernel.virtual_memory: dynamic_physical` was recorded as an unavailable
requirement until the implementation and checks in the follow-up below.

That diagnostic run exited with SDK assertion status 3. It did not reach a
title screen or qualify startup, controls, graphics/audio, saving or extended play. Update 2.0.2 remains
outside the development adapter, and no playable release/package was published.

Final Windows checks passed for the full runtime build, frozen helper imports
for all three adapters, RELR decoding, both SDK entry variants, kernel information,
TIPC/CMIF, catalog and native title identity, adapter contracts/doctor, managed
game profiles, save mount concurrency and save commit failure/retry handling.
These save tests use synthetic data; they do not establish Z-A save support.
Linux/macOS checks were added to CI but were not executed on this host.

## Virtual memory and NVN object follow-up (2026-10-18)

The development adapter now requests the **16 MiB system-resource budget**
recorded at NPDM offset `0x14`. That field is documented by the
[process metadata definition](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libstratosphere/include/stratosphere/ldr/ldr_types.hpp).
Existing adapters default to zero. Their heap, alias, stack, code and HLE
addresses remain unchanged. Only an opted-in boot extends the host reservation
from `0x7320000000` to `0x7800000000`, with a 16 GiB alias region at
`0x7400000000`. The original and extra reservations reset separately.

The shared kernel now tracks physical mappings at 4 KiB granularity. Repeated or
overlapping maps preserve existing bytes; subrange unmaps split mappings and
release backing; remapped pages are zeroed. QueryMemory and process usage follow
the same synchronized map. For opted-in titles, heap allocations and physical mappings share the
existing application-memory budget. Failed allocation rolls back newly mapped
holes while preserving prior mappings. These semantics were checked against the
[physical-memory SVC implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_physical_memory.cpp)
and its [page-table operations](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/kern_k_page_table_base.cpp).

System-resource usage is a bounded host model: the tracking bitmap/counts plus a
4 KiB charge for each populated 2 MiB page-table span. It is not exact Horizon
slab accounting. The runtime retains its existing permissive access protection;
unmapped loads/stores are not guaranteed to fault. On hosts with larger pages,
partial-page release preserves neighboring guest mappings and may retain backing
for unused subpages. The capability remains partial for these reasons.

Real startup now initializes the game's **12 GiB virtual allocator** and reaches
main constructors, resource loading and NVN device initialization. A temporary
large physical reservation reaches about 3.13 GiB, then is released after the
SDK handles a real resource-limit result. The later startup path uses roughly
1.2 GiB of mapped physical memory; a 12 GiB virtual request does not commit
12 GiB of RAM.

That progress exposed two NVN object-size bugs:

- A host sampler descriptor, including its backend pointer, occupied 80 bytes
  when embedded after a device pointer. Z-A provides only 64 bytes for its sampler
  builder, so the old writes cleared saved guest registers on the stack. A compact
  builder now fits in both API generations and expands into a separate host
  descriptor when initialized or registered. Backend pointers stay on the host.
- NVN 55 texture views occupy 16 bytes. The old 40-byte clear and expanded field
  writes corrupted neighboring allocator data. New views use the reviewed packed
  fields; descriptor registration and render-target, clear and copy commands
  expand them into host snapshots. NVN 53 retains its existing guest layout.
  Inspection of both supplied SDKs confirmed the version difference: old default
  setters write through offset `0x20`, while new defaults write only two words.

The strict run at that stage passed both former crashes and reached audio setup, then
stopped at **`hwopus` command 8**, which had no service implementation. `audren:u`
was opened, but audio decoding/playback has not been qualified. No title screen,
rendered game frame, control test, Z-A save/reopen or extended-play pass is claimed.
`audio.opus: service` recorded the missing decoder requirement. NVN 55 also remains
unavailable for release qualification and the adapter remains
`development` / `installable: false`.

New native tests cover map overlap, partial release, zero reuse, physical/system
limits, failed-allocation rollback, eight thousand concurrent mapping cycles,
five thousand randomized page/data/query operations, reset, real Windows
reservation/decommit behavior, both view ABIs, sampler guards
and command snapshot lifetimes. The Windows runtime builds and passes these
checks. Linux/macOS test recipes are in CI; they were not run on this host.

A separate LAS 1.0.0 regression rebuilt all 646 objects with the frozen helper
and packaged compiler (705 seconds, build exit 0). The final runtime kept the
legacy 4 GiB alias range and zero system-resource budget. In an isolated data
directory, it rendered the title screen, loaded a copied test autosave into
Marin and Tarin's house, and responded to scripted input by opening Marin's
dialogue. Captured frames were visually checked and the logs contained no fatal
errors. Each 90-second smoke run was stopped by the test timeout, so this is not
a normal-exit, extended-play or new save-write qualification. Original test saves
were copied, not edited in place.

## Audio service and process context follow-up (2026-10-18)

The enhanced branch first merged the published ACNH Beta 5.3 changes and upstream
`beta_5` through `b041b12`, retaining the fork's features. The merge is `8faa052`.
The upstream shared frame pacer retains LAS-only counter adjustment; other games
keep their real GPU counters and requested presentation intervals. A deterministic
test covers that boundary and the upstream fallback backoff.

The old-game rebuild gate caught a separate upstream-integration regression:
the new font box-glyph null check read the canonical 1.0.0 instruction directly,
so 1.1.0's shifted text failed its instruction guard. It now uses the existing
version-aware `patch_word` accessor. The actual instructions at `main+0x111e000`
(1.0.0) and `main+0x111e0d0` (1.1.0) were verified; the corresponding skip labels
are `0x111e06c` and `0x111e13c`. `test_version_hooks.py` now checks all 47 patch
outputs in both executable profiles, in addition to the hook tests, and rejects
a changed font instruction. The 1.0.0 emitted patch text is unchanged. Both LAS
adapter identities advance conservatively because a shared patch source changed.

The new `hwopus` service uses host libopus, with manager commands 0-9 and the
single-stream/multistream decoder interfaces. It supports interleaved PCM16 at
8, 12, 16, 24 and 48 kHz, the older 40 ms and extended 120 ms workspaces,
streaming history, reset flags and optional final-range checks. Invalid packets
and insufficient output buffers leave output and decoder history unchanged.
Requests sharing an object are serialized, active requests retain their state
during close, and an in-place reboot releases the remaining decoders.

The transport layout follows the [libnx client](https://github.com/switchbrew/libnx/blob/master/nx/source/services/hwopus.c)
and [audio service research](https://switchbrew.org/wiki/Audio_services#hwopus).
The [libopus decoder API](https://opus-codec.org/docs/opus_api-1.5/group__opus__decoder.html)
supplies actual decoding. Workspace sizes are conservative host requirements;
exact DSP allocation sizes, hardware error numbering, DSP timings and multistream
SetContext are not modelled. `audio.opus` is therefore a partial capability.
The dependency is pinned to libopus 1.6.1 with its published SHA-256; build,
packaging, license and CI recipes include it on all three platforms.

Real Z-A startup successfully passes its manager command 8 queries and opens
48 kHz stereo audio output. It has not yet exercised a guest Opus decode call or
established audible Z-A playback. The new tests generate their own audio and
compare the service's PCM byte-for-byte with an independent libopus decoder:
all five rates, mono/stereo, six-channel mapping, 120 ms packets, successive
packets, reset, concurrent requests, close during a request, malformed inputs,
range mismatch, output guards and reboot cleanup pass on Windows. The pinned
Windows libopus build passes the same tests.

The next actual startup exposed `GetInfo(20)` returning zero. SDK function
`sdk+0x13ff80` queried the process-local address and wrote its exception-handler
stack at offset `0x1c0`, causing a null access. The kernel now provides a separate
writable page before thread TLS, with handle/subtype validation and ordinary
guest-memory reset. The process-local address semantics follow the
[kernel GetInfo implementation](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libmesosphere/source/svc/kern_svc_info.cpp).
This provides registration storage; it does not implement guest exception delivery.

With those fixes, strict startup proceeds through more resource reads, empty
isolated save mounts and BCAT setup. It stops at target `main+0x166d8a0`, reached
by an unguarded relative jump table in `main+0x166d840`. That target is absent
from the generated function table. ADR discovery splits off the table's base at
`main+0x166d888`; the unresolved-table fallback only dispatches within its own
fragment, so it cannot reach the case at `main+0x166d8a0`. An asset-free synthetic
probe reproduces that split and confirms that retaining the containing fragment
preserves the case. This is diagnosis, not a qualified discovery-rule fix: real
callbacks and independently referenced function entries must still be retained.
No generator change was included in this step. No Z-A game frame, title screen,
save/reopen or extended play is qualified; NVN 55 and normal installation remain
gated.

Existing-game changes must pass the
[multi-game regression gates](multi-game.md#regression-gates-when-adding-a-game).
The Windows candidate is tested against separate copies of LAS 1.0.0, LAS 1.1.0
and ACNH 1.0.0; observations and limitations are recorded below after each run.
Linux/macOS runtime execution and gameplay are not verified on this host.

The legacy font fix and regression rules were pushed separately as `c376648`.
The audio/context candidate remains local and **is not release-qualified**:
one LAS 1.1.0 restart crashed before reading any save file, in the SDK's
`TlsHeapCache::AllocFuncAlgnMode0` at `sdk+0x2df720`, called during a RomFS
directory lookup. The same candidate completed a 254-second load/move/interior/
exterior/manual-save run with normal exit, backups and unchanged source saves;
a subsequent 115-second run read all 24,288 bytes of the new manual save and
returned to Mabe Village normally. These successes do not resolve the crash.

Follow-up isolation used copies of the previously accepted executable/library
and the candidate, with the same copied saves. Ten starts per executable passed,
including five without extra heap/memory logging. A separate candidate SVC trace
passed and showed no `GetInfo(20)` or physical map/unmap calls during startup.
All 175 SDK generated C files match the accepted baseline byte-for-byte. These
observations narrow the investigation; they do not establish equal failure rates,
prove that the failure predates this work, or qualify the candidate for a prerelease.

The completed Windows regression observations are:

| Existing release | Observed checks | Result and limits |
| --- | --- | --- |
| LAS 1.0.0 | Load copied progress, move between Mabe Village and Marin/Tarin's house, open inventory/system UI, manually save with automatic backups, reopen that save, render frames and produce nonzero audio | Normal exits. The final font-fix build reloaded the full 24,288-byte manual save; the preceding audio/context build performed the save write. No failure in these runs. |
| LAS 1.1.0 | The same load, movement, interior/exterior, manual save, backup, reopen and audio checks | Normal exits for the gameplay and successful reopen, but one additional startup crashed as described above. Candidate qualification remains blocked. |
| ACNH 1.0.0 | Load a copied island, move, use Save and End, observe completed multi-file commit and backups, restart and return to the island | Normal exits after 563 and 92 seconds. All five full data files were read on reopen; all five headers and 24 internal hashes validated after saving and again after reopening. Nonzero stereo audio was recorded. |

Each run used independent data/settings folders and copied saves. Source-save
SHA-256 manifests were checked after the runs and remained unchanged. The actual
captured frames were inspected; audio checks establish nonzero finite 48 kHz
stereo mixer output, not listening-based quality acceptance. The final candidate
executable was identical across the final LAS reopen and both ACNH runs. These
are bounded gameplay regressions, not full-playthrough or performance-parity
results, and they do not cover the experimental 1.1.0 randomizer. The shared
audio/context changes must stay out of a distributed prerelease until the LAS
startup failure has been explained and the affected checks pass again.

## Relative-table discovery follow-up (2026-10-18)

Enhanced now includes official `beta_5` through `ca7cd3e`. The first official-only
merge was published as `b8aae96`, with the enhanced segmented GPU-store
path retained and the upstream Ryujinx notice included in all package layouts.
The Windows runtime build, GPU-memory regression and stronger Windows thread
wait/TLS-cleanup regression pass; all seven remote CI workflows pass for that
merge. Follow-up `609c33f` adds the latest upstream condition-variable regression
and contributor credit without changing runtime behavior. Its test fixture uses
the enhanced module count and aborting stubs for unrelated memory/profile/research
entry points; the test passes in both the published and research checkouts.
All seven remote CI workflows also pass for `609c33f`.
At that checkpoint, the audio/context and discovery experiments were kept
local while the earlier LAS 1.1.0 startup failure was unresolved.

The discovery candidate recognizes an exact `ADR; LDRB/H; ADD; BR` sequence in
which the ADR is only a local relative-table arithmetic origin. It removes that
one discovery reference only when the existing resolver cannot enumerate a
bounds-checked table and the origin stays inside the same known function. It
does not infer an index range. Independent symbols, relocations, direct calls
and callback materializations continue to create entries; already-resolved
tables keep their previous partitioning. A scheduled `MOV Xd/Wd, ZR` before the
branch is also accepted when it cannot overwrite the branch register.

Seven asset-free tests cover byte/halfword tables, retained independent entries,
ambiguous register use, cross-function origins, bounded-table preservation and
the scheduled return-value clear.
The emitted native functions also match ARM64 execution for each synthetic
case. Nine cross-fragment table tests and frozen adapter
generation tests pass. Both table suites are wired into the three-platform CI.

A separate bounded-table repair searches through stack-frame setup before a
taken guard's table body, within the existing 64-step emulation budget. The
former 12-instruction search stopped partway through a prologue. The actual
table in `main+0x5e2a68` now resolves all seven targets, including `0x5e2e58`;
its synthetic fixture also checks cross-fragment emission and a call barrier.

Comparing real modules against the preceding discovery implementation gives:

| Input | Discovery comparison |
| --- | --- |
| LAS 1.0.0 and 1.1.0 | Each main module retains all 86,669 entries; each SDK retains all 34,982. Function ends, labels and resolved tables are identical. |
| ACNH 1.0.0 | Main changes 38 entry records and removes 31 boundaries; SDK changes 12 records and removes six boundaries. The subsdk module is identical. |
| Z-A 1.0.0 | Strict native startup passes the formerly missing `main+0x166d8a0` case and continues to the `csrng` service. |

These structural comparisons do not replace the saved-game regression matrix.
No Z-A gameplay qualification follows from this generator result alone.

The next strict run reached `csrng` command 0. Following the
[libnx wire contract](https://github.com/switchbrew/libnx/blob/master/nx/source/services/csrng.c),
the runtime now fills its output map-alias buffer using OS entropy: checked
`rand_s` on Windows, `getrandom` with interruption/short-read handling on Linux,
and `arc4random_buf` on macOS. Empty buffers succeed without querying the OS;
missing or overflowing descriptors and host failures return errors. No weak
fallback or game-randomizer seed is used. Tests exercise output canaries for
every length from zero through 1,024 bytes, invalid descriptors and injected
host failures; the Linux path also tests short reads and interruption.

Actual Windows startup then completed two eight-byte requests and stopped at
`main+0x27f5fac`. Its table has a return-value clear scheduled between the target
addition and branch, which motivated the additional recognition rule above.
Strict native startup now passes that site, input initialization and the
`0x5e2e58` site repaired by the bounded-table change. Windows service tests and
the runtime build pass. Linux/macOS service execution has not been checked on
this host.

The final strict probe stops at `main+0x607d90`, from the signed 32-bit relative
table at `main+0x607b20`. An ADR materializes its own arithmetic origin at
`0x607b14`, creating a fragment boundary after the `b.hi` guard. A read-only
probe using the whole function resolves 14 targets, including the failing one;
the same resolver starting at the emitted fragment finds no bounds. This
fallthrough-guard visibility issue is the next unresolved discovery case.
No game frame, title screen, actual Opus playback or save/reopen is qualified.

The final Windows research executable (`cbf4dfbf1ff6f87c...`) was used for all
three new legacy checks. Each LAS rebuild reused 645 objects and recompiled only
the library entry object. LAS 1.0.0 and 1.1.0 then read the complete 24,288-byte
copied manual save, moved into and out of Marin/Tarin's house, opened the item
menu and exited normally after 94 and 90 seconds. Their copied save files did
not change. This batch checks existing save reads; it does not repeat the earlier
manual save-write tests or qualify the experimental updated-version randomizer.

ACNH rebuilt 37 objects and reused 693. A 290-second run loaded the copied
island, moved and completed Save and End with backups and all five data/header
pairs committed. A separate 130-second run read all five full data files and
returned to the island. All five version headers and 24 internal hashes passed
after saving and again after reopening. Reopening performs an ordinary autosave,
so the copied ACNH files can change during that check. Both runs exited normally.

Source-save SHA-256 manifests remained unchanged, as did the accepted player
executable. Captured frames were inspected. A full old/new generator comparison
includes the preceding `Module` implementation as well as discovery, and gives
the structural results in the table above. These are bounded regressions, with
no performance-parity or full-playthrough claim. The earlier LAS 1.1.0 startup
failure remains unexplained, so this research candidate is still not qualified
for a prerelease despite the successful new runs.

## Fragmented guards and first presentation attempt (2026-10-18)

The next discovery change resolves a fallthrough guard across an artificial
fragment boundary while staying inside the known function. Calls and rewritten
condition flags prevent borrowing an earlier guard. It also tracks a masked
index recomputed from the same unchanged source, including ABI-preserved
registers across calls. A wider mask, overwritten source or basic-block join
does not inherit that proof. This corrects both the `0x607d90` failure above
and the subsequent `0x60f4e4` failure without inventing an index bound.

Nine new asset-free cases include native-versus-Unicorn execution, default
branches and rejected ambiguous cases. Together with the nine cross-fragment
and seven relative-origin tests, all 25 cases pass locally. The final frozen
generator passes adapter generation checks. These changes were research
work excluded from the separately published official-only sync at that checkpoint.

An intermediate Windows probe passed both repaired sites and reached NVN
presentation frame 0. It reported unsupported Maxwell shader words
`eb20018800f70804`, `eb5006080207000f` and `eb20018800f70408`, then terminated
at main target `0x166df70` after about 119 seconds. A captured thread stack
placed the presentation delay in the Intel shader compiler; this run did not
establish a GPU submission timeout. The missing target belongs to another
relative table in a function whose earlier bounded table still introduces a
fragment boundary. No captured game image, title screen, audio playback or
save/reopen is qualified by that presentation call.

Comparing the final guarded discovery with the complete pre-table `5bb42bb`
implementation gives the following structural results:

| Input | Final guarded discovery comparison |
| --- | --- |
| LAS 1.0.0 and 1.1.0 | Main retains 86,669 entries and the same function ends; two tables now enumerate all 45 and 13 masked-index targets. SDK retains 34,982 identical entries. |
| ACNH main | 177,426 to 177,434 entries; 43 retained records change, one entry is removed and nine are added. |
| ACNH SDK | 44,716 to 44,705 entries; 30 retained records change and 11 are removed. |
| ACNH subsdk | All 20,599 entries remain; 12 retained records change. |

The discovery comparison and a completed library rebuild do not replace
runtime regression. The earlier LAS 1.1.0 startup crash remains unresolved,
so the research runtime is not qualified for a distributed prerelease.

## Official synchronization and isolated legacy regression

The official synchronization commit `c073733` incorporates `beta_5` through
`290990e`, including rumble and launcher shader preparation, while retaining
enhanced's shared compilation queue, prepared-object ownership, translated UI
and managed imports. The published enhanced branch subsequently advanced to
`aab8190`, which has `c073733` as a parent and includes official `0f5a5ab`.
All seven remote workflows pass for both commits, including Windows, Linux and
macOS builds. The later merge adds movie-resource cleanup, miniboss-frequency
controls and upstream maintenance changes. The local research branch includes
that published state through merge `03f7d88`; its Windows build, fresh frozen
generator, movie lifecycle test and Vulkan fence-sequence test pass.

A separate official-sync executable (`10f3a61056f706bb...`, source `c073733`)
was used for the complete bounded gameplay batch below. It excludes the
unqualified Z-A audio/context and discovery experiments.

| Input | Official-sync gameplay evidence |
| --- | --- |
| LAS 1.0.0 | Load, move indoors and outdoors, open inventory, manually save, return to the launcher in process, then reload the complete 24,288-byte save. Normal exit after 195 seconds. All 895 prepared shader variants succeed before both boots. |
| LAS 1.1.0 | The same movement, inventory and manual-save checks, followed by a separate restart and full save read. Normal exits after 160 and 64 seconds. |
| ACNH 1.0.0 | Load the island, move, complete Save and End, then restart and return to the island. Normal exits after 240 and 119 seconds. All five version headers and 24 internal hashes pass after saving and again after reopening. |

Actual captured checkpoints were inspected. Each game produced finite, nonzero
48 kHz stereo mixer output, and all source-save manifests remained unchanged.
The LAS 1.0.0 reboot stopped 44 guest threads, drained and released renderer
resources, then prepared all variants again. An earlier interrupted run is
retained separately and is not counted as a normal-exit result. These checks
establish bounded functionality; they do not establish performance parity,
listening-based audio quality, a full playthrough or updated-version randomizer
qualification. The official executable, frozen generator, headers and three
game libraries are retained privately for reproduction. No game data is added
to the repository, and the player's installed application remains untouched.

## Latest research probe after synchronization

The merged research executable (`0719ef18969a321a...`, source `03f7d88`)
rebuilt the complete Z-A library in 160 seconds, reusing 1,051 objects and
compiling three. Strict startup then stopped at the same `main+0x166df70`
target with LR `0x166de08`, exit status 3, after 7.19 seconds on this later
run. The same three unsupported shader words were reported. Early captures
at frames 1, 2, 3 and 5 contain only black output; inspected captures do not
show a logo, title screen or gameplay. The shorter run does not resolve the
compatibility failure or establish a performance improvement.

The remaining branch failure requires a discovery repair, and the shader
words still require translation support. Z-A remains development-only, and
the earlier LAS 1.1.0 startup failure continues to block prerelease qualification of the
shared research runtime.

The same final research executable then passed another bounded legacy batch.
Each LAS rebuild reused 643 objects and compiled three; ACNH reused 553 and
compiled 178. LAS 1.0.0 and 1.1.0 read the copied manual save, moved into and
out of the house, opened inventory and exited normally after 82 and 81 seconds.
Their copied saves did not change. ACNH reopened the saved island and exited
normally after 100 seconds; all five version headers and 24 internal hashes
remained valid after its ordinary autosave. Captured checkpoints were inspected
and each run produced finite, nonzero 48 kHz stereo audio.

Source-save manifests and the accepted player executable's SHA-256 remained
unchanged. This last batch verifies reads and movement on the research
candidate; manual-save writes and launcher reboot were exercised separately on
the official-only executable above. Successful bounded runs do not explain or
clear the earlier LAS 1.1.0 startup crash, so prerelease qualification stays blocked.

## Official 35f7d30 baseline and nested relative tables

Published enhanced commit `0dc5db3` merges official `beta_5` through `35f7d30`.
All seven workflows pass, including Windows, Linux and macOS builds. This merge
includes upstream shader instruction corrections, cache versioning and periodic
Vulkan pipeline-cache persistence, Vulkan error handling, Metal binary archives
and deferred clears, and randomizer miniboss changes. Metal execution remains
untested on this Windows host. Existing enhanced watchdog and shader ownership
protections remain covered by their injected regressions.

The official-only executable (`dddd96afb6ccd1bb...`) passed the complete
copied-save batch before publication:

| Input | Observed result |
| --- | --- |
| LAS 1.0.0 | Indoor/outdoor movement, inventory, manual save, launcher reboot in the same process and a second full 24,288-byte save read; normal exit after 307 seconds. Both boots prepared 908 shader variants with no failures. The reboot stopped 44 guest threads and released renderer/audio resources. |
| LAS 1.1.0 | Movement, inventory and manual save, then a separate restart and full save read; normal exits after 138 and 52 seconds. |
| ACNH 1.0.0 | Island movement, Save and End, and a separate restart back to the island; normal exits after 209 and 97 seconds. Five version headers and 24 internal hashes passed after both runs. |

Actual captures were inspected and all five runs produced finite, nonzero
48 kHz stereo mixer output. Source-save manifests and the accepted player
application were unchanged. Runtime, frozen helper, headers and game libraries
are retained privately. These are bounded functional checks, with no new
full-playthrough, performance or updated-version randomizer qualification.

Research commit `1a7170e` addresses a bounded relative table whose case span
contains an unresolved relative-table branch. Treating the outer table's ADR
origin as a callable entry fragmented subsequent code and excluded reachable
cases. The repair recognizes the exact local ADR/load/add/branch use and
excludes that arithmetic origin only for this nested case. Disjoint bounded
tables retain their previous partitions. Independently materialized callbacks,
ADRP/add references, calls, relocations and symbol entries remain intact; no
unresolved table length is guessed.

Thirty jump-table regressions pass. Byte and halfword mixed-table fixtures run
through Unicorn and generated native code with matching case/default results,
stack pointer and return address. Full discovery comparisons against `67d7405`
show identical LAS 1.0.0/1.1.0 main and SDK records. ACNH loses one arithmetic
entry in main and four in SDK; its subsdk is unchanged. Z-A removes 67 such
entries in main and 21 in SDK, and the former `0x166df70` target is now present
in generated dispatch. The repair deliberately leaves unrelated large
functions partitioned as before.

Commit `18a2b7b` also makes the frozen-helper regression execute a bounded
ARM64 jump table through Unicorn's native JIT. This catches a manual Windows
packaging omission that straight-line generation did not exercise. The normal
`gen-helper` target already applies `pe_no_cfg.py`; the manually prepared helper
was corrected with that same build step, without changing host security policy.

The research executable (`988e385617139d9b...`) rebuilt Z-A in 181 seconds,
reusing 1,041 objects and compiling 13. Strict startup then advanced beyond the
old failure and stopped after 31.71 seconds at `main+0x1cfcc34`, with LR
`main+0x1d10240`. Early captures at frames 1, 2, 3, 5 and 10 remain black.
No title screen, gameplay or Z-A save/reopen is qualified by this run.

The same executable passed the bounded legacy batch: LAS 1.0.0 and 1.1.0
loaded their copied saves, moved indoors/outdoors and opened inventory, then
exited normally after 85 and 96 seconds. Their copied saves did not change.
ACNH reopened the island and exited normally after 100 seconds; all five
headers and 24 hashes remained valid after its ordinary autosave. Captures
were inspected and all three runs produced finite, nonzero 48 kHz stereo
audio. Original saves and the accepted player application remained unchanged.

During validation, published enhanced advanced to `c7542f0`, adding
adapter-aware extracted-folder mounting and the correct launcher version label.
All seven workflows pass for that commit too. The research branch merged it
as `0a0e2fb`; eight mounting regressions and the Windows build pass. The resulting
executable (`bda778bf258d04c9...`) mounted all 22 Z-A RomFS files from the folder
and reproduced the same `0x1cfcc34` failure after 10.25 seconds. Additional
captures at frames 20, 30 and 50 are black. The legacy runtime batch above used
the preceding executable, so it is not a claim of a repeated gameplay batch
on this later build.

Inspection of the new failure shows a local byte-table target calculated at
`main+0x1cfcbd8`, followed by floating-point work before `BR x11` at
`main+0x1cfcc24`. That scheduled sequence exceeds the existing computed-target
lookback and does not match the adjacent relative-table recognizer. A follow-up
needs register-aware validation across those intervening instructions, with
tests for overwritten branch registers and independent callback entries.

The three reported shader words remain unsupported. The
[envytools GM107 decoder](https://github.com/envytools/envytools/blob/f102b82381f3f11cee113d16374c87091db039d9/envydis/gm107.c#L1908)
identifies `eb20018800f70804` and `eb20018800f70408` as SUST, and
`eb5006080207000f` as SURED. The current Vulkan path lacks the corresponding
storage-image binding/write support. Correct support must cover translation,
descriptor reflection and binding, resource aliases and synchronization; simply
accepting the words would not establish correct rendering.

Further work should merge official `beta_5` first and extend its implementation
with focused, independently verified fixes. Prefer upstream implementations
where they cover the same behavior; propose isolated improvements upstream when
the local implementation has stronger evidence. Ongoing research is committed
directly to `enhanced` with its development gates intact. The earlier unexplained
LAS 1.1.0 startup crash and current Z-A failures remain open issues and prevent
qualifying these changes for a distributed prerelease or marking Z-A playable.

## Reproduction

Use a fresh local directory outside the checkout for inventories and modules:

```text
python tools/adapter.py probe --archive <base.nsz> --output <base-inventory.json> --exefs-output <base-exefs>
python tools/adapter.py probe --archive <update.nsz> --output <update-inventory.json> --exefs-output <update-exefs>
python tools/adapter.py scaffold --id pokemon_za --name "Pokemon Legends: Z-A" --application-id 0100f43008c44000 --version 1.0.0 --exefs <base-exefs> --output <new-template.json>
python tools/gen.py main <base-exefs>/main 0x7000000000 <local-generated>/main --profile generic --simd c
python tools/gen.py sdk <base-exefs>/sdk 0x7008000000 <local-generated>/sdk --profile generic --hle runtime/hle/hle_symbols.txt --simd c
python tools/test_adapter_probe.py -v
python tools/test_import_game.py -v
python tools/test_nsp_import.py -v
python tools/test_xci_import.py -v
python tools/test_game_catalog.py -v
python tools/test_adapter_contract.py -v
python tools/test_exclusive_pairs.py -v
python tools/test_relr.py -v
python tools/test_sdk_start.py
python tools/test_kernel_info.py
python tools/test_kernel_memory.py
python tools/test_platform_memory.py
python tools/test_nvn_sampler.py
python tools/test_nvn_views.py
python tools/test_hwopus.py
python tools/test_frame_pacing.py
python tools/test_tipc.py
python tools/adapter.py doctor --game pokemon_za
python tools/test_frozen_adapters.py <fresh-lasgen-executable>
```
