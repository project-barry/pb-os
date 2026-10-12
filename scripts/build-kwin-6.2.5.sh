#!/usr/bin/env bash
# Build KWin 6.2.5 with Valve's three "Xwayland XTest without the portal"
# patches inside the image rootfs (bubblewrap, so gcc/Qt/KF6 match the
# image), then copy the package files over the base's kwin.
#
# Why: Desktop Mode asks "An application requested access to input devices"
# whenever Steam Input moves the pointer (pb-os #6). KWin 6.4+ avoids it via
# kwinrc [Xwayland] XwaylandEisNoPrompt=true (set in Valve's kwinrc already);
# the Frame base ships KWin 6.2.5, which ignores the key. Details and the
# base-bump rule: external-and-mods/kwin-pbos/README.md.
#
# Usage: build-kwin-6.2.5.sh <rootfs>
# Env:   STEAMOS_WORK      work dir, default /work (cache in $WORK/kwin-pbos)
#        KWIN_NO_INSTALL=1 build into the cache only, leave <rootfs> alone
#        KWIN_PATCH_DIR    patches, default external-and-mods/kwin-pbos/patches
#        KWIN_JOBS         ninja jobs, default nproc
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
WORKDIR="${STEAMOS_WORK:-/work}"
CACHE="${WORKDIR}/kwin-pbos"
PATCH_DIR="${KWIN_PATCH_DIR:-${ROOT}/external-and-mods/kwin-pbos/patches}"
JOBS="${KWIN_JOBS:-$(nproc)}"

KWIN_VER=6.2.5
KWIN_URL="https://download.kde.org/stable/plasma/${KWIN_VER}/kwin-${KWIN_VER}.tar.xz"
KWIN_SHA=5cc450a6e41105c8c49929b72550b331237f96aafb294690f4707bdc5f776848
# Extra CMake Modules is cmake-only (arch any); the Frame base has none.
ECM_URL="http://mirror.archlinuxarm.org/aarch64/extra/extra-cmake-modules-6.30.0-1-any.pkg.tar.xz"
ECM_SHA=1c3fc456c94e2606365c6c1631b8e173f6abc5ac0d74de6bb38a14c6bb2b8685

log() { printf '==> [kwin] %s\n' "$*"; }
die() { printf 'ERROR: [kwin] %s\n' "$*" >&2; exit 1; }

command -v bwrap >/dev/null || die "bwrap required (apt install bubblewrap)"
[[ -x "$R/usr/bin/cmake" && -x "$R/usr/bin/g++" && -x "$R/usr/bin/ninja" ]] \
  || die "rootfs needs cmake, g++ and ninja"
[[ -d "$PATCH_DIR" ]] || die "no patch dir $PATCH_DIR"

# --- which KWin does the base ship? -----------------------------------------
desc="$(ls -d "$R"/usr/lib/holo/pacmandb/local/kwin-[0-9]*/desc 2>/dev/null | head -1 || true)"
[[ -n "$desc" ]] || desc="$(ls -d "$R"/var/lib/pacman/local/kwin-[0-9]*/desc 2>/dev/null | head -1 || true)"
[[ -n "$desc" ]] || die "no kwin package in the rootfs pacman database"
base_ver="$(sed -n '/^%VERSION%/{n;p;}' "$desc")"
base_up="${base_ver%%-*}"
case "$base_up" in
  "$KWIN_VER") ;;
  *)
    newest="$(printf '%s\n6.4.0\n' "$base_up" | sort -V | tail -1)"
    if [[ "$newest" == "$base_up" ]]; then
      log "base KWin is ${base_ver}: XwaylandEisNoPrompt is upstream there, nothing to build."
      log "Remove scripts/build-kwin-6.2.5.sh and external-and-mods/kwin-pbos (see its README)."
      exit 0
    fi
    die "base KWin is ${base_ver}, this backport is for ${KWIN_VER}: re-check the patches"
    ;;
esac

# --- sources ----------------------------------------------------------------
mkdir -p "$CACHE"
TGZ="$CACHE/kwin-${KWIN_VER}.tar.xz"
ECM_PKG="$CACHE/$(basename "$ECM_URL")"
fetch() { # url dest sha
  if [[ ! -f "$2" ]] || [[ "$(sha256sum "$2" | cut -d' ' -f1)" != "$3" ]]; then
    log "downloading $(basename "$2")"
    curl -fsSL --max-time 600 -o "$2.part" "$1" && mv "$2.part" "$2"
  fi
  [[ "$(sha256sum "$2" | cut -d' ' -f1)" == "$3" ]] || die "checksum mismatch: $2"
}
fetch "$KWIN_URL" "$TGZ" "$KWIN_SHA"
fetch "$ECM_URL" "$ECM_PKG" "$ECM_SHA"

