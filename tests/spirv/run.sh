#!/usr/bin/env bash
# Renders every shader in shaders/ twice on lavapipe, once from glslang's
# SPIR-V and once through VkVita's SPIR-V to Cg translation (parsed back as
# HLSL), and compares the pixels.
#
# Needs: glslangValidator, spirv-opt, gcc/g++, the Vulkan loader headers and
# lavapipe (Debian/Ubuntu: glslang-tools spirv-tools libvulkan-dev mesa-vulkan-drivers).
#   tests/spirv/run.sh                 VkVita's own translator
#   OPT=-O tests/spirv/run.sh          same on optimized SPIR-V
#   S2C_MODE=-c tests/spirv/run.sh    SPIRV-Cross
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
export OUT="${OUT:-$HERE/build}"
mkdir -p "$OUT"
[[ -x "$OUT/spirv2cg" ]] || bash "$HERE/../../tools/build_spirv2cg.sh" "$OUT/spirv2cg"
[[ -x "$OUT/render" ]] || gcc -O1 -o "$OUT/render" "$HERE/render.c" -lvulkan
python3 "$HERE/difftest.py" "$@"
