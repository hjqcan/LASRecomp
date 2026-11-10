#!/bin/bash
# android/deps/build_gen_deps.sh: what the app needs to build the game code on the device from the player's own
# extracted game files, as the desktop's start screen does - the generator (tools/gen.py) on Python for Android, the
# compiler zig - from pinned sources into android/deps/out/gen, for arm64-v8a:
#   python/   the official CPython for Android (python.org): libpython, its standard library, and pymain (a launcher)
#   lib/      the generator's native libraries, built with the NDK: capstone (ARM64 only), unicorn (aarch64 only), lz4;
#             their Python bindings load them with ctypes
#   site/     those bindings: capstone's and unicorn's own (pure Python), lz4.block from android/gen/lz4
#   zig/      zig for aarch64 Linux (a static binary: it runs on Android, as libzig.so) and what compiling the game's
#             C for aarch64 needs (its clang headers, musl's headers, its std for compiler_rt)
# Android runs programs only from the app's native library folder: the launchers ship as lib*.so there.
# Needs what build_deps.sh needs; each part is skipped when its output exists (delete out/gen to redo).
# From LASRecompAndroid by mamcjr (MIT): android/deps/build_gen_deps.sh and build_unicorn.sh.
set -euo pipefail
cd "$(dirname "$0")"
D=$PWD
S=$D/src
P=$D/out/gen
W=$D/work
mkdir -p "$S" "$P/lib" "$W"

case "$(uname -s)" in
    Darwin) SDK_DEFAULT=$HOME/Library/Android/sdk; EXE=; HOST=darwin-x86_64 ;;
    MINGW*|MSYS*|CYGWIN*) SDK_DEFAULT=${LOCALAPPDATA:-$HOME}/Android/Sdk; EXE=.exe; HOST=windows-x86_64 ;;
    *) SDK_DEFAULT=$HOME/Android/Sdk; EXE=; HOST=linux-x86_64 ;;
esac
SDK=${ANDROID_HOME:-$SDK_DEFAULT}
SDK=${SDK//\\//}
NDK=${ANDROID_NDK:-$SDK/ndk/30.0.16248370}
CMAKE_DIR=$SDK/cmake/3.30.4/bin
API=30
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)
CMAKE=$CMAKE_DIR/cmake$EXE
ANDROID_CMAKE=(-G Ninja -DCMAKE_MAKE_PROGRAM="$CMAKE_DIR/ninja$EXE" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
               -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release
               -DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384)
TC=$NDK/toolchains/llvm/prebuilt/$HOST/bin
CC=$TC/clang$EXE
STRIP=$TC/llvm-strip$EXE

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
fetch() {  # url sha256 file
    [ -f "$S/$3" ] || { curl -fsSL -o "$S/$3.part" "$1" && mv "$S/$3.part" "$S/$3"; }
    [ "$(sha256 "$S/$3")" = "$2" ] || { echo "checksum mismatch: $3" >&2; exit 1; }
}
unpack() { rm -rf "$W/$2" && mkdir -p "$W/$2" && tar -xf "$S/$1" -C "$W/$2" --strip-components 1; }

fetch https://www.python.org/ftp/python/3.14.8/python-3.14.8-aarch64-linux-android.tar.gz \
      11324313957da3736f44e90257304d288864321a4dd376ed0cc35a54eacf9a33 python-3.14.8-aarch64-linux-android.tar.gz
fetch https://github.com/capstone-engine/capstone/archive/refs/tags/5.0.9.tar.gz \
      0619da31af08152600af95c481527ef6d756c0a8404fca7544a4fdf6dfc2c0f9 capstone-5.0.9.tar.gz
fetch https://github.com/unicorn-engine/unicorn/archive/refs/tags/2.1.4.tar.gz \
      ea8863f095a0136388694e5a6063afd9bb7650e30243dd6251af59c5ce5601f4 unicorn-2.1.4.tar.gz
fetch https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz \
      537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b lz4-1.10.0.tar.gz
fetch https://ziglang.org/download/0.17.0/zig-aarch64-linux-0.17.0.tar.xz \
      9e8d11661d4ae3bd57702a3832781e23ad151dde5798e16a5ccd503f65234ff8 zig-aarch64-linux-0.17.0.tar.xz

# Python: libpython and the standard library (no tests, idle, tkinter, ensurepip), and the launcher
if [ ! -f "$P/python/libpymain.so" ]; then
    rm -rf "$W/py" && mkdir -p "$W/py" && tar -xzf "$S/python-3.14.8-aarch64-linux-android.tar.gz" -C "$W/py"
    rm -rf "$P/python" && mkdir -p "$P/python/lib"
    cp "$W/py/prefix/lib/libpython3.14.so" "$P/python/"
    cp -R "$W/py/prefix/lib/python3.14" "$P/python/lib/"
    rm -rf "$P/python/lib/python3.14/"{test,idlelib,tkinter,turtledemo,ensurepip,lib2to3,site-packages}
    find "$P/python/lib" -name __pycache__ -type d -prune -exec rm -rf {} +
    printf '#include <Python.h>\nint main(int argc, char **argv) { return Py_BytesMain(argc, argv); }\n' > "$W/pymain.c"
    "$CC" --target=aarch64-linux-android$API -O2 "$W/pymain.c" -I"$W/py/prefix/include/python3.14" -L"$P/python" -lpython3.14 \
        -Wl,-rpath,'$ORIGIN' -Wl,-z,max-page-size=16384 -o "$P/python/libpymain.so"
    cp "$W/py/prefix/lib/python3.14/../../../LICENSE"* "$P/python/LICENSE-python.txt" 2>/dev/null ||
        cp "$W/py/prefix/lib/python3.14/LICENSE.txt" "$P/python/LICENSE-python.txt"
