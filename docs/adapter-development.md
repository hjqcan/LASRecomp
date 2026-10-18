# Developing a reviewed game adapter

Development stays on `multi`. Each game and executable version needs reviewed
configuration and working gameplay. A matching archive, successful
compilation or passing synthetic test does not establish playability.

## Responsibilities

`adapters/<id>/adapter.json` owns a title's application ID, supported releases,
module identities and bases, initialization order, content validation, runtime
configuration, compatibility requirements and acceptance checklist. Each release
has its own `status` and `installable` flag; a development release cannot be
installable. Releases may override the common initialization order, RomFS roots,
runtime parameters and capability requirements when an update changes them.
`runtime.nvn_major` is optional and defaults to 53 for existing definitions;
`runtime.nvn_minor` remains required. These fields report the reviewed API
version to guest code. They do not establish renderer compatibility: an
unqualified major version must remain a development requirement.
`runtime.system_resource_size` defaults to zero, retaining the existing memory
layout. A reviewed nonzero byte budget (page-aligned, 1 to 512 MiB) opts into the
shared dynamic physical-memory implementation and requires
`kernel.virtual_memory: dynamic_physical`. The current layout reserves a separate
16 GiB alias region when that title starts. It does not commit that whole region.
The budget must come from reviewed process metadata; arbitrary imported metadata
does not turn this capability on. See the limits in `adapters/core-capabilities.json`.

The shared core implements archive/container parsing, supported update assembly,
generation, compilation, installation publication, graphics, sound, input, host
services, saves and desktop launch. New runtime behavior belongs in the relevant
core implementation. Extend existing tests for substantive behavior changes. Game-specific
behavior must be explicitly selected by the adapter. LAS's existing complex
hooks/randomizer profile remains guarded; generic adapters never inherit it.

`adapters/core-capabilities.json` records supported variants and known limits.
For example, complete virtual-pool aliases are available, sparse mapping is not,
and network services provide offline responses rather than online play. These
declarations and referenced tests are review aids, not automatically collected
gameplay evidence. An installable release cannot require an unavailable variant.

## Start with an inventory

An unregistered archive can be inventoried without adding it to the catalog:

```text
python tools/adapter.py probe --archive <local-game.nsz> --output <new-inventory.json> --exefs-output <new-local-exefs-directory>
```

The probe reads bounded NSO headers, reports every module's full Build ID and
memory layout, and compares the executable set with existing adapters. It does
not install, publish, execute or qualify the game. `--exefs-output` is optional;
omit it for a JSON-only inventory. Decompression uses temporary disk space, which
is removed after the probe. Existing output directories/files are not replaced.
Encrypted NSP/XCI content requires explicitly supplied `--keys` / `--title-keys`.
NSZ XML labels are reported as unverified; filenames never establish identity.
Probe base and update archives separately; this command does not merge updates
or install DLC. Keep all extracted modules outside the source checkout.

The scaffold command reads NSO headers without executing game code:

```text
python tools/adapter.py scaffold --id example --name "Example" --application-id <16-hex-id> --version 1.0.0 --exefs <local-exefs> --output <new-template.json>
```

It copies only module names and complete Build IDs into the template. Module
bases, initialization order, NVN version, content roles/versions, content rules
and capability requirements still require review. The template intentionally
fails validation until these values are supplied; it is not an enabled adapter.
Do not commit game data, generated code, compiled game libraries, keys or saves.

After reviewing a definition, place it at `adapters/<id>/adapter.json` and run:

```text
python tools/adapter.py check --write
python tools/adapter.py check
python tools/adapter.py doctor --game example
python tools/adapter.py doctor --game-dir <local-installation> --log <local-runtime-log>
python tools/adapter.py doctor --game-dir <managed-slot> --pending
```

`check --write` generates `runtime/title_catalog.inc` and
`runtime/game_installations.inc`; do not edit those outputs by hand. Ordinary new
releases omit the legacy installation fields and receive paths under their
application ID. The existing LAS/ACNH indices, paths and settings aliases remain
explicit compatibility entries. Save paths are independent of code/cache IDs.

The doctor reports declared capabilities, limits and acceptance steps. With a
local installation it checks every module identity. With a log it reports
`runtime_diagnostics.blockers`: missing generated functions, unsupported shader
instructions, guest traps, host exceptions and missing HLE/IPC calls. Repeated
records retain occurrence counts and first/last line numbers; the legacy
`unimplemented_calls` field remains available. Log reading and record sizes are
bounded, and truncation is explicit. An empty report is not a compatibility pass.
Log content is diagnostic data and is never executed. Use
`LARECOMP_STRICT_HLE=1` and `LARECOMP_STRICT_IPC=1` during port diagnostics to stop
at missing implementations. The old `ACNH_STRICT_*` names remain aliases.

Use a new game's failures to improve the shared subsystem and add synthetic
regressions that do not depend on that game's assets or addresses. For example,
`cpu.jump_tables: low_mask_relative` resolves masked byte/halfword tables even
when vector work is scheduled before the branch. Multiple tables in one function
must all have a local proof, and no incoming branch, callback, relocation or
table case may bypass any member's mask or address setup. The group is bounded
to 16 branches and each setup to 64 instructions. An unsupported capability such as
`gpu.surface_write: store_reduce` stays visible as unavailable instead of being
silently skipped. Declaring or implementing one capability does not qualify the
whole game.

Crash explanations also follow the selected adapter's content threshold. A
small but complete data folder must not inherit LAS's file count or suggest that
the player re-extract data when generated code is missing.

For a managed slot, the doctor follows the published installation record and
checks its receipt against the identified game/version. `--pending` explicitly
selects the unfinished candidate instead; its report labels that state and does
not present it as an installed or playable version. Legacy extracted folders
remain accepted. Reading a record does not lock out atomic publication.

## Guarded instruction patches

Generic releases can declare narrowly reviewed instruction replacements:

```json
"instruction_patches": [
  {
    "module": "main",
    "offset": "0x1234",
    "expected": "0x52800020",
    "replacement": "0x52800040",
    "reason": "Explain the reviewed compatibility requirement here."
  }
]
```

This is a format example, not a patch for an existing game. Offsets are relative
to the module's guest base and must lie inside executable text. Every original
instruction is checked before any replacement is applied to the temporary
generation image. Original game files are unchanged. Module Build IDs, patch
words and relevant generator inputs participate in the release code identity.
The frozen helper and native planner exchange that identity, so mismatched
resources fail before generating code. More complex hooks still require a
reviewed implementation; arbitrary scripts/plugins are not loaded from archives.

Use `tools/gen_game.py` or the launcher to pass the complete reviewed module plan.
Standalone `tools/gen.py` retains its LAS default for existing developer scripts.

## Integration and release

Keep unfinished adapters marked as development and unavailable for normal
installation. Their source can live on `multi` while implementation continues.
Enable installation only when the supported startup, gameplay and save paths work.

Run `python tools/adapter.py check` after changing adapter definitions. Use the
existing catalog/contract tests and the tests for the affected subsystem; avoid
duplicating the complete runtime matrix for a documentation-only change.
Use [the release checklist](multi.md#repeatable-release-checks) before packaging.
