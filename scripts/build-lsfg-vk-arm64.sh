#!/usr/bin/env bash
# Build the aarch64 lsfg-vk 1.x layer shipped as sm8650-overlay/usr/lib/liblsfg-vk-arm64.so.
# Run on an aarch64 Linux box (the build VM: Ubuntu 24.04, glibc 2.39, older than SteamOS).
# Needs git, cmake, make and clang: gcc ignores the export attribute in
# upstream's layer.hpp, so layer_vkGet*ProcAddr end up hidden and the
# loader cannot use the layer. Upstream builds with clang too.
#
# Why this commit: lsfg-vk 2.x (2.0.0 and master 40d1e4e) draws garbage on
# Adreno/Turnip; 62f8149 is the last 1.x commit before the 2.0 rewrite.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORK="${1:-/tmp/lsfg-vk-arm64}"
LSFG_REPO="https://github.com/PancakeTAS/lsfg-vk.git"
LSFG_COMMIT=62f8149
HEADERS_TAG=v1.4.321
OUT="${ROOT}/sm8650-overlay/usr/lib/liblsfg-vk-arm64.so"

[[ "$(uname -m)" == aarch64 ]] || { echo "ERROR: build on aarch64" >&2; exit 1; }
for tool in git cmake make clang clang++; do
  command -v "$tool" >/dev/null || { echo "ERROR: $tool missing" >&2; exit 1; }
done

mkdir -p "$WORK"
if [[ ! -d "$WORK/src/.git" ]]; then
  git clone -q "$LSFG_REPO" "$WORK/src"
fi
git -C "$WORK/src" checkout -q "$LSFG_COMMIT"
if [[ ! -d "$WORK/vkheaders" ]]; then
  git clone -q --depth 1 -b "$HEADERS_TAG" \
    https://github.com/KhronosGroup/Vulkan-Headers.git "$WORK/vkheaders"
fi

echo "==> cmake lsfg-vk ${LSFG_COMMIT}"
rm -rf "$WORK/build"
INC="-I$WORK/vkheaders/include"
cmake -S "$WORK/src" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="$INC" -DCMAKE_CXX_FLAGS="$INC"

echo "==> build"
make -C "$WORK/build" -j"$(nproc)" lsfg-vk

SO="$WORK/build/liblsfg-vk.so"
nm -D --defined-only "$SO" | grep -q ' T layer_vkGetInstanceProcAddr$' \
  || { echo "ERROR: layer_vkGetInstanceProcAddr not exported" >&2; exit 1; }
install -m0644 "$SO" "$OUT"
sha256sum "$OUT"
echo "==> update the sha256 in ${OUT}.README"