fi

# capstone: ARM64 only
if [ ! -f "$P/lib/libcapstone.so" ]; then
    unpack capstone-5.0.9.tar.gz capstone
    "$CMAKE" -S "$W/capstone" -B "$W/capstone/build" "${ANDROID_CMAKE[@]}" -DBUILD_SHARED_LIBS=ON \
        -DCAPSTONE_BUILD_TESTS=OFF -DCAPSTONE_BUILD_CSTOOL=OFF -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF -DCAPSTONE_ARM64_SUPPORT=ON
    "$CMAKE" --build "$W/capstone/build" -j"$JOBS"
    cp "$W/capstone/build/libcapstone.so" "$P/lib/"
fi

# unicorn: aarch64 only. Its build runs QEMU's configure (a Unix shell script: WSL on Windows), which wants a
# pkg-config binary though unicorn needs no system package (an empty one that finds none), and gets the NDK's clang
# without its target, so its test programs fail and it decides there is no __int128 (aarch64 has it: forced)
if [ ! -f "$P/lib/libunicorn.so" ]; then
    unpack unicorn-2.1.4.tar.gz unicorn
    mkdir -p "$W/bin"
    printf '#!/bin/sh\nexit 1\n' > "$W/bin/pkg-config" && chmod 755 "$W/bin/pkg-config"
    sed -i.orig 's/^int128=no$/int128=yes/' "$W/unicorn/qemu/configure"
    PATH="$W/bin:$CMAKE_DIR:$PATH" "$CMAKE" -S "$W/unicorn" -B "$W/unicorn/build" "${ANDROID_CMAKE[@]}" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DUNICORN_ARCH=aarch64 -DUNICORN_BUILD_TESTS=OFF -DUNICORN_INSTALL=OFF \
        -DBUILD_SHARED_LIBS=ON
    PATH="$W/bin:$CMAKE_DIR:$PATH" "$CMAKE" --build "$W/unicorn/build" -j"$JOBS"
    cp -L "$(ls "$W"/unicorn/build/libunicorn.so* | head -1)" "$P/lib/libunicorn.so"
fi

# lz4
if [ ! -f "$P/lib/liblz4.so" ]; then
    unpack lz4-1.10.0.tar.gz lz4
    "$CMAKE" -S "$W/lz4/build/cmake" -B "$W/lz4/b" "${ANDROID_CMAKE[@]}" -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF \
        -DLZ4_BUILD_CLI=OFF
    "$CMAKE" --build "$W/lz4/b" -j"$JOBS"
    cp -L "$(ls "$W"/lz4/b/liblz4.so* | head -1)" "$P/lib/liblz4.so"
fi
for f in "$P"/lib/*.so; do "$STRIP" --strip-unneeded "$f"; done

# the bindings: capstone's and unicorn's (pure Python, ctypes), lz4.block (android/gen/lz4)
if [ ! -d "$P/site/unicorn" ]; then
    rm -rf "$P/site" && mkdir -p "$P/site"
    cp -R "$W/capstone/bindings/python/capstone" "$P/site/"
    cp -R "$W/unicorn/bindings/python/unicorn" "$P/site/"
    cp -R "$D/../gen/lz4" "$P/site/"
    find "$P/site" \( -name '*.so' -o -name '*.dll' -o -name '*.dylib' -o -name __pycache__ \) -prune -exec rm -rf {} +
fi

# zig: the binary, its clang headers, musl's headers for aarch64 and its std (compiler_rt is built from it)
if [ ! -f "$P/zig/libzig.so" ]; then
    rm -rf "$W/zig" && mkdir -p "$W/zig" && tar -xJf "$S/zig-aarch64-linux-0.17.0.tar.xz" -C "$W/zig" --strip-components 1
    rm -rf "$P/zig" && mkdir -p "$P/zig/lib/libc/include"
    cp "$W/zig/zig" "$P/zig/libzig.so"
    cp -R "$W/zig/lib/include" "$W/zig/lib/std" "$W/zig/lib/compiler_rt" "$P/zig/lib/"
    cp "$W/zig/lib/compiler_rt.zig" "$W/zig/lib/c.zig" "$P/zig/lib/"
    for d in aarch64-linux-musl aarch64-linux-any any-linux-any generic-musl; do cp -R "$W/zig/lib/libc/include/$d" "$P/zig/lib/libc/include/"; done
    cp "$W/zig/LICENSE" "$P/zig/LICENSE-zig.txt"
fi
echo "android/deps/out/gen: $(du -sh "$P" | cut -f1)"
ls -la "$P/lib" "$P/python" | head -20
