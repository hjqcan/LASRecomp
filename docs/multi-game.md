# Shared game installations

The enhanced runtime reads reviewed `adapters/*/adapter.json` definitions through
`tools/game_catalog.py` and generates its native catalog and installation list.
It identifies each reviewed game/version by every module's full 32-byte
Build ID. LAS 1.0.0 and 1.1.0 use two modules; ACNH 1.0.0 uses three. ACNH is
still marked as development while the gameplay and save gates in
[ACNH-port.md](ACNH-port.md) remain open.

Archive inspection accepts NSP, NSZ, XCI and XCZ. The default is automatic game
identification. Readable content metadata provides a preliminary identity;
metadata-free NCZ packages are inspected through their executable modules.
Filenames do not select the game or version. Import validates module identities,
base/update roles and RomFS structure. The launcher stages this content, builds
and validates the exact library, then publishes the installation. The standalone
import command below extracts data only; it does not compile or create shortcuts.
The development override is intended for local port testing:

```
python tools/import_game.py --base /path/to/game.nsz --inspect
python tools/import_game.py --base /path/to/game.nsz --output /path/to/install --allow-development
```

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
hitches. Metal's scheduling is unchanged.

Run `python tools/test_shader_policy.py`, `python tools/test_shader_jobs.py` and
`python tools/test_shader_snapshot.py` for policy, queue, snapshot and lookahead
regressions. With a Vulkan 1.3 GPU plus shaderc development libraries, run
`python tools/test_shader_prepare_gpu.py`: it uses the production queue and stage
cache, exercises simultaneous requests, checks a first-use compute dispatch's
output, resets the cache, and verifies that compile failures wake every waiter.
The intentional invalid-shader diagnostic in that test is expected.

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
The [Pokemon Legends: Z-A probe](pokemon-za-port.md) records third-title archive
and generation results, including unresolved CPU requirements; it is not a
playable catalog entry.

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

Before the ABI-4 installation refactor, the Windows development package imported a metadata-free ACNH base NSZ,
validated all three modules, and built them using its bundled frozen generator
and Zig compiler, with no external Python/compiler override. Generation produced
523 main, 102 SDK and 100 multimedia chunks; compilation produced 729 objects
including tables and the library entry. A system sleep interrupted the first
attempt with a lost Vulkan device. A subsequent setup reused 559 verified
objects, compiled the remaining 170 and published the library and shortcut.
The resulting managed installation boots through the shortcut's stable launch
arguments. A Unicode data-folder alias also passes the real CLI identity check
and shell-link creation. These observations are development evidence; ACNH
remains gated until game saving, reopening and island gameplay pass.

The 2026-10-17 Windows development run passed photo registration, placed all
three tents, collected the campfire materials and entered the night gathering.
Before the first save, the Intel Arc Pro 140T Vulkan queue stopped completing
submissions. The old runtime logged repeated 10-second GPU wait timeouts but
continued executing; the host was eventually restarted manually after its
display froze. No save files existed for this test island, so save/reopen
acceptance remains unproven. The retained log does not establish the initial
cause of the GPU stall.

GPU submission waits now terminate with a failure status on timeout, before
callers can recycle in-flight resources. This path bypasses the normal renderer
shutdown, whose device-idle wait is unbounded. The injected timeout regression
uses no GPU and verifies that neither caller continuation nor GPU cleanup runs;
it does not establish that the original driver hang is fixed.

An independent CPU thread also monitors submission, presentation, image acquire,
and device-idle calls. If a driver call does not return within 20 seconds of
awake time, it records the operation and submission number and terminates the
process without querying or cleaning up the GPU. Windows sleep and hibernation
are excluded from this clock. Window-image acquisition uses a finite wait and
minimized windows are skipped. Mocked blocking calls test cancellation, thread
and slot reuse, simultaneous calls, nesting and termination without exit handlers;
no real driver hang is deliberately induced by these tests.

A later player freeze retained a thread dump showing the main thread in
`nvnSyncWait` / `renderer_wait_seq_timeout`, an alternate semaphore wait that
had bypassed the watchdog. That path now also caps each driver wait at ten
seconds and monitors both semaphore waiting and counter queries. Ordinary
finite guest timeouts still return the NVN timeout result. An indefinite guest
wait that exhausts the ten-second GPU limit terminates without cleanup. The
regression executes these production paths with a mocked driver, including
non-returning wait/query calls. This closes the observed protection gap; the
underlying rendering/submission stall and sunset lighting remain under investigation.

Recovery boot then read all five saved data files and stopped at SyncWait
submission 9. A CPU-only reproduction exposed a sequence error at the first
eight-slot ring wrap: the next slot retained `main_begun` from its previous
generation, so an empty queue fence named submission 9 while the empty submit
path produced no submission. Fence generation now checks the slot's sequence,
and slot initialization/begun publication/ring advancement share its lock. The
test fails against the old implementation and passes across repeated wraps,
pending uploads, main buffers, and empty fences. The previous indefinite wait
could therefore wait for work that did not exist; this is separate from proving
every reported host stall has the same cause.
