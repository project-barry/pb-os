#!/usr/bin/env bash
# Build uMTP-Responder (the MTP responder behind pbos-mtp.service) inside the
# Frame rootfs (glibc 2.39), like build-plymouth-in-rootfs.sh. Valve's ARM
# repos have no uMTP-Responder, and it is plain C with no dependencies beyond
# libc, libpthread, librt and libsystemd.
#
# Upstream unmodified, at the commit in external-and-mods/uMTP-Responder/REF.
# CONFIG_USB_SS_SUPPORT is required: without SuperSpeed descriptors the gadget
# enumerates on a SuperSpeed port and then sticks at UDC state `addressed`
# forever. See that folder's README for the rest.
#
# The rootfs is never written: the build runs in a throwaway overlay of it.
#
# Usage: build-umtprd-in-rootfs.sh <rootfs> [build-dir]
# Output: <build-dir>/dest (an install tree to copy into the rootfs) and
#         <build-dir>/stamp
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MODS="${ROOT}/external-and-mods/uMTP-Responder"
R="$(cd "${1:?rootfs}" && pwd)"
WORKDIR="${STEAMOS_WORK:-/work}"
BUILD="${2:-${WORKDIR}/umtprd-build}"
CLONE="${WORKDIR}/umtprd-src"
OVL="${WORKDIR}/umtprd-ovl"
REPO=https://github.com/viveris/uMTP-Responder.git
REF="$(tr -d '[:space:]' < "${MODS}/REF")"

log() { printf '==> [umtprd] %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

command -v bwrap >/dev/null || die "bwrap required (apt install bubblewrap)"
[[ -x "$R/usr/bin/gcc" && -x "$R/usr/bin/make" ]] || die "rootfs needs gcc+make"

# Same source + rootfs → reuse the last build.
stamp="$(cat "${MODS}/REF" "${BASH_SOURCE[0]}" | sha256sum | cut -c1-16) $(grep '^BUILD_ID=' "$R/usr/lib/os-release")"
if [[ -x "$BUILD/dest/usr/lib/pbos-mtp/umtprd" && "$(cat "$BUILD/stamp" 2>/dev/null)" == "$stamp" ]]; then
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

cleanup() {
  mountpoint -q "$OVL/root" && umount "$OVL/root"
  rm -rf "$OVL"
}
trap cleanup EXIT
cleanup
mkdir -p "$OVL/upper" "$OVL/wk" "$OVL/root"
mount -t overlay overlay -o "lowerdir=$R,upperdir=$OVL/upper,workdir=$OVL/wk" "$OVL/root"
mkdir -p "$OVL/root/mnt/src"

rm -rf "$BUILD"
mkdir -p "$BUILD/dest/usr/lib/pbos-mtp"
run() {
  bwrap --bind "$OVL/root" / \
    --bind "$CLONE" /mnt/src \
    --dev /dev --proc /proc --tmpfs /run --tmpfs /tmp \
    --unshare-pid --die-with-parent --chdir /mnt/src \
    --setenv PATH /usr/bin --setenv HOME /tmp \
    "$@"
}
log "make (Frame rootfs, upstream $REF)"
run env CPPFLAGS=-DCONFIG_USB_SS_SUPPORT=1 \
  make -j"$(nproc)" SYSTEMD=1 USE_SYSLOG=1 > "$BUILD/make.log" 2>&1 \
  || { tail -20 "$BUILD/make.log"; die "make failed (full log: $BUILD/make.log)"; }

# A binary without SuperSpeed descriptors looks fine here and then hangs at
# UDC state `addressed` on a real port, so check the define actually reached
# the compiler rather than trusting the environment went through bwrap.
grep -q -- '-DCONFIG_USB_SS_SUPPORT=1' "$BUILD/make.log" \
  || die "CONFIG_USB_SS_SUPPORT did not reach the compiler (see $BUILD/make.log)"

[[ -x "$CLONE/umtprd" ]] || die "no umtprd"
run strip --strip-unneeded /mnt/src/umtprd
install -m0755 "$CLONE/umtprd" "$BUILD/dest/usr/lib/pbos-mtp/umtprd"
if find "$BUILD/dest" -type f -exec strings {} + 2>/dev/null | grep -q 'GLIBC_2\.4[0-9]'; then
  die "umtprd links against a newer glibc than the Frame"
fi
printf '%s\n' "$stamp" > "$BUILD/stamp"
log "OK: $BUILD/dest"
