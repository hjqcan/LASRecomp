# Shared game installations

The enhanced runtime reads reviewed `adapters/*/adapter.json` definitions through
`tools/game_catalog.py` and generates its native catalog and installation list.
It identifies each reviewed game/version by every module's full 32-byte
Build ID. LAS 1.0.0 and 1.1.0 use two modules; ACNH 1.0.0 uses three. ACNH's base
release is available as a Windows beta; see [ACNH support](ACNH-port.md) for its
requirements and limits.

Archive inspection accepts NSP, NSZ, XCI and XCZ. The default is automatic game
identification. Readable content metadata provides a preliminary identity;
metadata-free NCZ packages are inspected through their executable modules.
Filenames do not select the game or version. Import validates module identities,
base/update roles and RomFS structure. The launcher stages this content, builds
and validates the exact library, then publishes the installation. The standalone
import command below extracts data only; it does not compile or create shortcuts.
For supported releases, standalone inspection and extraction use:

```
python tools/import_game.py --base /path/to/game.nsz --inspect
python tools/import_game.py --base /path/to/game.nsz --output /path/to/install
```

`--allow-development` is reserved for local testing of other unfinished ports.
It is not required for the reviewed LAS releases or ACNH 1.0.0.

Encrypted content uses locally selected player key files. No keys, extracted
game code, game assets, generated sources, game libraries or saves belong in
source commits or distributed runtime packages.

## Shared Vulkan shader preparation

All titles use background shader preparation by default. Program registration
snapshots the translated code range and queues common variants; bounded
command-list lookahead queues exact attribute/target variants ahead of draws.
Lookahead stops at memory-writing, dispatch and synchronization commands.
It does not execute guest commands. A required queued job can run immediately
on the calling thread; an already running job is joined before the draw or
dispatch executes. An unfinished compilation never causes a draw to be skipped.

Identical GLSL stages share one compilation, reflection result and shader module.
Both successful and failed jobs wake all dependents. Guest shader bytes are
copied before background work begins. Replacing a program's shaders advances
its cache revision, including when guest addresses are reused. In-process reset
drains workers before releasing pipelines, programs and shared shader modules.
The translator remains serialized because it has shared scratch storage;
independent shaderc compilation uses between two and eight worker threads.

`LARECOMP_SYNC_SHADERS=1` is the inline diagnostic baseline; unset or `0` enables
the safe background path. `LARECOMP_VK_PREWARM=0` disables speculative preparation
while retaining required-job waits and stage deduplication. The renderer reports
`shader waits <count> <milliseconds>` separately. An unseen variant that cannot
finish ahead of use still blocks; this is not a guarantee of zero first-use
hitches.

LAS uses a shipped shader-variant list and
launcher preparation with Skip. The list contains hashes and render state;
shader code comes from the selected player's files. This preparation and reuse
are LAS-only. ACNH and development adapters retain the shared background queue.
Vulkan preparation uses the same required-work queue, segmented guest-memory
layout and compute configuration keys as live rendering. Skipping preparation
can cause first-use waits; it does not discard required draws or dispatches.

Each preparation rebuilds its file index for the selected installation. Vulkan
reboot drains queued work, releases prepared objects once even when several
lookup entries reference them, and then clears the stage cache. Returning to
the launcher can prepare the next boot again. The enhanced import progress,
compilation cancellation and translated controls remain available. Metal takes
the upstream preparation path while retaining compute shared-memory identity;
its runtime behavior has not been tested on the Windows development host.

Run `python tools/test_shader_policy.py`, `python tools/test_shader_jobs.py`,
`python tools/test_prepared_shader_lifetime.py` and
`python tools/test_shader_snapshot.py` for policy, queue, snapshot and lookahead
regressions. With a Vulkan 1.3 GPU plus shaderc development libraries, run
`python tools/test_shader_prepare_gpu.py`: it uses the production queue and stage
cache, exercises simultaneous requests, checks a first-use compute dispatch's
output, resets the cache, and verifies that compile failures wake every waiter.
The intentional invalid-shader diagnostic in that test is expected.

## Development changes

Keep title-specific addresses, memory layouts and behavior in the adapter.
Exercise the existing tests for the subsystem being changed. Changes to saves,
installation publication or rendering also need a focused check with compatible
copied game data. Compilation alone does not qualify a new game as playable.

## Managed locations

| Launch ID | Stable installation slot | Vanilla save folder |
| --- | --- | --- |
| `existing` | `extracted` | `save` |
| `las/1.0.0` | `games/v1.0.0` | `save/v1.0.0` |
| `las/1.1.0` | `games/v1.1.0` | `save/v1.1.0` |
| `acnh/1.0.0` | `games/acnh/v1.0.0` | `save/acnh/v1.0.0` |

