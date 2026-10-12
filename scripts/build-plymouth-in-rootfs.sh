#!/usr/bin/env bash
# Build Plymouth (the boot splash) inside the Frame rootfs (glibc 2.39), like
# build-mangoapp-in-rootfs.sh. Valve's ARM repos have no Plymouth, and Arch
# Linux ARM's build needs glibc 2.43.
#
# Upstream 26.134.222 at the commit in external-and-mods/plymouth/REF plus the
# patches next to it (fixes from upstream that came after the release), built
# with only what the PB-OS theme needs: the DRM renderer (plus frame-buffer
# as a fallback) and the script, details and text splash plugins. No Pango,
# FreeType or GTK; the theme draws no text of its own.
#
# The rootfs is never written: the build runs in a throwaway overlay of it.
#
# Usage: build-plymouth-in-rootfs.sh <rootfs> [build-dir]
# Output: <build-dir>/dest (an install tree to copy into the rootfs) and
#         <build-dir>/stamp
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODS="${ROOT}/external-and-mods/plymouth"
R="$(cd "${1:?rootfs}" && pwd)"
WORKDIR="${STEAMOS_WORK:-/work}"
BUILD="${2:-${WORKDIR}/plymouth-build}"
CLONE="${WORKDIR}/plymouth-src"
OVL="${WORKDIR}/plymouth-ovl"
MESON_VER=1.9.1
MESON="${WORKDIR}/meson-${MESON_VER}"
REPO=https://gitlab.freedesktop.org/plymouth/plymouth.git
REF="$(tr -d '[:space:]' < "${MODS}/REF")"

log() { printf '==> [plymouth] %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

command -v bwrap >/dev/null || die "bwrap required (apt install bubblewrap)"
[[ -x "$R/usr/bin/gcc" && -x "$R/usr/bin/ninja" ]] || die "rootfs needs gcc+ninja"

# Same source + rootfs → reuse the last build.
stamp="$(cat "${MODS}/REF" "${MODS}"/*.patch "${BASH_SOURCE[0]}" | sha256sum | cut -c1-16) $(grep '^BUILD_ID=' "$R/usr/lib/os-release")"
if [[ -x "$BUILD/dest/usr/bin/plymouthd" && "$(cat "$BUILD/stamp" 2>/dev/null)" == "$stamp" ]]; then
  log "up to date: $BUILD/dest"
  exit 0
fi

g=(git -c safe.directory='*' -C "$CLONE")
if [[ ! -d "$CLONE/.git" ]]; then
  log "cloning $REPO"
  git clone -q "$REPO" "$CLONE"
fi
"${g[@]}" cat-file -e "${REF}^{commit}" 2>/dev/null || "${g[@]}" fetch -q origin
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
    --unshare-pid --die-with-parent --chdir /mnt/src \
    --setenv PATH /usr/bin --setenv HOME /tmp \
    "$@"
}
log "meson setup (Frame rootfs, upstream $REF)"
# sbindir=bin: the Frame's /usr/sbin is a symlink to bin.
run python3 /opt/meson/meson.py setup /mnt/build/meson --buildtype=release \
  --prefix=/usr --sbindir=bin --libdir=lib --libexecdir=lib \
  --sysconfdir=/etc --localstatedir=/var -Drunstatedir=/run \
  -Dpango=disabled -Dfreetype=disabled -Dgtk=disabled -Ddocs=false \
  -Ddrm=true -Dudev=enabled -Dsystemd-integration=true \
  -Dupstart-monitoring=false -Dtracing=true \
  -Dbackground-color=0x000000 -Dbackground-start-color-stop=0x000000 \
  -Dbackground-end-color-stop=0x000000
log "ninja"
run ninja -C /mnt/build/meson
run env DESTDIR=/mnt/build/dest ninja -C /mnt/build/meson install >/dev/null

d="$BUILD/dest"
[[ -x "$d/usr/bin/plymouthd" ]] || die "no plymouthd"
# Boot splash only: no initrd tools (our initramfs is busybox), no splash at
# shutdown/reboot, no upstream themes or the plugins they need.
rm -rf "$d/usr/share/man" "$d/usr/share/doc" "$d/usr/include" "$d/usr/lib/pkgconfig" \
  "$d/usr/lib/plymouth/plymouth-generate-initrd" "$d/usr/lib/plymouth/plymouth-populate-initrd" \
  "$d/usr/lib/plymouth/plymouth-update-initrd" "$d/usr/bin/plymouth-set-default-theme" \
  "$d/usr/lib/dracut" "$d/usr/lib/initcpio" "$d/etc/logrotate.d" "$d/var"
for w in initrd-switch-root reboot kexec poweroff halt; do
  rm -rf "$d/usr/lib/systemd/system/${w}.target.wants"
done
rm -f "$d"/usr/lib/systemd/system/plymouth-{halt,kexec,poweroff,reboot,switch-root,switch-root-initramfs}.service
# Settings come from plymouthd.defaults in steamos-overlay (not /etc: plymouthd
# starts before the /etc overlay is mounted). bizcom.png is a logo for upstream themes.
rm -rf "$d/etc/plymouth" "$d/usr/share/plymouth/bizcom.png" "$d/usr/share/plymouth/plymouthd.defaults"
for t in "$d"/usr/share/plymouth/themes/*; do
  case "${t##*/}" in details|text) ;; *) rm -rf "$t" ;; esac
done
for p in "$d"/usr/lib/plymouth/*.so; do
  case "${p##*/}" in script.so|details.so|text.so) ;; *) rm -f "$p" ;; esac
done
rm -f "$d"/usr/lib/plymouth/renderers/x11.so "$d"/usr/lib/*.a
run sh -c 'strip --strip-unneeded /mnt/build/dest/usr/bin/plymouth /mnt/build/dest/usr/bin/plymouthd /mnt/build/dest/usr/lib/libply*.so.*.* /mnt/build/dest/usr/lib/plymouth/*.so /mnt/build/dest/usr/lib/plymouth/renderers/*.so'
if find "$d" -type f -exec strings {} + 2>/dev/null | grep -q 'GLIBC_2\.4[0-9]'; then
  die "plymouth links against a newer glibc than the Frame"
fi
printf '%s\n' "$stamp" > "$BUILD/stamp"
log "OK: $BUILD/dest"
