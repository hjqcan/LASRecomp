# Pokemon Legends: Z-A integration probe

Status on 2026-10-17: **unregistered and not playable in this runtime**. This is
third-title framework evidence, not a completed port. No Z-A release has been
added to the installable catalog. Tests used local files on Windows, starting
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

## Framework results

- Normal archive inspection rejected the unregistered application before
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
  these are not an approved runtime layout.
- Six generated translation units compiled with the packaged Zig compiler and
  current runtime headers: base main chunks 000/463/926, SDK chunks 000/115 and
  the SDK function table. This was an object sample, not a complete linked game
  library. No game startup or gameplay was attempted.
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

The full Windows runtime builds, base main chunks 000/464/928 compile with the
packaged compiler, and native nonlocal-return, adapter identity and production
build/cancel/resume/publication tests pass. A complete Z-A game library and
startup/gameplay/save checks remain outstanding.

Reservations retain the runtime's existing **value-based** model: a write that
restores identical bits (ABA) is not detected. This limitation is recorded in
`cpu.exclusive_pairs` as `value_cas`; it is not a complete hardware reservation
model. The CPU structure still occupies 1024 bytes with unchanged existing
field/register offsets. New reservation fields consume reserved padding. Common
code/header identities change, so existing game libraries require rebuilding;
save identities and the module-table ABI remain unchanged.

The implementation follows the pair access and atomic-store requirements in
the [Arm instruction reference](https://documentation-service.arm.com/static/67e40f3398aa3c3b6eea6a85).

Next review the module layout and initialization, NVN version,
RomFS rules, required HLE/services and update assembly. Only then register a
development adapter and proceed to complete linking, strict startup diagnostics,
graphics/audio/controls, saving and reopening, repeated starts and extended play.

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
python tools/test_frozen_adapters.py <fresh-lasgen-executable>
```