Locations are relative to the application's data folder. Existing LAS locations
and setting values remain valid. A custom ACNH folder uses a save namespace
containing its program ID and executable Build ID. Test runs can explicitly
override the save directory.

Legacy files directly inside these slots remain valid. New managed setup uses
`<slot>/.installations/build-<id>/content`; `.installations/current` selects the
published generation. This internal directory is never embedded in a managed
desktop shortcut or used as a save identity. Legacy numeric slot indices and
setting aliases are explicitly preserved in the adapter definitions.

The launcher inspects a selected archive and selects the matching managed slot.
The native importer checks the expected title as well as version, so two games
that both use version 1.0.0 cannot overwrite each other's slot. Generated-source
and object caches include the adapter and all modules; an incompatible SDK or
module set is rejected before compilation replaces a working library.

## Candidate installation and recovery

An exclusive OS lock covers a managed installation's import, build and publication.
All new content and build work stays in a candidate, recorded by
`.installations/pending`. Cancelling compilation or terminating the process leaves
that candidate available to **Build or resume game code**. The old installation
remains selected throughout. A failed import needs the source to be selected again;
only a complete imported candidate can resume compilation without its archive.

Before switching `current`, the publisher checks the exact module set, the
adapter's content rules, the resulting library through the host dynamic loader,
and its ABI, core ID and adapter ID. It writes a content receipt and atomically
replaces the small startup record. A crash before the switch retains the old
installation; a crash after it leaves the complete new installation selected.
Shortcut creation follows publication and can be retried independently.

Previous generations and legacy content are retained, including while another
process may still be using them. This costs additional disk space: rebuilding an
existing installation currently copies its data and verified build cache. There
is no automatic garbage collection or rollback UI yet. **Delete game files**
removes the managed slot's owned generations and legacy game files; saves, seeds,
mods and settings remain outside them. **Clear compilation cache** operates on
the pending build when there is one and keeps the installed game library.

External extracted developer folders retain their original build-only workflow
and library-pair rollback. Managed setup does not overwrite those source folders.

## Code identity and reuse

The native library ABI is now **4**. Libraries from the previous ABI need a
one-time rebuild; existing saves and launch IDs do not change. Thereafter:

- The common identity covers shared generation code, host ABI, target and flags.
- The release identity covers its module plan, reviewed instruction patches,
  adapter revision, shared generator sources and its own generator profile inputs.
- The complete title catalog, labels, paths and unrelated adapters are excluded
  from these code dependencies. Adding another title does not invalidate an
  already compatible library. LAS-only patch changes do not invalidate ACNH.
- Generation and compiler receipts still validate file contents and retain
  unchanged compiled chunks. Compiler/header/flag changes invalidate the affected
  objects. The helper checks the requested release identity before generation,
  rejecting mismatched helper/runtime packages.

These checks establish artifact compatibility. Developer qualification of a
game/version remains separate from successful installation on a player's PC.
Players do not repeat the developer's extended gameplay acceptance run.
Keeping an older installation does not make an incompatible library loadable
with a newer host ABI/code contract: that host still requires a successful rebuild.
Failure preservation keeps the older files intact for their compatible runtime.

See [adapter development](adapter-development.md) for schema checks, guarded
patches, scaffolding and missing-capability diagnostics.
The [Pokemon Legends: Z-A port](pokemon-za-port.md) remains a development
adapter with unresolved CPU and GPU requirements.

## Desktop launch

On Windows, successful setup creates a shell link on the desktop returned by
`SHGetKnownFolderPath`, including redirected/OneDrive desktops. It records the
game's stable launch ID and absolute data folder:

```
lasrecomp.exe --play --game acnh/1.0.0 --data-root "C:\Games\Recomp"
```

Existing installations can use `--create-shortcut` or the launcher's shortcut
button. A shortcut is updated only when its description identifies the same
installation; unrelated same-name files are preserved. `icon.ico` in an
installation supplies its icon when present; otherwise the runtime icon is used.
Windows 10 1903 and newer use the embedded UTF-8 process manifest for native
file paths and command-line arguments. Shell-link paths use wide APIs directly.

Tests use `LARECOMP_SHORTCUT_DIR` to keep fixture links off the real desktop.
`tools/test_game_shortcut.py` reads the resulting links through the Shell API and
checks Unicode paths, arguments, duplicate updates and preservation of unrelated
files. `tools/test_game_profiles.py` checks exact managed locations, launch IDs
and distinct save folders. These tests do not replace real import, compilation,
desktop launch and gameplay verification.
