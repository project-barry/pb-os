#!/usr/bin/env bash
# Motion sensors (accelerometer and gyroscope) for the Steam Deck controller
# on SM8550 handhelds (and the Retroid Pocket Duo) whose sensors sit behind the ADSP's sensor hub
# (external-and-mods/pbos-motion, sm8550-overlay/.../sm8550-sensors):
#   /usr/lib/sm8550-sensors/{hexagonrpcd,sscregistrygen,pbos-motiond}
#   /usr/share/sm8550-sensors/<device>/   stock sensor config per device
#   sm8550-sensors(-root).service, pbos-motiond.service
# Each unit checks the device itself (sm8550-sensors profile).
# Usage: install-pbos-motion.sh <rootfs>          install
#        install-pbos-motion.sh <rootfs> --remove  take it out again
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
OVL="${ROOT}/sm8550-overlay"
UNITS=(sm8550-sensors.service sm8550-sensors-root.service pbos-motiond.service)
RULES=76-rpduo-sensors.rules

log() { echo "== pbos-motion: $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

rm -rf "$R/usr/lib/sm8550-sensors" "$R/usr/share/sm8550-sensors"
rm -f "$R/usr/lib/steamos-sm8550/sm8550-sensors" "$R/usr/lib/udev/rules.d/$RULES" \
  "$R/usr/lib/systemd/system/multi-user.target.wants/sm8550-sensors.service"
for u in "${UNITS[@]}"; do rm -f "$R/usr/lib/systemd/system/$u"; done
if [[ "${2:-}" == --remove ]]; then
  log "removed"
  exit 0
fi

"${ROOT}/external-and-mods/pbos-motion/build.sh" "$R" "$R/usr/lib/sm8550-sensors" \
  || die "cannot build the motion sensor stack (the rootfs needs g++, meson, ninja and json-c)"
install -D -m 0755 "$OVL/usr/lib/steamos-sm8550/sm8550-sensors" "$R/usr/lib/steamos-sm8550/sm8550-sensors"
mkdir -p "$R/usr/share/sm8550-sensors"
cp -a "$OVL/usr/share/sm8550-sensors/." "$R/usr/share/sm8550-sensors/"
for u in "${UNITS[@]}"; do
  install -m 0644 "$OVL/usr/lib/systemd/system/$u" "$R/usr/lib/systemd/system/$u"
done
install -D -m 0644 "$OVL/usr/lib/udev/rules.d/$RULES" "$R/usr/lib/udev/rules.d/$RULES"
mkdir -p "$R/usr/lib/systemd/system/multi-user.target.wants"
ln -sfn ../sm8550-sensors.service "$R/usr/lib/systemd/system/multi-user.target.wants/sm8550-sensors.service"
log "installed for: $(ls "$R/usr/share/sm8550-sensors" | xargs)"
