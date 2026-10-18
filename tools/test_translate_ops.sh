#!/bin/sh
# Instruction-semantics regression test for runtime/nvn/maxwell/translate.c (see tools/test_translate_ops.c):
# translates small programs, runs their MSL bodies on the CPU and checks the results.
# usage: tools/test_translate_ops.sh   (needs clang / clang++)
set -e
cd "$(dirname "$0")/.."
OUT=build/test_translate_ops
mkdir -p "$OUT"
CC=${CC:-clang}
CXX=${CXX:-clang++}
$CC -O1 -Wall tools/test_translate_ops.c runtime/nvn/maxwell/translate.c -o "$OUT/gen" -lm
"$OUT/gen" "$OUT/cases.cpp"
$CXX -std=c++17 -O1 -w "$OUT/cases.cpp" -o "$OUT/cases"
"$OUT/cases"
