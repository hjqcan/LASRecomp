# Animal Crossing: New Horizons port

The shared enhanced runtime supports Animal Crossing: New Horizons 1.0.0
(program ID `01006F8002326000`) as a Windows beta. Normal archive setup is enabled
for this exact base release after the local acceptance checks recorded below.
Updates and DLC are not qualified. No game code, extracted files, fonts,
generated C, compiled game library, or saves belong in commits.

## Module layout

| Module | Guest base | Build ID |
| --- | --- | --- |
| main | 0x7000000000 | 7fc1baff976aeca414520cb89f4616cb |
| sdk | 0x7004000000 | ad3fdfbd62a5f4b082d385ffe420d4400abb831d |
| subsdk0 | 0x7006000000 | 5a7ada8dd7447c10aa244944f64d0aff17c50539 |

The loader checks the full 32-byte IDs and rejects other versions. The SDK runs
its initializers first, followed by multimedia and game initialization. All three
modules have independent function tables (game-library ABI 4, including a release
code-identity check). The runtime also
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
Both path-only and client-reader movie source overloads open local files;
failed source changes clear old tracks and packets. `tools/test_movie_source.c`
uses a synthetic PCM/WAV fixture to check both overloads and error cleanup.
The public movie buffer layout supports the game's inline base/size accessors.
Copy-mode output preserves the caller's allocation: PCM is copied into it,
and NV12 format 4 strips the host decoder's row and plane padding. Replacing
the buffer's base pointer left the game's upload allocation empty. The output
size and alignment query writes two 32-bit integers. Synthetic tests check
the retained pointer, tight NV12 rows, undersized-buffer rejection and canaries.
A managed-installation replay rendered the real opening movie correctly
and reached the island after decoder EOS. That early replay showed coastline
artifacts and did not complete a game-managed save. Later save recovery and
graphics checks are recorded below.
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
Legacy splitter updates retain unchanged destinations, route each voice channel
to its destination stride, and order submixes across fan-out connections. The
protocol and routing semantics follow the public
[splitter state](https://git.axenov.dev/Museum/ryujinx/src/commit/f3b0b4831c323a20393aa0388f947317354372b7/Ryujinx.Audio/Renderer/Server/Splitter/SplitterDestination.cs)
and [command generator](https://source.hodakov.me/hdkv/yuzu/src/commit/d379fdd5b689e2cec95476bf1336e7b55d348959/src/audio_core/command_generator.cpp).
`tools/test_audio_splitter.c` covers partial updates, padding, invalid packets
and volume ramps. With `-DTEST_AUDIO_RENDERER` and the runtime build headers,
it additionally feeds synthetic PCM through the actual voice/submix mixer,
checking stereo destination strides and existing direct routes. Local captures
from the game validate the packet layout but are not repository fixtures.
Audio effect 3 implements Q14 delay gains, persistent delay lines, low-pass
feedback, mono/stereo/quad/5.1 routing and the legacy surround-channel mapping.
The equations follow [Ryujinx's delay implementation](https://git.axenov.dev/Museum/ryujinx/src/commit/dde9bb5c69d2e1a70df82af8accd3d01fb94b78d/Ryujinx.Audio/Renderer/Dsp/Command/DelayCommand.cs).
Impulse tests cover packet boundaries, bypass, aliasing, feedback, invalid
descriptors and the game's observed 48 kHz / 400 ms parameter set. The production
mixer test checks ordered biquad/delay processing and effect-slot retirement.
Audio effect 5 implements 3D room reverb, early reflections, filtered feedback,
two decay stages, persistent delay lines and mono/stereo/quad/5.1 outputs. It
follows [Ryujinx's reverb-3D implementation](https://git.axenov.dev/Museum/ryujinx/src/commit/dde9bb5c69d2e1a70df82af8accd3d01fb94b78d/Ryujinx.Audio/Renderer/Dsp/Command/Reverb3dCommand.cs).
Local differential tests run the original C# implementation and compare a full
second of every channel, including a mid-stream parameter change; all compared
samples matched exactly in the Windows test. Tests retain selected numerical
checkpoints for CI. The REV11 channel remap applies to 5.1 only; the reference's
fixed six-element index arrays also remap inactive quad entries, so the quad
comparison uses its legacy mapping. Runtime integration checks reverb-setting
bypass and effect retirement.

Audio effect 2 exchanges signed integer PCM through the game's auxiliary send
and return rings. The runtime retains attached audio memory-pool ranges, checks
each ring against them, transfers all planar channels, and publishes frame
offsets once per block. The host mixer already holds PCM magnitudes in floats;
auxiliary exchange only converts their numeric representation to/from int32.
Normalization to the device's [-1,1] range happens once, at the final sink.
Disabled effects reset DSP offsets and copy
their input while preserving CPU headers and loss counters. Layout and protocol
follow the public [auxiliary-buffer implementation](https://git.axenov.dev/Museum/ryujinx/src/commit/dde9bb5c69d2e1a70df82af8accd3d01fb94b78d/Ryujinx.Audio/Renderer/Dsp/Command/AuxiliaryBufferCommand.cs).
Tests cover ring wrapping, PCM conversion, frame offsets, pool attach/detach,
canaries, invalid ranges and the production mixer's effect dispatch. A full
decoder/auxiliary/sink regression checks audible amplitude: 8192 PCM units must
remain 0.25 at the device. It also applies a guest limiter before returning audio.
This catches the extra 32768 conversion that made island audio nearly silent
while the movie's separate output remained audible. Actual
island audio still needs listening and capture validation. This routing
implementation covers the legacy destination format, not REV12 biquads.

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
Windows guest threads reserve 256 MB of host stack for recompiled recursion but
commit pages on demand. Passing that size through winpthreads committed the
whole stack for each thread; an airport boot committed about 18 GB in total.
The CRT thread entry point uses the documented
[reservation flag](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/beginthread-beginthreadex).
A native test creates sixteen threads, checks committed-memory growth, grows a
stack beyond its initial commit, and verifies pthread locking and thread joins.

First-use shader compilation waits for the shader instead of dropping draws
that initialize resources only once. Vulkan can render individual depth slices
of a 3D image using compatible 2D views; ACNH uses this for its color lookup
volume. Maxwell LDC instructions sign-extend their 16-bit byte displacement,
including negative bone-matrix offsets and overlapping 64-bit destinations.
These fixes restore the airport background, colors and animated characters.
Full virtual-pool mappings alias their physical pool's storage, including
nonzero physical offsets. ACNH maps its transient texture pool this way before
creating movie textures. Returning success without installing the mapping left
texture addresses as low offsets and crashed when video uploaded its first
frame. Sparse mappings and rebinding an existing alias are rejected; they are
not implemented by the current GPU-address-equals-host-address model.
`tools/test_nvn_pool_mapping.c` checks alias visibility, bounds and rejected
mapping forms without loading game code.

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

Windows samples in the island scene identified repeated x87 environment saves
and restores in the generated module's CRT rounding functions. The x86-64 SIMD
lowering now changes the SSE MXCSR rounding bits directly, leaving exception
flags, masks, denormal controls and x87 state intact. Other hosts keep the C
fenv path. Guest FZ handling, NaNs, directed rounding and tiny-result checks are
unchanged. `tools/test_simd_rounding.c` checks host restoration and independent
threads; a Cortex-A57 differential run passed 95,988 cases across 8,000 encodings.
With the packaged Zig compiler, a one-million-operation FZ microbenchmark took
0.197 seconds before and 0.0063 seconds after, with identical result bits. This
microbenchmark is not an in-game frame-rate result; the rebuilt game still needs
that measurement.

Compute LDS/STS instructions use workgroup storage on Vulkan and Metal. NVN's
control block supplies its byte size at `0x754`, after the three local dimensions.
Tracing the original parser and compute setup confirmed that this field reaches
the QMD shared-memory size (offset `0x44`, low 18 bits), whose layout is also
documented in [NVIDIA's Maxwell QMD definitions](https://github.com/NVIDIA/open-gpu-doc/blob/master/classes/compute/clb1c0qmd.h).
Byte and halfword stores use atomic compare/exchange to preserve simultaneous
writes to neighboring bytes. Compute cache keys include these configuration
values. `tools/test_shader_shared.py` executes translated bodies with concurrent
workgroups, covers signed loads, 64/128-bit vectors, overlapping registers and
bounds, and optionally compiles every GLSL shader with `GLSLC`. Metal source is
checked for workgroup storage but has not been executed on a Mac in this run.

The JPEG encoder's error recovery uses guest `setjmp`/`longjmp`. The generator
recognizes named entry points and AArch64 PLT calls and keeps a host checkpoint
in each caller. After the guest restores its own registers, dispatch unwinds the
host stack and resumes at the original call's continuation. Checkpoints survive
tail transitions between generated fragments and are retired on return, thread
exit or reboot. No JPEG return values or game progress flags are replaced.
`tools/test_guest_jump.py` compiles synthetic callers and checks nested unwinds,
same-PC frames with different stacks, thread isolation, reused buffers, RET-based
longjmp implementations, and calls at fragment boundaries.
The observed portrait failure returns `-65`; the game's caller then reduces JPEG
quality and retries. The host checkpoint preserves this guest return value rather
than replacing it with the host `setjmp` result.
A rebuilt managed installation launched through its existing Windows desktop
shortcut passed the previously failing appearance-confirmation step at frame
5,833, then continued to photo registration. This used the same 2560x1440 window
and native 1080p render override as the failed run. Encoder-entry logging sits
behind the dispatch cache, so that log does not count every quality retry.

Vulkan screen attachments include ACNH's uncompressed R8/R16F/RG16F buffers,
not just textures with the compression hint. A captured 720p frame had a 960x540
color attachment beside 640x360 depth; the clipped image produced a rectangular
lighting boundary. `tools/test_vk_texture_scale.sh` checks matched physical
extents, original dimensions for sampled images, and content-preserving late
attachment classification. New color attachments also start zeroed, like depth,
so unwritten regions cannot expose undefined VRAM. The airport scene and photo
registration have passed at the desktop resolution above; a scaled island
scene and the full save/reopen flow still require validation.

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
The same live script accepts `frame:shot:name` to save `build/name.png` from
the presented window, including menus, on Vulkan or Metal. Names use letters,
digits, underscores and hyphens.

Synthetic touchscreen tests cover poisoned buffers, 1,024 samples, capacity
bounds, surrounding canaries and reboot. Native execution of translated synthetic
LDC shader bodies covers negative and boundary offsets plus overlapping load
destinations. Shared-runtime LAS startup exposed and fixed an unfenced-sync
polling regression; delayed fence publication and GPU completion still wait.

The 2026-10-17 desktop audio correction preserves PCM16 magnitudes through the
auxiliary send/return path and normalizes only at the final device sink. Its
production-renderer regression passes, and the player confirmed audible in-game
music/effects after restarting the updated desktop program.

Two fragment programs used `TMML.LOD`, which previously failed translation.
The Vulkan translator now supports bound 1D/2D/3D/cube and supported array LOD
queries, including their reversed component order and signed fixed-point result.
The encoding and result semantics were checked against
[envytools](https://github.com/envytools/envytools/blob/master/envydis/gm107.c)
and [Ryujinx's translator](https://github.com/kotx/Ryujinx/blob/master/src/Ryujinx.Graphics.Shader/Instructions/InstEmitTexture.cs).
Synthetic tests execute the generated query/result code and compile complete
GLSL for seven texture types. That instruction change did not resolve the
recorded post-tent sunset brightness; the later feedback-snapshot correction
and scene comparison are described below.

The first player save reached the NookPhone and wrote all ten header/data files,
then crashed because `fsp-srv` command 1200 reported success without returning
`IMultiCommitManager`. That interface now returns a real object with Add (1)
and Commit (2), matching the [service implementation reference](https://github.com/kotx/Ryujinx/blob/master/src/Ryujinx.HLE/HOS/Services/Fs/IMultiCommitManager.cs).
The IPC dispatcher resolves input interfaces from copied handles or the calling
domain. Managers retain their mounts, publish their pending files, return write
errors, and take backups only after all member commits succeed. Tests send real
session/domain messages through the production codec, commit ten nested files,
close input handles before committing, reject invalid objects, and retry a
failed publication. Publication retains the existing per-file rename semantics;
it does not promise a power-loss-atomic transaction across multiple filesystems.

The failed first save's original files were preserved separately. A recovery
copy passed all 24 checksums for the five 1.0.0 data files and their matching
version headers. The game loaded that copy at the player's tent with the NookPhone,
completed two automatic commits and a manual Save and End, and displayed its
successful-save message. The new files again passed all checksums. After normal
process exit, the verified copy replaced the managed save while retaining both
original backups; a fresh desktop launch with default shader preparation loaded
it again and completed two further automatic commits.

Strict Vulkan validation of that recovery run identified an oversized global-memory
descriptor: the imported heap exceeded this Intel GPU's `maxStorageBufferRange`.
The Vulkan backend now splits heap bindings at the device's aligned range limit,
and shader LDG/STG selects a segment for each word. Generated shader tests cover
signed sub-word loads, masked stores, vectors crossing a segment boundary, the
last partial segment, and compute fast-copy routing. The range validation error
no longer occurred. The initial longer validation runs were inconclusive: the local MSYS2
validation layer stalled during semaphore queries and later crashed inside its
worker/mutex code. Those runs do not establish a clean Vulkan validation result.
Normal runs completed the recovery flow above without those failures.

On 2026-10-18 the same rendering/save fixes (`6097a68`) were tested with the
official LunarG SDK 1.4.363.0 Windows validation layer. Its component checksum
and LunarG signature were verified, and the layer was loaded locally through
`VK_LAYER_PATH`, without installing a system layer or changing a driver.
The desktop shortcut ran an isolated copy of the save with
`LARECOMP_VK_VALIDATION_STOP=1`, strict HLE/IPC checks and default shader
preparation. The 616.9-second run loaded the island, walked in both directions,
entered the tent, opened and closed the inventory, left the tent, and completed
the game's Save and End flow. It made three successful multi-commits, exited
normally, and the resulting save passed all 24 data checksums. No Vulkan errors
were reported. The layer warned about an unused fragment color output during
depth-only draws. This covers those scenes and operations, not every rendering
path, synchronization validation, or performance without diagnostic overhead.

For reproducible lighting/day-rollover diagnostics, `LARECOMP_TEST_TIME` accepts
a Unix timestamp in 2000-2100 only with an explicit `LARECOMP_SAVE_DIR`. Use a
separate copy of the save for these tests. It advances the guest clock from that
instant without changing the host clock. Normal launches retain host time.
With that isolated save, `LARECOMP_SHADER_OVERRIDE_DIR` can also replace a
translated graphics shader from `<hex-code-address>_fs.glsl` or `_vs.glsl` for translator
comparisons. Overrides must preserve the translated descriptor bindings and
interface locations; they are ignored without an explicit save directory.
Keep captured game shaders outside the repository.
`LARECOMP_DEBUG_TEXTURE_MIPS=<hex-guest-address>` dumps the matching single-layer
R11G11B10 texture's raw mip levels on the next debug frame, once per process and
only with an explicit save directory. This permits inspecting HDR values without
the clipping applied by an ordinary PNG screenshot.

The sunset lighting investigation found that Vulkan feedback snapshots retained
only mip zero. Environment filtering reads LOD 2/3/4 from the same texture while
writing a later level, so these snapshots redirected each read to its sharp base
image. Snapshots now retain the complete mip/layer chain and each descriptor's
original view range, format and swizzle. A production-code mock-GPU regression
checks the LOD result, snapshot refresh, descriptor views, array layers and volume
depth. The old production snapshot fails that regression; the repaired version
passes. A subsequent isolated 17:30 launch, without shader overrides or disabled
lighting effects, showed normal matte skin, clothing and foliage on the loaded
island and in the title scene. The HDR level-four red peak fell from about 54 to
2.4 while the sharp sun remained in level zero. These captures establish the
sunset-feedback regression, not full graphics compatibility.

Extended acceptance exposed a separate timing issue: ACNH requests NVN present
interval 2, but the runtime had stored that value without waiting on it. Fast
indoor scenes consequently presented at 80-90 FPS. Positive intervals on generic
adapters now pace presentation against the guest's 60 Hz display; interval 2
requests 30 FPS. The interval interpretation is also documented by
[FPSLocker](https://github.com/masagrator/FPSLocker#usage).
The deadline includes rendering time and starts again from the actual present
time after a stall or a late wake, without catch-up bursts. Each window keeps
its own deadline. LAS retains its existing VI-event/frame-mode pacing.
`tools/test_nvn_present_interval.py` runs the production NVN entry points with a
controlled clock, covering intervals, changes, rendering time, stalls, late
wakes, clock rollback and the LAS path. The old presentation path fails this
regression. A subsequent 568.8-second desktop run used the correction, loaded
the saved island, walked outside, opened and closed the inventory, entered and
left the tent, and completed Save and End. Indoor and outdoor presentation
remained about 29.7-29.8 FPS. All four save commits completed, the process exited
normally, and the files passed all 24 checksums.

## Windows beta acceptance, 2026-10-18

The packaged frozen generator and bundled Zig compiler rebuilt all 729 ACNH
objects, validated the completed installation and published its desktop
shortcut. The tests used Windows 11 and an Intel Arc Pro 140T Vulkan GPU, strict
HLE/IPC diagnostics, default shader preparation and independent save copies.

Before the interval correction, a 48.2-minute desktop session loaded an existing
island, exercised outdoor movement, crossed 05:00 into the next game day,
completed Nook's morning broadcast, rendered the new rain weather, read mail,
entered and left the player's tent and resident services, advanced NPC dialogue,
and entered the airport. It completed 15 multi-commits including manual Save and
End, exited normally and passed all 24 save checksums. A later desktop launch
read that progress successfully. This run established scene/save stability and
exposed the presentation issue; the corrected pacing was checked separately as
described above.

Real complete imports used locally supplied NSZ archives. NSP, NSZ, XCI and XCZ
parsing, automatic identity selection, encrypted sections, counter boundaries
and rejection of mixed or unsupported modules also have synthetic regression
coverage. The ACNH import regression now succeeds without a development
override. These checks do not certify every commercial archive or every game
scene. Full-game progression, additional hardware/platforms, updates and DLC
remain further compatibility work. Online play and amiibo are unavailable.

## Extracted-folder mounting correction after Beta 5.3

Beta 5.3's raw-storage opener still used LAS's `region_common` directory as a
folder sentinel. A complete ACNH folder could pass the adapter's installation
checks, then fail at its first RomFS mount if `romfs.bin` could not be opened.
The real NSZ acceptance imports retained that image and did not expose this
folder-only path. This failure occurs before game rendering and does not by
itself indicate an NVIDIA or Vulkan problem.

The corrected opener uses adapter content rules for complete folders, preserves
image preference for image-backed adapters and keeps LAS's folder-first behavior.
Failure messages include both the folder validation reason and image-open error.
The launcher also displays the selected adapter's version instead of LAS's
version enum, which previously labeled ACNH as `vunsupported`.

`tools/test_romfs_source.py` exercises production mounting and data reads with
eight synthetic cases. The former implementation reproduces the reported fatal
message; the correction passes all eight. A read-only check of the local ACNH
folder, without an adjacent image, mounted all 66,790 files and matched 32 sampled
payload reads. These data-layer checks do not constitute a new NVIDIA gameplay
acceptance run.
