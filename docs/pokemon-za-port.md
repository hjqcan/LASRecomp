# Pokemon Legends: Z-A integration probe

Status on 2026-10-18: **base 1.0.0 reaches audio initialization; blocked by missing hwopus service; not playable**.
This is third-title framework evidence, not a completed port. No Z-A release has
been added to the installable catalog. Update 2.0.2 remains an inventory-only
input. Tests used local files on Windows, starting
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

The final strict run passes both former crashes and reaches audio setup, then
stops at **`hwopus` command 8**, which has no service implementation. `audren:u`
was opened, but audio decoding/playback has not been qualified. No title screen,
rendered game frame, control test, Z-A save/reopen or extended-play pass is claimed.
`audio.opus: service` records the missing decoder requirement. NVN 55 also remains
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
python tools/test_tipc.py
python tools/adapter.py doctor --game pokemon_za
python tools/test_frozen_adapters.py <fresh-lasgen-executable>
```
