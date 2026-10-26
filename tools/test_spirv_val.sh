#!/bin/sh
# SPIR-V validation of the shader translator's Vulkan output: every program of the instruction-semantics test
# (tools/test_translate_ops.c) translated to GLSL as a vertex, fragment and compute shader, compiled with glslc and
# checked with spirv-val, both for Vulkan 1.3 (the backend's target: runtime/nvn/vulkan/shaders.c). Lists every
# shader that fails. (The shipped shader list, runtime/nvn/shader_variants.txt, holds only hashes: the shaders
# themselves are in the player's game files, which CI never has.)
# usage: tools/test_spirv_val.sh      (needs clang, glslc and spirv-val: Ubuntu glslc + spirv-tools, macOS
#                                       brew install shaderc spirv-tools)
set -e
cd "$(dirname "$0")/.."
OUT=build/test_spirv_val
rm -rf "$OUT"
mkdir -p "$OUT/glsl" "$OUT/spv"
CC=${CC:-clang}
$CC -O1 -Wall tools/test_translate_ops.c runtime/nvn/maxwell/translate.c -o "$OUT/gen" -lm
TFAIL=0
"$OUT/gen" --glsl "$OUT/glsl" || TFAIL=1
set +e
total=0
failed=""
for src in "$OUT"/glsl/*.vert "$OUT"/glsl/*.frag "$OUT"/glsl/*.comp; do
  [ -f "$src" ] || continue
  total=$((total + 1))
  base=$(basename "$src")
  id=${base%.*}
  name=$(grep "^${id} " "$OUT/glsl/names.txt" | cut -d' ' -f2-)
  spv="$OUT/spv/$base.spv"
  if ! glslc --target-env=vulkan1.3 -o "$spv" "$src" > "$OUT/spv/$base.log" 2>&1; then
    failed="$failed
  $base ($name): glslc: $(head -n 3 "$OUT/spv/$base.log" | tr '\n' ' ')"
  elif ! spirv-val --target-env vulkan1.3 "$spv" > "$OUT/spv/$base.log" 2>&1; then
    failed="$failed
  $base ($name): spirv-val: $(head -n 3 "$OUT/spv/$base.log" | tr '\n' ' ')"
  fi
done
nfail=$(printf '%s' "$failed" | grep -c .)
echo "SPIR-V validation (Vulkan 1.3): $total shaders, $nfail failed"
if [ -n "$failed" ]; then
  echo "Failed shaders (GLSL and logs in $OUT):$failed"
fi
[ "$total" -gt 0 ] && [ "$nfail" -eq 0 ] && [ "$TFAIL" -eq 0 ]
