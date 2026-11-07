#!/bin/bash
# android/deps/build_deps.sh: what the Android app links against, from pinned sources (the versions of
# tools/linux/build_deps.sh where both have them) into android/deps/out, for arm64-v8a:
#   SDL3 (shared, plus its Java side of the same version), shaderc (one shared library, glslang and SPIRV-Tools
#   inside), the headers of Vulkan (newer than the NDK's) and the Vulkan Memory Allocator, and volk (built into the
#   app: Android's libvulkan.so exports Vulkan 1.1 only before API 33, so every function comes through volk).
#   FFmpeg (shared, LGPL only): the desktop builds' version and configuration (tools/linux/build_deps.sh), for the
#   game's movies (runtime/hle/movie.c) and the start screen's background frame.
# Needs bash, curl, Python 3 and the Android SDK with NDK 30.0.16248370 and CMake
# 3.30.4 (ANDROID_HOME, default ~/Library/Android/sdk on macOS, ~/Android/Sdk elsewhere). Each part is skipped when its
# output exists (delete out/ to redo). From LASRecompAndroid by mamcjr (MIT).
set -euo pipefail
cd "$(dirname "$0")"
D=$PWD
S=$D/src
P=$D/out
W=$D/work
mkdir -p "$S" "$P/include" "$P/lib" "$W"

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
               -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release)

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
fetch() {  # url sha256 file
    [ -f "$S/$3" ] || { curl -fsSL -o "$S/$3.part" "$1" && mv "$S/$3.part" "$S/$3"; }
    [ "$(sha256 "$S/$3")" = "$2" ] || { echo "checksum mismatch: $3" >&2; exit 1; }
}
unpack() { rm -rf "$W/$2" && mkdir -p "$W/$2" && tar -xf "$S/$1" -C "$W/$2" --strip-components 1; }

fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz \
      7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68 SDL3-3.4.16.tar.gz
fetch https://github.com/google/shaderc/archive/refs/tags/v2026.3.tar.gz \
      ee493ccf1b3038b4ef2fe024664c5eb2dc4bcc1f6b05b33e3909de0e19c81024 shaderc-2026.3.tar.gz
fetch https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz \
      6d1bb65e49520344cc0a48af3dc02e993781efff14c7ebdcb8ae9fa23ddf7e83 Vulkan-Headers-1.4.350.1.tar.gz
fetch https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz \
      c4f6bbe6b5a45c2eb610ca9d231158e313086d5b1a40c9922cb42b597419b14e VulkanMemoryAllocator-3.3.0.tar.gz
fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
      8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz
fetch https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz \
      078a9411298e4e0f60f5f5398c890783427c25a414619294ca8e69587bbd5eae volk-1.4.350.1.tar.gz

if [ ! -f "$P/include/vulkan/vulkan_core.h" ]; then
    unpack Vulkan-Headers-1.4.350.1.tar.gz vkh
    cp -r "$W/vkh/include/vulkan" "$W/vkh/include/vk_video" "$P/include/"
fi
if [ ! -f "$P/include/vma/vk_mem_alloc.h" ]; then
    unpack VulkanMemoryAllocator-3.3.0.tar.gz vma
    mkdir -p "$P/include/vma" && cp "$W/vma/include/vk_mem_alloc.h" "$P/include/vma/"
fi

if [ ! -f "$P/volk/volk.c" ]; then
    unpack volk-1.4.350.1.tar.gz volk
    mkdir -p "$P/volk" && cp "$W/volk/volk.h" "$W/volk/volk.c" "$W/volk/LICENSE.md" "$P/volk/"
fi

