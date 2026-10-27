#!/bin/bash
# make deps-windows (MSYS2 UCRT64 shell): what the shipped Windows build links against instead of MSYS2's packages,
# built from pinned sources into build/deps-win. MSYS2's FFmpeg is a GPLv3 build (x264, x265, xvid, ...); the
# release uses an LGPL-only FFmpeg with just what the game's movies need - WebM: VP9 video + Vorbis audio, plus
# VP8 / Opus / H.264 / AAC in WebM or MP4 (the same configuration as tools/build_deps_macos.sh) - and swscale.
# SDL3 (zlib licence) and the compiler runtime DLLs still come from MSYS2.
# Needs: the packages from tools/setup_windows.ps1 (clang, make, nasm). Skipped when its output exists
# (delete build/deps-win to redo). FFmpeg stores its configure line (with --prefix) in the DLLs, so it is configured
# for a neutral prefix and installed into build/deps-win with DESTDIR: no path of this machine ships.
set -euo pipefail
cd "$(dirname "$0")/.."
P=$PWD/build/deps-win
S=$P/src
mkdir -p "$S"
JOBS=$(nproc)
NEUTRAL=/opt/las-recomp-deps  # (FFmpeg's --prefix: never used at run time, the DLLs sit next to the exe)

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || curl -fsSL -o "$S/$3.part" "$1" && { [ -f "$S/$3" ] || mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | sha256sum -c --quiet
}
unpack() { rm -rf "$S/$2" && mkdir -p "$S/$2" && tar -xf "$S/$1" -C "$S/$2" --strip-components 1; }

fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
      8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz

fetch https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz \
      6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1 opus-1.6.1.tar.gz

if [ ! -f "$P/lib/libopus.dll.a" ]; then
    unpack opus-1.6.1.tar.gz opus
    (cd "$S/opus" && CC=clang ./configure --prefix="$P" --host=x86_64-w64-mingw32 \
        --enable-shared --disable-static --disable-extra-programs --disable-doc \
        && make -j"$JOBS" && make install)
fi

if [ ! -f "$P/lib/libavcodec.dll.a" ]; then
    unpack ffmpeg-9.0.2.tar.xz ffmpeg
    (cd "$S/ffmpeg" && ./configure --prefix="$NEUTRAL" --cc=clang --cxx=clang++ --arch=x86_64 --target-os=mingw32 \
        --enable-shared --disable-static \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        && make -j"$JOBS" && rm -rf "$S/ffmpeg-root" && make install DESTDIR="$S/ffmpeg-root")
    cp -R "$S/ffmpeg-root$NEUTRAL"/. "$P"/
    sed -i "s|$NEUTRAL|$P|g" "$P"/lib/pkgconfig/lib{avformat,avcodec,avutil,swscale}.pc  # (pkg-config: the real place)
fi
if [ "${CI:-}" != true ]; then  # (no path of this machine inside: FFmpeg's configure line; not on CI: no one's paths)
    for f in "$P"/bin/*.dll; do
        ! grep -qaiF -e "$PWD" -e "$(cygpath -m "$PWD")" -e "$(cygpath -w "$PWD")" "$f" || { echo "$f holds a path of this machine"; exit 1; }
    done
fi
grep -q 'GPL version 3\|--enable-gpl' "$P/lib/pkgconfig/libavcodec.pc" && { echo "deps-win: FFmpeg is not LGPL"; exit 1; }
echo "deps-win: $(ls "$P/bin"/*.dll | xargs -n1 basename | tr '\n' ' ')"
