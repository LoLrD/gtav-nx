#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MESA_SDK="${MESA_SDK:-$HOME/work/nier-nx-data/mesa-sdk}"
NRO_OUT="${NRO_OUT:-out/nx}"
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
  -e MESA_SDK="$MESA_SDK" -e NRO_OUT="$NRO_OUT" -e JOBS="$JOBS" -e ROOT="$ROOT" -e SHADERS="$SHADERS" "$IMAGE" bash -euo pipefail -c '
    cmake -S gtav -B "$NRO_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
      -DDEVKITPRO=/opt/devkitpro -DREXSDK_DIR="$ROOT/sdk" -DREXGLUE_BUILD_TESTS=OFF \
      -DGTAV_PREBUILT_NATIVE_SHADERS="$SHADERS" \
      -DREXGLUE_SWITCH_NVK_SDK="$MESA_SDK/opt/devkitpro/portlibs/switch"
    cmake --build "$NRO_OUT" -j "$JOBS"
  '
echo "ok: $ROOT/$NRO_OUT/gtav.nro"
