# Building LAS Recomp on Windows (x86-64, and ARM64 experimental)

Status: part of main. Builds and runs the game on Vulkan (with your own game files); `make dist` packages the
release zip.

## One-time setup
Clone with LF line endings to a short path (deep folders hit Windows' path length limit), e.g.:

    git clone -c core.autocrlf=false https://github.com/peiikai/LASRecomp.git C:\dev\lasrecomp

(The setup script also sets `core.autocrlf=false`, but files already checked out keep CRLF until they are checked
out again, and the bash tools break on CRLF.)

Then run in PowerShell from the repo root:

    powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1

This installs MSYS2 (UCRT64: clang, lld, make, SDL3, FFmpeg, Vulkan loader + headers, shaderc, Vulkan Memory
Allocator, validation layers), Python 3.12 for the tools (`tools\venv`: unicorn, capstone, lz4) and the randomizer
(`third_party\LAS-Randomizer\venv`) and switches the clone to LF line endings.

## Build
From an MSYS2 UCRT64 shell, or Git Bash with MSYS2 first on PATH:

    export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
    make gen        # once: recompile the game from extracted/exefs (your own game files; see the start screen)
    make -j         # -> build/lasrecomp.exe and the game code library build/lasgame.dll

Without `generated/` only the app is built: the start screen builds the game code itself when you set up the game
files (as for players), given the generator and the compiler (`make gen-helper`, `tools/fetch_zig.sh`).
`make runtime-objs` compiles the runtime only (quick check).

Run from a shell that has `C:\msys64\ucrt64\bin` (ARM64: `C:\msys64\clangarm64\bin`) on PATH (SDL3, FFmpeg,
Vulkan, shaderc DLLs).

## Package
    make deps-windows   # once: the LGPL FFmpeg the zip ships (build/deps-win; MSYS2's own FFmpeg is GPL)
    make rando-helper   # the seed generator without Python (build/helper/rando-gen)
    make gen-helper     # the game code generator without Python (build/helper/lasgen)
    tools/fetch_zig.sh  # the compiler the start screen builds the game code with (build/zig)
    make dist           # -> build/dist/LAS Recomp-<version>-windows.zip (no game code inside)

The zip's `LAS Recomp/` folder holds `lasrecomp.exe` (icon from `docs/icon/icon.ico`, via windres),
`rando-gen/` and the MSYS2 DLLs. No installer: game files, saves, seeds and settings are kept next to the exe, so
the folder must be writable (the app says so and quits if it isn't, e.g. under Program Files). A source checkout
with `extracted/` in the starting folder keeps using that folder instead.

## Windows on ARM (experimental)
Builds natively on ARM64 Windows. The same `tools\setup_windows.ps1` sees ARM64 and sets up MSYS2 CLANGARM64
instead of UCRT64, plus an x64 Python 3.12 in `%LOCALAPPDATA%\Programs\Python\Python312-x64` (unicorn and
capstone have no ARM64 Windows wheels, so the Python tools run under Windows' x64 emulation).

Build from an MSYS2 CLANGARM64 shell, or with `/c/msys64/clangarm64/bin` first on PATH. The steps are the same as
above, except that the helpers need that Python passed in (there is no `py` launcher):

    PY64=/c/Users/<you>/AppData/Local/Programs/Python/Python312-x64/python.exe
    make deps-windows
    make rando-helper RANDO_PY="$PY64"
    make gen-helper RANDO_PY="$PY64"
    tools/fetch_zig.sh  # (zig for aarch64-windows)
    make dist           # -> build/dist/LAS Recomp-<version>-windows-arm64.zip

Everything in the zip is ARM64 except the two helpers (`lasgen`, `rando-gen`), which are x64. AArch64 FP/SIMD
instructions stay inline assembly (`--simd asm`), as on the Mac. The ARM64 zip is not part of the normal releases.
It is built by `.github/workflows/experimental.yml` for the Experimental builds page.

Running it needs a Vulkan 1.3 driver (on Snapdragon X, Qualcomm's). To test without one, Mesa's lavapipe (a CPU
Vulkan driver) for ARM64 works: set `VK_DRIVER_FILES=<its folder>\lvp_icd.aarch64.json`. The Vulkan loader ignores
driver entries under HKCU. Lavapipe is slow, so keep the window minimized while the start screen builds the game code.

## Differences from macOS
- Guest window 0x70_0000_0000..0x73_2000_0000 is reserved with VirtualAlloc and committed per region
  (kernel regions, modules, HLE heap); stray first touches are committed by a vectored exception handler.
- AArch64 FP/SIMD instructions are lowered to C (`gen.py --simd=c`, the default on non-arm64 hosts) instead of
  inline-asm passthrough (Windows on ARM keeps the inline asm).
- Renderer: Vulkan (runtime/nvn/vulkan) instead of Metal; shaders are translated to GLSL and compiled with
  shaderc. macOS keeps Metal.
- Seed switches and "Title screen" reboot the game in place on both (runtime/main.c); the only re-exec left is
  the startup retry when the guest window is taken, which on Windows starts a new process and exits.

## Tests
- `tools/test_translate.sh`: shader translator fuzz (MSL output unchanged, GLSL compiles and validates).
- `tools/test_vk_pipelines.sh`: fuzzed shaders through the backend's compile/reflection into real pipelines.
- `LARECOMP_VK_VALIDATION=1` with `VK_ADD_LAYER_PATH` pointing at `C:\msys64\ucrt64\bin` (ARM64:
  `C:\msys64\clangarm64\bin`): Khronos validation layer.
- Run test windows directly, not under MSYS `timeout` (it stalls the window).
