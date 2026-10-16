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

## Verification gate

Completed: extraction and module inventory; generation of 223,103 functions;
sorted, aligned, nonoverlapping function tables; strong import resolution;
host save-file write/flush/close/reopen cycles (128 iterations), descriptor
reuse, and buffer/path bounds. `tools/test_acnh_save.c` invokes the production
filesystem handlers and native Windows positional I/O with scratch files.

Still required: first real game frame, intro and island controls, graphics and
audio correctness, game-managed saving and reopening, day rollover, repeated
starts, and an extended gameplay run. Update and DLC compatibility remain
separate follow-up work after the base game passes these checks.
