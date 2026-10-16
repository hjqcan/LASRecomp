# Multi-game development on enhanced

The target is one application maintained on `enhanced`: select a supported game's
NSP/NSZ/XCI/XCZ, import the player's data, compile locally, then launch that
installation from its desktop shortcut. Each title still needs an explicit,
tested adapter. Recognizing an archive or compiling its code does not establish
playability.

## Implemented foundation

`tools/game_catalog.py` is the title catalog used by the archive importer. It
describes application IDs, accepted content versions, complete 32-byte NSO Build
IDs, module bases, constructor order, generator profile and adaptation status.
Archive import validates every compiled module against the selected release;
an updated executable paired with the wrong SDK is rejected. The existing LAS
patch-address mapping remains in `game_versions.py`; a regression check keeps
its version IDs aligned with the catalog.

| Adapter | Registered versions | Status in enhanced |
| --- | --- | --- |
| `las` | 1.0.0 base, 1.1.0 update | Existing beta installation path; updated randomizer remains experimental |
| `acnh` | 1.0.0 base | Development: inspection and module planning only; installation disabled |

The current launcher still offers LAS version slots. This change does **not**
add a multi-game library screen, automatic shortcuts, or an ACNH runtime to the
published application. ACNH's experimental runtime work must be integrated
through title-specific configuration before enabling its install flag.

Read-only developer commands (also useful for future launcher integration):

```powershell
python tools/game_catalog.py
python tools/game_catalog.py --exefs "path/to/extracted/exefs"
python tools/import_game.py --catalog
python tools/import_game.py --game acnh --base "path/to/base.nsz" --inspect
```

The frozen helper exposes the import commands as
`lasgen --import-nsz --catalog` and
`lasgen --import-nsz --game acnh --base <archive> --inspect`.
Encrypted inputs still require the player's local keys. Metadata inspection
preserves its CNMT/XML/unavailable provenance: XML labels alone do not verify
the executable. The module-plan command checks all registered module IDs and
performs no compilation or filesystem writes.

## Shared core and title adapters

The shared core owns archive parsing, decompression, supported update assembly,
the AArch64-to-C generator, compilation/cache management, graphics, audio,
input, operating-system services, saves and the launcher. Generic fixes found
while adapting one game should improve this core for subsequent games.

Title adapters own module layouts and startup order, supported Build IDs,
game-specific patches, service quirks, feature availability and verification
evidence. LAS randomizer hooks, memory probes, camera offsets and resource
replacements must only run for LAS. ACNH must use its own module initialization
and must not inherit those hooks. A new title can also require new core service
or shader support; a catalog entry alone is insufficient.

Use one extensible, versioned library ABI with a module array, rather than
separate hardcoded main/sdk/subsdk structs. Bump the ABI when its layout changes.
Validate every module identity, adapter revision and library identity before
loading guest code. A game switch starts a fresh process initially; resetting
all state between different games inside one process is a separate feature.

## Intended user flow

1. Select a local archive. Identify its application ID and version, then show
   the matching adapter's status. Unsupported builds get a precise explanation.
2. For an installable adapter, stage extraction and updates in a temporary
   directory. Check every module before publishing the installation.
3. Compile the adapter's module plan with the bundled toolchain. Retain verified
   completed objects when cancelled; report progress and diagnostics.
4. Validate the resulting library and record the installation. Obtain the name
   and icon from the player's control data, with a generic host icon fallback.
5. Create a Windows `.lnk` only after a successful build. The link targets the
   installed launcher with a stable installation ID and direct-play option.
   Creating the link does not itself launch the game.
6. Double-click the shortcut to resolve that installation, verify compatibility
   and start play. Reuse compiled code until its inputs or toolchain require a
   rebuild; a stale or moved installation opens a repair screen.

The proposed `--game <installation-id> --play` interface is not implemented yet.
Use a stable ID instead of embedding mutable cache paths in shortcuts. Resolve
the Windows Desktop known folder, including redirected/OneDrive desktops, and
write ShellLink fields through native APIs rather than shell command strings.

## Storage and compatibility

New installations should live under application ID and complete build identity.
Compilation receipts must additionally include every module's input identity,
adapter revision, generator identity, runtime ABI, target architecture, compiler
and flags. Keep game settings, shader caches, logs and saves isolated by title
and compatible version/profile. Updating the application must not erase saves;
cross-version save migration requires a separately verified path and backup.

Retain existing LAS directories and saves through a versioned migration. Never
silently interpret existing `games/v1.0.0` as another title. Compiled libraries,
generated game C, extracted assets, local keys and saves remain local and are
excluded from source commits and release packages.

## Integration order and release gate

1. **Catalog and identity checks:** implemented here; preserve existing LAS
   imports and register ACNH as development-only.
2. **Common runtime and compiler plan:** merge ACNH's general fixes, introduce
   module arrays and gate all LAS-specific behavior by adapter. Recheck both LAS
   versions while bringing ACNH forward.
3. **Library and shortcuts:** connect import/build progress to the catalog,
   persist installations and create direct-play desktop links.
4. **ACNH acceptance:** verify visible scene rendering and controls, intro/island
   flow, audio, game-managed save/reopen, day rollover, repeated starts and an
   extended gameplay run before enabling installation for users.
5. **Additional titles:** add an adapter and missing core behavior, run its
   acceptance checks, and keep regression coverage for already supported games.

The current ACNH experiment has displayed dialogue, accepted an advance input,
and rendered scene geometry into offscreen targets. Final scene composition,
characters and the gameplay/save acceptance checks remain incomplete. It is not
a stable playable release.

## Foundation validation

The 44 existing synthetic import/container tests and 8 adapter tests pass on
Windows. Checks cover cross-title rejection, changed Build ID tails, mixed SDKs,
missing modules and rejection of development installs before output creation.
Read-only inspection also matches the local LAS 1.0.0/1.1.0 and ACNH 1.0.0
module sets. The rebuilt frozen helper lists the catalog, inspects ACNH metadata,
and imports a synthetic LAS base plus update end to end. These checks validate
the catalog/import change, not ACNH gameplay or a multi-game launcher.
