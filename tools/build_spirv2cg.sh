#!/usr/bin/env bash
# Builds the spirv2cg host tool.
#   tools/build_spirv2cg.sh [output]   (default: ./spirv2cg)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-spirv2cg}"
gcc -std=gnu11 -O2 -I "$ROOT/third_party/spirv" "$ROOT/src/vkv_spirv.c" "$ROOT/src/vkv_spirv_lower.c" \
    "$ROOT/src/vkv_spirv_cg.c" "$ROOT/tools/spirv2cg.c" -lm -o "$OUT"
echo "built $OUT"