# One stamp per (source, patches, base package, flags): rebuilt only when
# one of them changes, so image builds reuse the cached package.
stamp_input="$(
  echo "kwin $KWIN_SHA"
  echo "ecm $ECM_SHA"
  echo "base $base_ver"
  for p in "$PATCH_DIR"/*.patch; do sha256sum "$p" | cut -d' ' -f1; done
  echo "flags v1"
)"
STAMP="$(printf '%s' "$stamp_input" | sha256sum | cut -c1-16)"
STAGE="$CACHE/stage-${STAMP}"

build() {
  local src="$CACHE/src" bld="$CACHE/build" ecm="$CACHE/ecm" stage="$STAGE.part"
  rm -rf "$src" "$bld" "$ecm" "$stage"
  mkdir -p "$src" "$bld" "$ecm" "$stage"
  log "unpacking kwin-${KWIN_VER}"
  tar -xJf "$TGZ" -C "$src"
  tar -xJf "$ECM_PKG" -C "$ecm" usr/share/ECM
  local tree="$src/kwin-${KWIN_VER}"
  for p in "$PATCH_DIR"/*.patch; do
    log "patch $(basename "$p")"
    patch -p1 -s -d "$tree" -i "$p"
  done
  run() {
    bwrap --ro-bind "$R" / \
      --dev /dev --proc /proc --tmpfs /tmp --tmpfs /run \
      --bind "$tree" /tmp/src \
      --bind "$bld" /tmp/build \
      --bind "$stage" /tmp/stage \
      --ro-bind "$ecm/usr" /tmp/ecm \
      --unshare-pid --die-with-parent --chdir /tmp/src \
      --setenv PATH /usr/bin --setenv HOME /tmp \
      --setenv CMAKE_PREFIX_PATH /tmp/ecm \
      "$@"
  }
  log "configuring"
  run /usr/bin/cmake -S /tmp/src -B /tmp/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_LIBEXECDIR=lib \
    -DBUILD_TESTING=OFF
  grep -q 'HAVE_XWAYLAND_ENABLE_EI_PORTAL 1' "$bld/src/config-kwin.h" \
    || die "cmake did not detect Xwayland -enable-ei-portal support"
  log "building with $JOBS jobs"
  run /usr/bin/cmake --build /tmp/build -j"$JOBS"
  log "installing into the cache"
  run env DESTDIR=/tmp/stage /usr/bin/cmake --install /tmp/build
  [[ -f "$stage/usr/lib/libkwin.so.${KWIN_VER}" && -f "$stage/usr/bin/kwin_wayland" \
     && -f "$stage/usr/lib/qt6/plugins/kwin/plugins/eis.so" ]] \
    || die "build output incomplete"
  rm -rf "$src" "$bld" "$ecm"
  printf '%s\n' "$stamp_input" > "$stage/.pbos-kwin-stamp"
  mv "$stage" "$STAGE"
}

if [[ -f "$STAGE/.pbos-kwin-stamp" ]]; then
  log "cached build $STAGE"
else
  build
fi

[[ "${KWIN_NO_INSTALL:-0}" == 1 ]] && { log "KWIN_NO_INSTALL=1: not installed into $R"; exit 0; }

# --- install over the base's kwin -------------------------------------------
# Every file the package ships is replaced by our build so the library, the
# binaries and the plugins come from one tree (same ABI among themselves).
# A reused build rootfs keeps the stamp of what it got last time.
if [[ -f "$R/usr/share/kwin/pbos-kwin-stamp" ]] \
   && [[ "$(cat "$R/usr/share/kwin/pbos-kwin-stamp")" == "$stamp_input" ]]; then
  log "already installed in $R (same source, patches and base)"
  exit 0
fi
log "installing over kwin ${base_ver} in $R"
( cd "$STAGE/usr" && find . -type f -o -type l ) | while read -r f; do
  f="${f#./}"
  mkdir -p "$R/usr/$(dirname "$f")"
  cp -a "$STAGE/usr/$f" "$R/usr/$f"
done
printf '%s\n' "$stamp_input" > "$R/usr/share/kwin/pbos-kwin-stamp"
log "OK: kwin ${KWIN_VER} + Xwayland libei socket patches installed"
