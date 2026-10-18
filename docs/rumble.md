# Controller rumble

HID commands 201/206 pass the recompiled SDK's vibration values to the existing
SDL gamepad. No new game events are synthesized. The SDK vibration graph and
effect decoder remain recompiled.

The input bridge publishes values under a spinlock; the UI thread that owns
the gamepad sends SDL_RumbleGamepad calls. Commands 202, 204 and 205 provide
requested-state readback and permission; 211 reports recognized virtual devices
mounted. Device initialization retains the existing HLE shortcut.

## Mapping and lifetime

The four-float value contains low amplitude/frequency and high amplitude/frequency.
Finite amplitudes are clamped to [0,1], rounded to [0,65535], and mapped to SDL's
low/high motors. Each motor receives the maximum across the two Switch sides,
so duplicated left/right effects do not double their strength. Invalid amplitudes
produce zero. Only player 1 is supported, matching the existing input bridge.

This approximates HD Rumble: SDL_RumbleGamepad cannot preserve the original
frequencies in Hz or independently reproduce both Joy-Cons' spatial effects.
Strength has not been calibrated against original hardware.

Guest values are latched and carry no duration. The host renews a 250 ms effect
every 100 ms while frames progress. Menu/focus changes, disconnect, reset and
normal exit clear/stop output. A render stall longer than 250 ms clears values;
if the UI itself stalls, SDL's outstanding effect expires. These timings are
host watchdog policy, not durations decoded from the game.

## Required timer fix

The game's vibration graph uses a 5 ms TimerEvent. The kernel must return
WaitProcessWideKeyAtomic timeout without owning the mutex: the SDK acquires it
after timeout. Acquiring it in both places deadlocks the update thread. This
behavior is already present on beta_5 (8a8f74f); no separate kernel PR is required.

## Tests

From an MSYS2 UCRT64 shell at the repository root:

```sh
clang -O2 $(pkg-config --cflags sdl3) tools/test_rumble.c -o build/test_rumble.exe
build/test_rumble.exe
clang -O2 $(pkg-config --cflags sdl3) tools/test_sdl_rumble.c $(pkg-config --libs sdl3) -o build/test_sdl_rumble.exe
build/test_sdl_rumble.exe
```

The input/HID regression compiles the real bridge with a fake SDL sink and tests
malformed requests, amplitude mapping/mixing, readback, permission, renewal,
stop/stall, focus/menu, reconnect, unsupported output and reset. The SDL test
uses a virtual controller to verify amplitude, duration expiry and immediate
stop; it never vibrates physical controllers. The kernel regression verifies
timeout ownership and successful/contended wakeups. Assertions stay enabled
under optimized builds.

Validated on Windows x86-64/Vulkan with an Xbox controller. The user confirmed
in-game rumble with a wired connection. Wireless did not vibrate in this setup;
standalone SDL and XInput tests also failed to produce perceptible vibration,
so its cause remains unresolved. Metal and Linux were not built or tested.

## References

- https://wiki.libsdl.org/SDL3/SDL_RumbleGamepad
- https://github.com/switchbrew/libnx/blob/master/nx/source/services/hid.c
- https://github.com/switchbrew/libnx/blob/master/nx/include/switch/services/hid.h
- https://github.com/switchbrew/libnx/blob/master/nx/source/kernel/condvar.c

libnx is independent homebrew documentation, not official Nintendo SDK material.
