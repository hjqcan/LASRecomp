# One-time setup of a Windows machine for building LARecomp (see docs/BUILD-windows.md).
# Installs MSYS2 (clang, lld, make, SDL3, FFmpeg, Vulkan loader/headers, shaderc) and Python 3.12, then creates
# tools/venv (lifter: unicorn, capstone, lz4). Safe to re-run.
# On Windows on ARM (experimental): MSYS2 CLANGARM64 instead of UCRT64, and an x64 Python 3.12 (unicorn / capstone
# have no ARM64 Windows wheels; it runs under emulation).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path C:\msys64\usr\bin\pacman.exe)) {
    winget install --id MSYS2.MSYS2 -e --accept-source-agreements --accept-package-agreements --silent
}
$arm = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64'
$env:MSYSTEM = if ($arm) { 'CLANGARM64' } else { 'UCRT64' }
$env:CHERE_INVOKING = '1'
$bash = 'C:\msys64\usr\bin\bash.exe'
& $bash -lc 'pacman -Syuu --noconfirm' | Out-Null
& $bash -lc 'pacman -Syuu --noconfirm' | Out-Null  # second pass after a core update
$mp = if ($arm) { 'mingw-w64-clang-aarch64' } else { 'mingw-w64-ucrt-x86_64' }
$pkgs = @('make', 'zip', 'unzip') + (@('clang', 'lld', 'sdl3', 'ffmpeg', 'vulkan-devel', 'shaderc', 'spirv-tools', 'pkgconf',
        'gdb', 'vulkan-memory-allocator', 'vulkan-validation-layers') | ForEach-Object { "$mp-$_" })
if (-not $arm) { $pkgs += "$mp-nasm" }  # (make deps-windows: FFmpeg's VP9 assembly; ARM64 assembles with clang)
& $bash -lc "pacman -S --needed --noconfirm $($pkgs -join ' ')"

# Python for the tools: the python.org build (prebuilt unicorn / capstone wheels; MSYS2's Python can't build them)
if ($arm) {
    # x64 next to any ARM64 3.12 (winget won't install both under one id), from python.org's signed installer
    $py = "$env:LOCALAPPDATA\Programs\Python\Python312-x64\python.exe"
    if (-not (Test-Path $py)) {
        $exe = "$env:TEMP\python-3.12.10-amd64.exe"
        Invoke-WebRequest https://www.python.org/ftp/python/3.12.10/python-3.12.10-amd64.exe -OutFile $exe
        if ((Get-AuthenticodeSignature $exe).Status -ne 'Valid') { throw "${exe}: not validly signed" }
        Start-Process $exe -Wait -ArgumentList '/quiet', 'InstallAllUsers=0', 'PrependPath=0', 'Include_launcher=0',
            'Include_test=0', "TargetDir=$(Split-Path $py)"
    }
} else {
    $py = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
    if (-not (Test-Path $py)) {
        winget install --id Python.Python.3.12 -e --scope user --accept-source-agreements --accept-package-agreements --silent
    }
}
if (-not (Test-Path "$root\tools\venv\Scripts\python.exe")) { & $py -m venv "$root\tools\venv" }
& "$root\tools\venv\Scripts\python.exe" -m pip install -q lz4 capstone unicorn

# Randomizer (LAS-Randomizer's own venv, used by the Randomizer tab's seed generator)
$las = "$root\third_party\LAS-Randomizer"
if (-not (Test-Path "$las\venv\Scripts\python.exe")) { & $py -m venv "$las\venv" }
& "$las\venv\Scripts\python.exe" -m pip install -q -r "$las\requirements.txt"

# LF line endings in this clone (the bash tools break with CRLF; Git for Windows defaults to autocrlf=true)
git -C $root config core.autocrlf false
git -C $root config core.eol lf
if ($arm) {
    Write-Host 'Done. Build from an MSYS2 CLANGARM64 shell (or put C:\msys64\clangarm64\bin;C:\msys64\usr\bin first on PATH): make -j'
    Write-Host "Helpers (x64 Python, no py launcher): make rando-helper RANDO_PY=`"$($py -replace '\\', '/')`" (same for gen-helper)"
} else {
    Write-Host 'Done. Build from an MSYS2 UCRT64 shell (or put C:\msys64\ucrt64\bin;C:\msys64\usr\bin first on PATH): make -j'
}
