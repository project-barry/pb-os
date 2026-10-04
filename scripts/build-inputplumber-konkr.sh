#!/usr/bin/env bash
# Build on ARM64 Linux with glibc <= 2.39, Rust 1.93.1, libiio/udev/clang development files.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CACHE="$ROOT/external-and-mods/InputPlumber"
SRC="${INPUTPLUMBER_BUILD_SRC:-/work/inputplumber-konkr-src}"
REF=ea60d873cca17edd1cb655ede26f557108135252
[[ $(uname -m) == aarch64 ]] || { echo 'ARM64 build host required' >&2; exit 1; }
if [[ ! -d "$SRC/.git" ]]; then git clone https://github.com/ShadowBlip/InputPlumber.git "$SRC"; fi
[[ -z $(git -C "$SRC" status --porcelain) ]] || { echo 'Use a clean source checkout' >&2; exit 1; }
git -C "$SRC" checkout --detach "$REF"
git -C "$SRC" apply "$CACHE/0001-bound-ayaneo-haptics-polling.patch"
git -C "$SRC" apply "$CACHE/0002-ayaneo-rumble-curve.patch"
(cd "$SRC" && cargo +1.93.1 build --release --locked -j"${BUILD_JOBS:-4}")
install -m755 "$SRC/target/release/inputplumber" "$CACHE/inputplumber-0.81.0-konkr"
(cd "$CACHE" && sha256sum inputplumber-0.81.0-konkr > inputplumber-0.81.0-konkr.sha256)
echo 'Verify this binary against the target rootfs before release.'
