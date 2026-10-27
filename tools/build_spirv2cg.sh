#!/usr/bin/env bash
# Builds the spirv2cg host tool with the bundled SPIRV-Cross.
#   tools/build_spirv2cg.sh [output]   (default: ./spirv2cg)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-spirv2cg}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
SC="$ROOT/third_party/SPIRV-Cross"
for f in cfg cross cross_c cross_parsed_ir glsl hlsl parser; do
  g++ -std=gnu++11 -O2 -w -DSPIRV_CROSS_C_API_GLSL=1 -DSPIRV_CROSS_C_API_HLSL=1 -c "$SC/spirv_$f.cpp" -o "$TMP/$f.o" &
done
gcc -std=gnu11 -O2 -I "$SC" -c "$ROOT/src/vkv_spirv.c" -o "$TMP/vkv_spirv.o" &
gcc -std=gnu11 -O2 -c "$ROOT/src/vkv_spirv_lower.c" -o "$TMP/vkv_spirv_lower.o" &
gcc -std=gnu11 -O2 -c "$ROOT/tools/spirv2cg.c" -o "$TMP/spirv2cg.o" &
wait
g++ "$TMP"/*.o -o "$OUT"
echo "built $OUT"
