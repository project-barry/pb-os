#!/usr/bin/env bash
# Build PB-OS's PipeWire ALSA plugin (libspa-alsa.so) inside the Frame rootfs
# (glibc 2.39), like build-mangoapp-in-rootfs.sh.
#
# Our speakers run the ALSA sink IRQ-driven (api.alsa.disable-tsched). In
# that mode one refused graph cycle left the sink silent for good (the Thor
# sound dropout). The source is upstream PipeWire at the version in
# external-and-mods/pipewire-alsa/REF, which must match the rootfs' PipeWire,
# plus the patches next to it. Only the plugin is built; the rest of
# PipeWire stays stock.
#
# The rootfs is never written: the build runs in a throwaway overlay of it.
#
# Usage: build-spa-alsa-in-rootfs.sh <rootfs> [build-dir]
# Output: <build-dir>/libspa-alsa.so (stripped) and <build-dir>/stamp
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODS="${ROOT}/external-and-mods/pipewire-alsa"
R="$(cd "${1:?rootfs}" && pwd)"
WORKDIR="${STEAMOS_WORK:-/work}"
BUILD="${2:-${WORKDIR}/spa-alsa-build}"
CLONE="${WORKDIR}/pipewire-src"
OVL="${WORKDIR}/spa-alsa-ovl"
MESON_VER=1.9.1
MESON="${WORKDIR}/meson-${MESON_VER}"
REPO=https://gitlab.freedesktop.org/pipewire/pipewire.git
REF="$(tr -d '[:space:]' < "${MODS}/REF")"

log() { printf '==> [spa-alsa] %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

command -v bwrap >/dev/null || die "bwrap required (apt install bubblewrap)"
[[ -x "$R/usr/bin/gcc" && -x "$R/usr/bin/ninja" ]] || die "rootfs needs gcc+ninja"
# The plugin must match the PipeWire it is loaded into.
have="$(sed -n 's/^Version: //p' "$R/usr/lib/pkgconfig/libpipewire-0.3.pc")"
[[ "$have" == "$REF" ]] || die "rootfs has PipeWire $have, external-and-mods/pipewire-alsa/REF is $REF: move REF and refresh the patches"

# Same source + patches + rootfs → reuse the last build.
stamp="$(cat "${MODS}/REF" "${MODS}"/*.patch "${BASH_SOURCE[0]}" | sha256sum | cut -c1-16) $(cat "$R/usr/lib/os-release" | grep '^BUILD_ID=')"
if [[ -f "$BUILD/libspa-alsa.so" && "$(cat "$BUILD/stamp" 2>/dev/null)" == "$stamp" ]]; then
  log "up to date: $BUILD/libspa-alsa.so"
  exit 0
fi

g=(git -c safe.directory='*' -C "$CLONE")
if [[ ! -d "$CLONE/.git" ]]; then
  log "cloning $REPO"
  git clone -q "$REPO" "$CLONE"
fi
"${g[@]}" cat-file -e "${REF}^{commit}" 2>/dev/null || "${g[@]}" fetch -q --tags origin
"${g[@]}" -c advice.detachedHead=false checkout -q -f "$REF"
"${g[@]}" clean -qfdx
for p in "${MODS}"/*.patch; do
  log "patch $(basename "$p")"
  "${g[@]}" apply "$p"
done

if [[ ! -f "$MESON/meson.py" ]]; then
  log "meson ${MESON_VER}"
  curl -sfL "https://github.com/mesonbuild/meson/releases/download/${MESON_VER}/meson-${MESON_VER}.tar.gz" \
    | tar -xz -C "$WORKDIR"
fi

cleanup() {
  mountpoint -q "$OVL/root" && umount "$OVL/root"
  rm -rf "$OVL"
}
trap cleanup EXIT
cleanup
mkdir -p "$OVL/upper" "$OVL/wk" "$OVL/root"
mount -t overlay overlay -o "lowerdir=$R,upperdir=$OVL/upper,workdir=$OVL/wk" "$OVL/root"
mkdir -p "$OVL/root/mnt/src" "$OVL/root/mnt/build" "$OVL/root/opt/meson"

rm -rf "$BUILD"
mkdir -p "$BUILD"
run() {
  bwrap --bind "$OVL/root" / \
    --bind "$CLONE" /mnt/src --bind "$BUILD" /mnt/build --bind "$MESON" /opt/meson \
    --dev /dev --proc /proc --tmpfs /run --tmpfs /tmp \
    --unshare-pid --unshare-net --die-with-parent --chdir /mnt/src \
    --setenv PATH /usr/bin --setenv HOME /tmp \
    "$@"
}
# Same plugin features as the distro build: udev enumeration and compress
# offload.
log "meson setup (Frame rootfs, PipeWire $REF)"
run python3 /opt/meson/meson.py setup /mnt/build/meson --buildtype=plain \
  -Dc_args='-O2 -g' -Db_ndebug=if-release \
  -Dauto_features=disabled '-Dsession-managers=[]' \
  -Dspa-plugins=enabled -Dalsa=enabled -Dudev=enabled -Dcompress-offload=enabled
log "ninja"
run ninja -C /mnt/build/meson spa/plugins/alsa/libspa-alsa.so
run strip -o /mnt/build/libspa-alsa.so /mnt/build/meson/spa/plugins/alsa/libspa-alsa.so
[[ -f "$BUILD/libspa-alsa.so" ]] || die "no libspa-alsa.so"
if strings "$BUILD/libspa-alsa.so" | grep -q 'GLIBC_2\.4[0-9]'; then
  die "libspa-alsa.so links against a newer glibc than the Frame"
fi
printf '%s\n' "$stamp" > "$BUILD/stamp"
log "OK: $BUILD/libspa-alsa.so"
