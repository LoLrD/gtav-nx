#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MESA_SDK="${MESA_SDK:-$ROOT/out/mesa-sdk}"
PGO="${PGO:-OFF}"
LTO="${LTO:-OFF}"
LTO_JOBS="${LTO_JOBS:-auto}"
CODE_BLOCKS="${CODE_BLOCKS:-ON}"
MALLOC_CACHE="${MALLOC_CACHE:-ON}"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}"
case "$PGO" in
  OFF) NRO_OUT="${NRO_OUT:-out/nx}" ;;
  GENERATE) NRO_OUT="${NRO_OUT:-out/nx-pgo-gen}" ;;
  USE) NRO_OUT="${NRO_OUT:-out/nx-pgo-use}" ;;
  *) echo "error: PGO must be OFF, GENERATE or USE" >&2; exit 1 ;;
esac
JOBS="${JOBS:-8}"
SHADERS="${GTAV_NATIVE_SHADERS:-$ROOT/build/mac/native_shaders}"
LIBVULKAN="$MESA_SDK/opt/devkitpro/portlibs/switch/lib/libvulkan.a"

[[ -f "$LIBVULKAN" ]] || { echo "error: the NVK driver was not found at $LIBVULKAN" >&2; exit 1; }
[[ -f "$ROOT/gtav/generated/rexglue.cmake" ]] || { echo "error: gtav/generated missing; run codegen" >&2; exit 1; }
[[ -f "$SHADERS/native_blit_shaders.h" ]] || { echo "error: native shader headers missing in $SHADERS (build for macOS first)" >&2; exit 1; }
MESA_SDK="$(cd "$MESA_SDK" && pwd -P)"
SHADERS="$(cd "$SHADERS" && pwd -P)"
source "$ROOT/tools/switch/docker.sh"
ensure_image

docker run --rm -v "$ROOT:$ROOT" -v "$MESA_SDK:$MESA_SDK:ro" -w "$ROOT" \
  -e MESA_SDK="$MESA_SDK" -e NRO_OUT="$NRO_OUT" -e JOBS="$JOBS" -e PGO="$PGO" -e LTO="$LTO" -e LTO_JOBS="$LTO_JOBS" -e CODE_BLOCKS="$CODE_BLOCKS" -e MALLOC_CACHE="$MALLOC_CACHE" -e ROOT="$ROOT" -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" -e SHADERS="$SHADERS" "$IMAGE" bash -euo pipefail -c '
    cmake -S gtav -B "$NRO_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
      -DDEVKITPRO=/opt/devkitpro -DREXSDK_DIR="$ROOT/sdk" -DREXGLUE_BUILD_TESTS=OFF \
      -DGTAV_PREBUILT_NATIVE_SHADERS="$SHADERS" -DGTAV_PGO="$PGO" \
      -DGTAV_SWITCH_LTO="$LTO" -DGTAV_SWITCH_LTO_JOBS="$LTO_JOBS" \
      -DREXGLUE_SWITCH_CODE_BLOCKS="$CODE_BLOCKS" -DREXGLUE_SWITCH_MALLOC_CACHE="$MALLOC_CACHE" \
      -DREXGLUE_SWITCH_NVK_SDK="$MESA_SDK/opt/devkitpro/portlibs/switch"
    cmake --build "$NRO_OUT" -j "$JOBS" -- -k 0
  '
echo "ok: $ROOT/$NRO_OUT/gtav.nro"