# SDL3: the shared library and the Java side of the same version (org.libsdl.app, compiled into the app)
if [ ! -f "$P/lib/libSDL3.so" ]; then
    unpack SDL3-3.4.16.tar.gz sdl
    "$CMAKE" -S "$W/sdl" -B "$W/sdl/build" "${ANDROID_CMAKE[@]}" -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    "$CMAKE" --build "$W/sdl/build" -j"$JOBS"
    "$CMAKE" --install "$W/sdl/build"
    rm -rf "$P/java" && mkdir -p "$P/java/org/libsdl"
    cp -r "$W/sdl/android-project/app/src/main/java/org/libsdl/app" "$P/java/org/libsdl/"
fi

# shaderc as one shared library, as on Linux / Windows; its DEPS file pins glslang / SPIRV-Tools / SPIRV-Headers
if [ ! -f "$P/lib/libshaderc_shared.so" ]; then
    unpack shaderc-2026.3.tar.gz shaderc
    PY=${PYTHON:-}
    for p in python3 python; do  # (Windows: python3 may be the Microsoft Store's stub, which can't run anything)
        [ -z "$PY" ] && command -v $p >/dev/null && $p -c 1 2>/dev/null && PY=$p
    done
    : "${PY:?no Python for the shaderc utils/git-sync-deps}"
    (cd "$W/shaderc" && "$PY" utils/git-sync-deps)
    "$CMAKE" -S "$W/shaderc" -B "$W/shaderc/build" "${ANDROID_CMAKE[@]}" -DSHADERC_SKIP_TESTS=ON \
        -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON \
        -DENABLE_GLSLANG_BINARIES=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON -DBUILD_SHARED_LIBS=OFF
    "$CMAKE" --build "$W/shaderc/build" --target shaderc_shared -j"$JOBS"
    mkdir -p "$P/include/shaderc"
    cp "$W/shaderc/libshaderc/include/shaderc/"*.h "$P/include/shaderc/"
    cp "$W/shaderc/build/libshaderc/libshaderc_shared.so" "$P/lib/"
fi

# FFmpeg: configure is a Unix shell script (WSL on Windows). Shared libraries without version suffixes (libavcodec.so,
# not .so.62): Android loads what the APK ships by its exact file name. From LASRecompAndroid by mamcjr (MIT).
if [ ! -f "$P/lib/libavcodec.so" ]; then
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) echo "FFmpeg: run this script in WSL"; exit 1 ;; esac
    unpack ffmpeg-9.0.2.tar.xz ffmpeg
    TC=$NDK/toolchains/llvm/prebuilt/$HOST/bin
    (cd "$W/ffmpeg" && ./configure --prefix="$P" --enable-cross-compile --target-os=android --arch=aarch64 --cpu=armv8-a \
        --cc="$TC/aarch64-linux-android$API-clang" --cxx="$TC/aarch64-linux-android$API-clang++" \
        --nm="$TC/llvm-nm" --ar="$TC/llvm-ar" --ranlib="$TC/llvm-ranlib" --strip="$TC/llvm-strip" \
        --extra-ldflags=-Wl,-z,max-page-size=16384 \
        --enable-shared --disable-static --enable-pic \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac)
    sed -i.orig -e 's/^SLIBNAME_WITH_MAJOR=.*/SLIBNAME_WITH_MAJOR=$(SLIBNAME)/' \
        -e 's/^SLIBNAME_WITH_VERSION=.*/SLIBNAME_WITH_VERSION=$(SLIBNAME)/' -e 's/^SLIB_INSTALL_NAME=.*/SLIB_INSTALL_NAME=$(SLIBNAME)/' \
        -e 's/^SLIB_INSTALL_LINKS=.*/SLIB_INSTALL_LINKS=/' "$W/ffmpeg/ffbuild/config.mak"
    (cd "$W/ffmpeg" && make -j"$JOBS" && make install)
fi
# (the library's own license line: avcodec_license())
grep -qa 'GPL version 3\|--enable-gpl' "$P/lib/libavcodec.so" && { echo "deps: FFmpeg is not LGPL"; exit 1; }

echo "deps: $P"
ls "$P/lib"
