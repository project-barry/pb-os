#!/bin/bash
# Proton PB-OS: build Wine's X11 driver (winex11.so, aarch64-unix) from Valve's
# Proton source with the patches here, in Valve's own arm64 Proton SDK image.
# Run on an arm64 Linux host with Docker (the build VM), as root:
#   external-and-mods/proton-pbos/build.sh TAG [WORKDIR]   (WORKDIR default /work/proton-pbos)
# TAG is Valve's Proton git tag of the installed tool's version, e.g.
#   proton-11.0-2e            Proton 11.0 (ARM64), version proton-11.0-2e-arm64
#   experimental-11.0-20261001  Proton Experimental (ARM64), experimental-11.0-20261001-arm64
# Output: dist/TAG-arm64/winex11.so (stripped) here, for files/lib/wine/aarch64-unix/
# of that exact Valve tool, plus valve-winex11.so.sha256 = Valve's own file it replaces
# (pass VALVE_WINEX11=path/to/Valve's winex11.so, from a device with that version).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTON_TAG="${1:?usage: build.sh TAG [WORKDIR]}"
W="${2:-/work/proton-pbos}/$PROTON_TAG"
OUT="$HERE/dist/$PROTON_TAG-arm64"
J="${J:-$(nproc)}"

mkdir -p "$W"
if [[ ! -d "$W/src/.git" ]]; then
  git clone --depth 1 --branch "$PROTON_TAG" https://github.com/ValveSoftware/Proton.git "$W/src"
  git -C "$W/src" submodule update --init --depth 1 wine
fi
# The SDK image Valve built this tag with (its Makefile.in), unless SDK= is set.
SDK="${SDK:-$(sed -n 's/^[[:space:]]*STEAMRT_IMAGE ?= \(.*\/sdk\/arm64-llvm:.*\)$/\1/p' "$W/src/Makefile.in")}"
[[ -n "$SDK" ]] || { echo "no arm64-llvm SDK in $W/src/Makefile.in; set SDK=" >&2; exit 1; }
echo "SDK: $SDK"
docker pull -q "$SDK"
cp "$HERE"/*.patch "$W/"

docker run --rm -v "$W:$W" -e W="$W" -e J="$J" "$SDK" bash -euo pipefail -c '
  rm -rf "$W/wine" "$W/build"; cp -a "$W/src/wine" "$W/wine"; cd "$W/wine"
  for p in "$W"/*.patch; do patch -p1 < "$p"; done
  # As Proton'"'"'s Makefile.in does before configuring wine.
  XDG_CACHE_HOME="$W/cache" dlls/winevulkan/make_vulkan -x vk.xml -X video.xml
  tools/make_specfiles || true
  tools/make_requests
  autoreconf -fi >/dev/null
  mkdir -p "$W/build"; cd "$W/build"
  CFLAGS="-O2 -g -march=armv8.2-a -mtune=cortex-x3" "$W/wine/configure" -C \
    --host=aarch64-linux-gnu --disable-tests --with-mingw=clang \
    --enable-archs=arm64ec,aarch64,i386 >configure.log 2>&1
  make -j"$J" __tooldeps__ >make-tools.log 2>&1
  make -j"$J" dlls/winex11.drv/winex11.so >make-x11.log 2>&1
  strip -o "$W/winex11.so" dlls/winex11.drv/winex11.so'
mkdir -p "$OUT"
cp "$W/winex11.so" "$OUT/winex11.so"; chmod 0644 "$OUT/winex11.so"
if [[ -n "${VALVE_WINEX11:-}" ]]; then
  (cd "$(dirname "$VALVE_WINEX11")" && sha256sum "$(basename "$VALVE_WINEX11")") \
    | awk '{print $1 "  winex11.so"}' > "$OUT/valve-winex11.so.sha256"
fi
ls -la "$OUT"
