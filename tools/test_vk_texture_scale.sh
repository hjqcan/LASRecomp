#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
${CC:-clang} -std=gnu11 -O1 -Iruntime tools/test_vk_texture_scale.c \
    runtime/nvn/tegra.c runtime/nvn/formats.c runtime/nvn/astc.c \
    -lSDL3 -lvulkan-1 -o build/test-vk-texture-scale.exe
build/test-vk-texture-scale.exe
