# Animal Crossing: New Horizons port

This private development branch adapts the LASRecomp runtime to the base release
of Animal Crossing: New Horizons (program ID `01006F8002326000`). It is an active
port, not a verified playable release. No game code, extracted files, fonts,
generated C, compiled game library, or saves belong in commits.

## Module layout

| Module | Guest base | Build ID |
| --- | --- | --- |
| main | 0x7000000000 | 7fc1baff976aeca414520cb89f4616cb |
| sdk | 0x7004000000 | ad3fdfbd62a5f4b082d385ffe420d4400abb831d |
| subsdk0 | 0x7006000000 | 5a7ada8dd7447c10aa244944f64d0aff17c50539 |

The loader checks the full 32-byte IDs and rejects other versions. The SDK runs
its initializers first, followed by multimedia and game initialization. All three
modules have independent function tables (game-library ABI 2).

`tools/gen.py` defaults to the generic profile, with no LAS instruction patches,
randomizer variants, or gameplay hooks. The ACNH runtime also disables the LAS
memory probes, texture replacements, and game-specific overlay pages.

## Building and running

Use the Windows toolchain described in BUILD-windows.md. Place the three NSO
modules and main.npdm in `extracted/exefs`, then run `make gen` and `make -j4`.
The game data is read from `extracted/romfs.bin`. The current FFmpeg movie adapter
also needs `extracted/romfs/Movie/OpeningMovie.webm` from the same local data.
The executable/library filenames currently retain their inherited LAS names.

Run `build/lasrecomp.exe extracted --play` with an isolated `LARECOMP_DATA_ROOT`
and `LARECOMP_SAVE_DIR`. `ACNH_STRICT_HLE=1` and `ACNH_STRICT_IPC=1` stop at missing
host implementations instead of returning empty successful responses. These are
diagnostic controls, not a replacement for gameplay verification.

The offline NIFM adapter reports unavailable networking and supplies the required
request/event objects. Its IPC layouts follow the
[libnx NIFM client](https://github.com/switchbrew/libnx/blob/master/nx/source/services/nifm.c)
and [public definitions](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/nifm.h).

The friends adapter exposes an empty local list and local completion events.
BCAT reports unavailable networking for delivery requests. NFC discovery reports
no attached reader. Their command layouts follow the public
[friends](https://switchbrew.org/wiki/Friend_services),
[BCAT](https://switchbrew.org/wiki/BCAT_services), and
[NFC](https://switchbrew.org/wiki/NFC_services) protocol descriptions.

Shared-font calls use installed Windows fonts, with the configured language's
font first. The local `ngc` fallback handles UTF-8/UTF-16 input and basic text
checks; it does not include Nintendo's system dictionary or claim matching
online filtering. Network play and amiibo functionality are not implemented.

NVN recorders use private host arenas and compact allocation budgets, avoiding
inflated guest heap requirements and shared scratch-memory races. Completed
control-memory handles own immutable lists. Referenced lists are retained through
parent recording and execution; `CopyCommands` makes independent copies. Pool
destruction, explicit finalization and handle reuse retire unused lists.
Allocation accounting and callbacks remain visible to the game. Fence commands
use the sync-object argument and wait for their actual GPU submission, including
submissions that have not yet reached the queue.

Thread affinity records both the preferred core and allowed mask. Reported guest
cores follow that mask independently of Windows scheduling; ACNH uses these
identities to index progress slots in its three-worker barriers.

## Verification gate

Completed: extraction and module inventory; generation of 242,778 functions;
sorted, aligned, nonoverlapping function tables; strong import resolution;
host save-file write/flush/close/reopen cycles (128 iterations), descriptor
reuse, and buffer/path bounds. `tools/test_acnh_save.c` invokes the production
filesystem handlers and native Windows positional I/O with scratch files.

Eight synthetic jump-table tests cover cross-fragment targets and guards, hoisted
table bases, unrelated object reads, index copies surviving function calls,
conditional compares, and shared flags across separate control-flow paths.
Command-storage tests exercise independent recorders, 1,000 reuse cycles,
allocation callbacks, deep copies, and retained nested lists. Sync tests cover
polling, delayed publication, GPU completion, timeout and reuse. Affinity tests
cover distinct worker slots and supported mask modes. The local text filter has
ABI, UTF-16, masking, count-bound, and destruction checks.

The current boot passes three-module initialization, window/input setup,
font loading, friends/BCAT/NFC setup, and the initial graphics-resource recording.
It has submitted more than 4,000 frames in a one-minute run. Deferred conditional
rendering and counter writes now preserve visible dialogue; a keyboard press
advances that dialogue. Offscreen captures contain airport scene geometry and
loading artwork. Final scene composition and characters are still incomplete.
This is startup progress only; it is not evidence of stable gameplay or audio.

Still required: complete scene output, intro and island controls, graphics and
audio correctness, game-managed saving and reopening, day rollover, repeated
starts, and an extended gameplay run. Update and DLC compatibility remain
separate follow-up work after the base game passes these checks.
