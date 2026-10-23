#!/usr/bin/env bash
# PB-OS Quick Access: the PB-OS tab in Steam's Quick Access menu (was the
# PB-OS Tools and Barry Launcher Decky plugins), on every device.
#   /usr/lib/pbos-qam/pbos-qamd         the daemon, built here in the rootfs; also
#                                       the stick light engine (--lights, its own unit)
#   /usr/share/pbos-qam/pbos-qam.js     the frontend loader (prebuilt, frontend/build.sh)
#   /usr/share/pbos-qam/sections/*.js   each section's frontend
#   /usr/share/pbos-qam/store/catalog.json  the App Store's apps
#   /usr/share/pbos-qam/store/art/ID/       their Steam artwork
# Usage: install-pbos-qam.sh <rootfs>
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
SRC="${ROOT}/external-and-mods/pbos-qam"
OVL="${ROOT}/steamos-overlay"
HOME_DST="${STEAMOS_HOME:-$R/home/steamos}"

log() { echo "== pbos-qam: $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

[[ -f "$SRC/frontend/dist/pbos-qam.js" ]] || die "missing $SRC/frontend/dist/pbos-qam.js (run frontend/build.sh)"
rm -rf "$R/usr/lib/pbos-qam" "$R/usr/share/pbos-qam"
"$SRC/build.sh" "$R" "$R/usr/lib/pbos-qam/pbos-qamd" \
  || die "cannot build pbos-qamd (the rootfs needs g++, systemd and libarchive headers)"
install -D -m 0644 "$SRC/frontend/dist/pbos-qam.js" "$R/usr/share/pbos-qam/pbos-qam.js"
for f in "$SRC"/sections/*.js; do
  install -D -m 0644 "$f" "$R/usr/share/pbos-qam/sections/$(basename "$f")"
done
# The App Store's catalog (sections/store.js, daemon/store.cpp).
install -D -m 0644 "$SRC/store/catalog.json" "$R/usr/share/pbos-qam/store/catalog.json"
for d in "$SRC"/store/art/*/; do
  for f in "$d"*; do
    install -D -m 0644 "$f" "$R/usr/share/pbos-qam/store/art/$(basename "$d")/$(basename "$f")"
  done
done
install -m 0644 "$OVL/usr/lib/systemd/system/pbos-qamd.service" \
  "$R/usr/lib/systemd/system/pbos-qamd.service"
mkdir -p "$R/usr/lib/systemd/system/multi-user.target.wants"
ln -sfn ../pbos-qamd.service "$R/usr/lib/systemd/system/multi-user.target.wants/pbos-qamd.service"
ln -sfn ../lib/pbos-qam/pbos-qamd "$R/usr/bin/pbos-qamd"
# The Decky plugins it replaces, from a reused home.
rm -rf "$HOME_DST/homebrew/plugins/pbos-utils" "$HOME_DST/homebrew/plugins/dual-screen"
log "installed $(du -sh "$R/usr/lib/pbos-qam" | cut -f1) daemon, $(du -sh "$R/usr/share/pbos-qam" | cut -f1) frontend"
