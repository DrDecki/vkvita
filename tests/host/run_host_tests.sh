#!/usr/bin/env bash
# Host smoke test: builds VkVita + all samples for the PC against a mock of
# the Vita system/GXM APIs (tests/host/mock_sce.c) with AddressSanitizer and
# UBSan, then runs each sample for a scripted number of frames.
#
# Needs: gcc/g++ with ASan, vita-headers (from your VitaSDK) and vitashark.h.
#   VITA_HEADERS  default: $VITASDK/arm-vita-eabi/include
#   VITASHARK_INC default: same as VITA_HEADERS
#   SPIRV_CROSS   empty: build the bundled SPIRV-Cross. Or link flags for an
#                 installed SPIRV-Cross C library, for example
#                 SPIRV_CROSS="-I/usr/include/spirv_cross -lspirv-cross-c-shared"
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VITA_HEADERS="${VITA_HEADERS:-${VITASDK:-/usr/local/vitasdk}/arm-vita-eabi/include}"
VITASHARK_INC="${VITASHARK_INC:-$VITA_HEADERS}"
OUT="${OUT:-$ROOT/build-host}"
mkdir -p "$OUT"

CFLAGS=(-std=gnu11 -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined
        -Wall -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function
        -D__vita__ -DVKV_WITH_SHARK=1 -DVKV_ENABLE_DEBUG=1 -DVKV_WITH_SPIRV=1
        -isystem "$VITA_HEADERS" -I "$VITASHARK_INC"
        -I "$ROOT/include" -I "$ROOT/third_party/Vulkan-Headers/include" -I "$ROOT/samples/common")

SPIRV_LINK=()
if [[ -n "${SPIRV_CROSS:-}" ]]; then
  read -r -a SPIRV_LINK <<< "$SPIRV_CROSS"
  for flag in "${SPIRV_LINK[@]}"; do [[ $flag == -I* ]] && CFLAGS+=("$flag"); done
  LINKER=gcc
else
  CFLAGS+=(-I "$ROOT/third_party/SPIRV-Cross")
  LINKER=g++
fi

echo "== building library + mock"
objs=()
for f in "$ROOT"/src/*.c "$ROOT/tests/host/mock_sce.c" "$ROOT/samples/common/vkv_sample.c"; do
  o="$OUT/$(basename "$f").o"
  gcc "${CFLAGS[@]}" -c "$f" -o "$o"
  objs+=("$o")
done
if [[ -z "${SPIRV_CROSS:-}" ]]; then
  echo "== building SPIRV-Cross"
  for f in cfg cross cross_c cross_parsed_ir glsl hlsl parser; do
    o="$OUT/spirv_$f.o"
    [[ -f "$o" ]] || g++ -std=gnu++11 -O1 -g -w -fsanitize=address,undefined -DSPIRV_CROSS_C_API_GLSL=1 \
      -DSPIRV_CROSS_C_API_HLSL=1 -c "$ROOT/third_party/SPIRV-Cross/spirv_$f.cpp" -o "$o" &
    objs+=("$o")
  done
  wait
fi

build_sample() { # name source
  $LINKER "${CFLAGS[@]}" -x c "$2" -x none "${objs[@]}" "${SPIRV_LINK[@]}" -lm -o "$OUT/$1"
}
build_sample hello_triangle "$ROOT/samples/hello_triangle/main.c"
build_sample textured_cube  "$ROOT/samples/textured_cube/main.c"
build_sample fixed_function "$ROOT/samples/fixed_function/main.c"
build_sample template       "$ROOT/samples/standalone_template/main.c"
build_sample spirv_cube     "$ROOT/samples/spirv_cube/main.c"

fail=0
run() { # label binary [env...]
  local label="$1" bin="$2"; shift 2
  local fs="$OUT/fs_$label"
  rm -rf "$fs"; mkdir -p "$fs"
  if [[ -n "${CONFIG_TXT:-}" ]]; then mkdir -p "$fs/ux0/data/vkvita"; printf '%s\n' "$CONFIG_TXT" > "$fs/ux0/data/vkvita/config.txt"; fi
  local log="$OUT/$label.log"
  if env MOCK_ROOT="$fs" MOCK_FRAMES="${FRAMES:-90}" ASAN_OPTIONS=detect_leaks=0 "$@" "$bin" > "$log" 2>&1; then
    local verr; verr=$(grep -c "\[vkvita ERROR\]" "$log" || true)
    if [[ "$verr" != "0" && -z "${EXPECT_ERRORS:-}" ]]; then
      echo "FAIL $label: $verr VkVita errors"; grep "\[vkvita ERROR\]" "$log" | head -5; fail=1
    else
      echo "ok   $label  $(grep '^\[mock\] frames' "$log")"
    fi
  else
    echo "FAIL $label (exit $?)"; grep -E "MOCK ERROR|ERROR|runtime error|AddressSanitizer" "$log" | head -15; fail=1
  fi
}

echo "== running"
run hello_triangle "$OUT/hello_triangle"
run textured_cube  "$OUT/textured_cube"
run fixed_function "$OUT/fixed_function"
run template       "$OUT/template" MOCK_NO_SHARK=1
run spirv_cube     "$OUT/spirv_cube"
CONFIG_TXT=$'debug = info, validation, shaders' FRAMES=12 \
  run spirv_debug "$OUT/spirv_cube"
CONFIG_TXT=$'debug = info, validation, profile, shaders\ncompat = cpu_clear, gl_clip_space, invert_cull, ignore_scissor, shader_cache\nprofile_interval = 30' \
  run cube_compat "$OUT/textured_cube"
CONFIG_TXT=$'debug = validation, gpu_sync, calls\ncompat = fast_transfers, no_vsync' FRAMES=12 \
  run ff_debug "$OUT/fixed_function"

# second launch reuses the GXP files dumped by the first one (shader cache)
fs="$OUT/fs_cache"; rm -rf "$fs"; mkdir -p "$fs"
MOCK_ROOT="$fs" MOCK_FRAMES=5 ASAN_OPTIONS=detect_leaks=0 "$OUT/textured_cube" > "$OUT/cache1.log" 2>&1 || fail=1
if MOCK_ROOT="$fs" MOCK_FRAMES=5 MOCK_NO_SHARK=1 ASAN_OPTIONS=detect_leaks=0 "$OUT/textured_cube" > "$OUT/cache2.log" 2>&1 \
   && grep -q "shader cache hit" "$OUT/cache2.log"; then
  echo "ok   shader_cache  (second run used dumped GXP with the compiler unavailable)"
else
  echo "FAIL shader_cache"; fail=1
fi

# no compiler available: must fail cleanly with a clear message, not crash
if MOCK_ROOT="$OUT/fs_noshark" MOCK_NO_SHARK=1 MOCK_FRAMES=5 ASAN_OPTIONS=detect_leaks=0 "$OUT/hello_triangle" > "$OUT/noshark.log" 2>&1; then :; fi
if grep -q "libshacccg" "$OUT/noshark.log" && ! grep -q "AddressSanitizer\|runtime error" "$OUT/noshark.log"; then
  echo "ok   no_compiler  (clean error message)"
else
  echo "FAIL no_compiler"; fail=1
fi

[[ $fail == 0 ]] && echo "ALL HOST TESTS PASSED" || { echo "HOST TESTS FAILED (logs in $OUT)"; exit 1; }
