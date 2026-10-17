# Developing a reviewed game adapter

Development stays on `enhanced`. Each game and executable version needs reviewed
configuration and actual gameplay evidence. A matching archive, successful
compilation or passing synthetic test does not establish playability.

## Responsibilities

`adapters/<id>/adapter.json` owns a title's application ID, supported releases,
module identities and bases, initialization order, content validation, runtime
configuration, compatibility requirements and acceptance checklist. Each release
has its own `status` and `installable` flag; a development release cannot be
installable. Releases may override the common initialization order, RomFS roots,
runtime parameters and capability requirements when an update changes them.

The shared core implements archive/container parsing, supported update assembly,
generation, compilation, installation publication, graphics, sound, input, host
services, saves and desktop launch. New runtime behavior belongs in the relevant
core implementation and needs an independent regression test. Game-specific
behavior must be explicitly selected by the adapter. LAS's existing complex
hooks/randomizer profile remains guarded; generic adapters never inherit it.

`adapters/core-capabilities.json` records supported variants and known limits.
For example, complete virtual-pool aliases are available, sparse mapping is not,
and network services provide offline responses rather than online play. These
declarations and referenced tests are review aids, not automatically collected
gameplay evidence. An installable release cannot require an unavailable variant.

## Start with an inventory

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
```

`check --write` generates `runtime/title_catalog.inc` and
`runtime/game_installations.inc`; do not edit those outputs by hand. Ordinary new
releases omit the legacy installation fields and receive paths under their
application ID. The existing LAS/ACNH indices, paths and settings aliases remain
explicit compatibility entries. Save paths are independent of code/cache IDs.

The doctor reports declared capabilities, limits and acceptance steps. With a
local installation it checks every module identity. With a log it extracts missing
HLE/IPC calls, without treating log content as instructions. Use
`LARECOMP_STRICT_HLE=1` and `LARECOMP_STRICT_IPC=1` during port diagnostics to stop
at missing implementations. The old `ACNH_STRICT_*` names remain aliases.

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

## Two separate acceptance decisions

Developer qualification is per game/version: startup, controls, graphics/audio,
game-managed saving and reopening, repeated starts, relevant time transitions
and an extended gameplay run. Record known unsupported functionality. Keep the
release in development until these checks support the intended claim.

Local installation checks verify the imported content, complete build, loadable
library and matching identities. Once they pass for an installable release, the
publisher switches the startup record and creates its shortcut. The player does
not need to perform the developer qualification again.

## Repeatable engineering checks

```text
python tools/test_adapter_contract.py
python tools/test_game_install.py
python tools/test_gamebuild_resume.py
python tools/test_title_profile.py
python tools/test_game_profiles.py
python tools/test_game_shortcut.py
python tools/test_frozen_adapters.py <built-lasgen-executable>
```

The contract test introduces a third synthetic title with different content
roots and checks native selection, generated slots, settings and save identity.
It also checks actual Makefile cache identity behavior, per-release dependencies,
guarded patches and platform-independent descriptor hashing. The installation
test kills a subprocess on both sides of publication and checks restart behavior,
concurrent writers, candidate isolation and cleanup boundaries. The build test
uses a real compiler and production library validator/publication code with tiny
synthetic function tables. The frozen-helper test checks imports, inventories,
receipts and actual generation of a tiny synthetic ARM64 function.

Record framework integration effort separately from compatibility work for the
next real title: descriptor changes, dedicated patches, core changes and time
spent diagnosing missing behavior. A shorter registration diff is useful only
when failures are understandable and existing games retain their behavior.

The installation publisher currently retains previous generations and copies
data/cache during a rebuild. Account for that temporary/retained disk usage;
automatic reclamation and a rollback UI are future work. Desktop link tests use
a scratch desktop. These engineering checks do not certify a new playable game.
