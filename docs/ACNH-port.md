# Animal Crossing: New Horizons port

The shared enhanced runtime is being adapted to the base release
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
modules have independent function tables (game-library ABI 3). The runtime also
loads the reviewed LAS 1.0.0 and 1.1.0 adapters from the same catalog.

`tools/gen_game.py` selects the generic profile for ACNH, with no LAS instruction
patches, randomizer variants, or gameplay hooks. Direct `tools/gen.py` use keeps
its LAS default for existing workflows. The ACNH runtime disables the LAS
memory probes, texture replacements, and game-specific overlay pages.

## Building and running

Use the Windows toolchain described in BUILD-windows.md. Place the three NSO
modules and main.npdm in the game folder's `exefs`, then run
`make GAME_DIR=/path/to/game GEN=generated-acnh gen` and
`make GAME_DIR=/path/to/game GEN=generated-acnh -j4`.
The game data is read from that folder's `romfs.bin`. The FFmpeg movie adapter
also needs `romfs/Movie/OpeningMovie.webm` from the same local data.
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

Audio effect 6 supports the legacy signed Q14 biquad parameters, per-channel
state, reset and disabled-effect copies. Its parameter layout follows the
[published reverse-engineered ABI](https://git.axenov.dev/Museum/ryujinx/src/commit/6aef27166101a0f733f9bfdfeb268fdc45048550/Ryujinx.Audio.Renderer/Parameter/Effect/BiquadFilterEffectParameter.cs).
Impulse-response tests check continuity across renderer packets and bounds.
Auxiliary-buffer, delay, reverb-3D and splitter support are still incomplete.

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

First-use shader compilation waits for the shader instead of dropping draws
that initialize resources only once. Vulkan can render individual depth slices
of a 3D image using compatible 2D views; ACNH uses this for its color lookup
volume. Maxwell LDC instructions sign-extend their 16-bit byte displacement,
including negative bone-matrix offsets and overlapping 64-bit destinations.
These fixes restore the airport background, colors and animated characters.

Docked touchscreen reads return a valid empty sample. Returning no sample from
an unpopulated HID ring left stale stack data in the game's newest touch state.
Its touch-processing loop then subtracted 15 from unrelated memory, including
the high word of an audio-config pointer. A Windows hardware write watch traced
that write to the touch loop. The sample layout follows
[libnx's HID definitions](https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/hid.h).

The time service returns the host's local zone through both explicit-rule and
current-rule conversion calls. Opaque rules produced by this service support
`Host/Local`, `UTC` and `Etc/UTC`; arbitrary named zones are not implemented.
Both calendar-to-POSIX commands use the documented command IDs, validate buffer
sizes, and preserve skipped/ambiguous hours at daylight-saving transitions.
The IPC layouts follow the [libnx time client](https://github.com/switchbrew/libnx/blob/master/nx/source/services/time.c).
Tests cover local/UTC round trips, a leap-day midnight boundary, dates beyond
2038, invalid calendar dates, daylight-saving gaps/folds and output canaries.

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
font loading and friends/BCAT/NFC setup. Airport scenery and both characters
render, animate and display Chinese dialogue. Real keyboard input advances the
intro, and scripted controller input reaches the game's own naming keyboard.
A ten-minute run submitted more than 22,000 frames without the previously
observed pointer corruption or a crash. That run remained in the intro/naming
flow; it does not prove island gameplay, audio correctness or save stability.

For checkpoint-driven regression, `LARECOMP_INPUT_FILE` reads the same
`frame:BUTTON:duration,...` format as `LARECOMP_INPUT` from a scratch file every
30 presented frames. Commands can be appended for future frames after inspecting
the game capture. `LARECOMP_SCREENSHOT_EVERY` and `LARECOMP_SCREENSHOT_FROM`
capture game images without depending on desktop focus. Cold startup timing is
not fixed, so input should be scheduled after observing the target screen.

Synthetic touchscreen tests cover poisoned buffers, 1,024 samples, capacity
bounds, surrounding canaries and reboot. Native execution of translated synthetic
LDC shader bodies covers negative and boundary offsets plus overlapping load
destinations. Shared-runtime LAS startup exposed and fixed an unfenced-sync
polling regression; delayed fence publication and GPU completion still wait.

Still required: complete intro and island controls, broader graphics and
audio correctness, game-managed saving and reopening, day rollover, repeated
starts, and an extended gameplay run. Update and DLC compatibility remain
separate follow-up work after the base game passes these checks.
