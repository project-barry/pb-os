#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copy the SteamOS running from internal storage onto a microSD card, the
# other direction of install-masios-to-internal.sh. The card gets the layout
# of a flashed pb-os image:
#
#   | BOOT 512M (FAT, KERNEL) | root (ext4) | home (ext4, rest of the card) |
#
# and boots with Boot source set to SD in the ABL menu. Internal storage is
# only read; the card is erased.
#
# Usage (from SteamOS on internal storage, as root):
#   copy-to-sd.sh --probe           describe the card as KEY=VALUE, write nothing
#   copy-to-sd.sh --dry-run         show the plan, write nothing
#   copy-to-sd.sh                   asks before erasing the card
#
# Options:
#   --card DEV         the card (default: the one microSD card, /dev/mmcblkN)
#   --home all|essentials|none
#                      what to bring from /home (default all); essentials
#                      leaves installed games behind
#   --expect CID       only write if the card is still the one with this CID (GUI)
#   --force            no confirmation prompt
set -euo pipefail
export PATH="/usr/sbin:/usr/bin:/sbin:/bin:${PATH:-}"

HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
BOOTIMG=(python3 "$HERE/ufs-bootimg.py")
# shellcheck source=home-copy.sh
source "$HERE/home-copy.sh"

CARD=""
HOME_MODE=all
EXPECT=""
PROBE=0
DRY_RUN=0
FORCE=0
WORK=/run/copy-to-sd
GIB=$(( 1 << 30 ))
MIB=$(( 1 << 20 ))
BOOT_MIB=512
# The system partition gets what internal storage gives it (STORAGE), or
# more when the system has grown past that.
ROOT_MIN_BYTES=$(( 20 * GIB ))
ROOT_FREE_BYTES=$(( 4 * GIB ))
MIN_HOME_BYTES=$(( 8 * GIB ))

log()  { printf '\033[1;32m[ufs]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[ufs] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
usage() { sed -n '3,24p' "$0" | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --card) CARD="${2:?}"; shift 2 ;;
    --home) HOME_MODE="${2:?}"; shift 2 ;;
    --expect) EXPECT="${2:?}"; shift 2 ;;
    --probe) PROBE=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    --force) FORCE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option $1 (see --help)" ;;
  esac
done
case "$HOME_MODE" in all|essentials|none) ;; *) die "--home all|essentials|none" ;; esac

# --probe reports problems as ERROR= lines, for PB-OS Utils.
fail() {
  if (( PROBE )); then echo "ERROR=$*"; exit 0; fi
  die "$*"
}

# ------------------------------------------------------------ preflight ----
[[ $EUID -eq 0 ]] || die "run as root (sudo)"
for t in sfdisk wipefs mkfs.vfat mkfs.ext4 rsync findmnt lsblk python3 md5sum blockdev flock uuidgen; do
  command -v "$t" >/dev/null || die "missing tool: $t"
done
tr '\0' '\n' </sys/firmware/devicetree/base/compatible | grep -qxE 'qcom,sm(8650|8550)' \
  || die "this tool is for SM8650 and SM8550 devices with ROCKNIX ABL"
MODEL="$(tr -d '\0' </sys/firmware/devicetree/base/model)"

ROOT_SRC="$(findmnt -no SOURCE /)"
ROOT_DISK="/dev/$(lsblk -no PKNAME "$ROOT_SRC" | head -1)"
[[ "$ROOT_DISK" == /dev/sd* ]] \
  || fail "SteamOS is running from the microSD card; this copies an internal install to a card"
[[ -f /boot/KERNEL ]] || fail "/boot/KERNEL not found (is the ROCKNIX partition mounted?)"

if [[ -z "$CARD" ]]; then
  cards=()
  for d in /sys/block/mmcblk[0-9]; do
    if [[ -e "$d" && "$(cat "$d/device/type" 2>/dev/null)" == SD ]]; then cards+=("/dev/${d##*/}"); fi
  done
  (( ${#cards[@]} > 0 )) || fail "no microSD card found; put one in"
  (( ${#cards[@]} == 1 )) || fail "more than one microSD card; pick one with --card"
  CARD="${cards[0]}"
fi
[[ "$CARD" =~ ^/dev/mmcblk[0-9]$ && -b "$CARD" ]] || fail "$CARD is not a microSD card"
[[ "$CARD" != "$ROOT_DISK" ]] || fail "refusing to erase the disk SteamOS runs from"
BASE="${CARD#/dev/}"
CID="$(cat "/sys/block/$BASE/device/cid" 2>/dev/null || true)"
[[ -z "$EXPECT" || "$EXPECT" == "$CID" ]] || fail "a different card is in now; check again"
CARD_BYTES="$(blockdev --getsize64 "$CARD")"
# Never the card SteamOS's own /home, /boot or swap would be on.
for m in / /home /boot; do
  src="$(findmnt -no SOURCE "$m" 2>/dev/null || true)"
  if [[ -n "$src" && "$(lsblk -no PKNAME "$src" 2>/dev/null | head -1)" == "$BASE" ]]; then
    fail "$m is on $CARD; refusing to erase it"
  fi
done

# ---------------------------------------------------------------- plan -----
root_used=$(df -B1 --output=used / | tail -1)
root_bytes=$(( root_used + ROOT_FREE_BYTES ))
(( root_bytes > ROOT_MIN_BYTES )) || root_bytes=$ROOT_MIN_BYTES
root_bytes=$(( (root_bytes + MIB - 1) / MIB * MIB ))
# 1 MiB gap, BOOT, root, then home to the end of the card.
home_bytes=$(( (CARD_BYTES / MIB - 1 - BOOT_MIB) * MIB - root_bytes ))

parts=()
library=0
card_parts() { lsblk -rno NAME "$CARD" | tail -n +2; }
for name in $(card_parts); do
  # One field per call: lsblk's columns shift when one is empty.
  fstype="$(lsblk -dno FSTYPE "/dev/$name" 2>/dev/null || true)"
  label="$(lsblk -dno LABEL "/dev/$name" 2>/dev/null || true)"
  mountpoint="$( (findmnt -rno TARGET -S "/dev/$name" 2>/dev/null || true) | head -1 | sed 's/\\x20/ /g')"
  desc="$name ${fstype:-unformatted}"
  if [[ -n "$label" ]]; then desc+=" \"$label\""; fi
  if [[ -n "$mountpoint" && -d "$mountpoint/steamapps" ]]; then
    desc+=" (Steam library)"; library=1
  fi
  parts+=("$desc")
done

if (( PROBE )); then
  echo "CARD=$CARD"
  echo "CARD_CID=$CID"
  echo "CARD_NAME=$(cat "/sys/block/$BASE/device/name" 2>/dev/null || true)"
  echo "CARD_BYTES=$CARD_BYTES"
  echo "CARD_PARTS=$(IFS=,; echo "${parts[*]}")"
  echo "CARD_STEAM_LIBRARY=$library"
  echo "ROOT_USED_BYTES=$root_used"
  echo "ROOT_PART_BYTES=$root_bytes"
  echo "HOME_PART_BYTES=$(( home_bytes > 0 ? home_bytes : 0 ))"
  (( home_bytes >= MIN_HOME_BYTES )) \
    || echo "ERROR=the card is too small: SteamOS needs at least $(( (BOOT_MIB * MIB + root_bytes + MIN_HOME_BYTES) / GIB + 1 )) GB"
  exit 0
fi

log "$MODEL: microSD card $CARD ($(( CARD_BYTES / GIB )) GiB)"
(( home_bytes >= MIN_HOME_BYTES )) \
  || die "the card is too small: SteamOS needs at least $(( (BOOT_MIB * MIB + root_bytes + MIN_HOME_BYTES) / GIB + 1 )) GiB"
need=$(home_bytes "$HOME_MODE")
(( need < home_bytes * 95 / 100 )) \
  || die "/home to copy ($(( need >> 30 )) GiB) doesn't fit the card's home ($(( home_bytes >> 30 )) GiB); try --home essentials"

echo
echo "Plan for $CARD ($(cat "/sys/block/$BASE/device/name" 2>/dev/null || echo card)):"
echo "  ERASED: ${parts[*]:-(empty card)}"
echo "  BOOT ${BOOT_MIB} MiB | root $(( root_bytes / GIB )) GiB | home $(( home_bytes / GIB )) GiB"
echo "  system: a copy of this internal install ($(( root_used >> 30 )) GiB)"
case "$HOME_MODE" in
  all) echo "  /home: copied from internal storage, games included ($(( need >> 30 )) GiB)" ;;
  essentials) echo "  /home: copied without installed games ($(( need >> 30 )) GiB)" ;;
  none) echo "  /home: fresh, no account or personal data (Steam starts from its first-run setup)" ;;
esac
echo "  internal storage is not changed"
echo
if (( DRY_RUN )); then
  log "dry run: nothing was written"
  exit 0
fi
if (( ! FORCE )); then
  read -r -p "Type ERASE to continue: " ok
  [[ "$ok" == ERASE ]] || die "cancelled; nothing was written"
fi

# --------------------------------------------------------------- execute ---
mounted=()
cleanup() {
  local i
  for (( i=${#mounted[@]}-1; i>=0; i-- )); do umount "${mounted[i]}" 2>/dev/null || true; done
  mounted=()
}
trap cleanup EXIT
mnt() { mkdir -p "$2"; mount "${@:3}" "$1" "$2"; mounted+=("$2"); }

log "preparing the card"
# SteamOS's auto-mounter (block-device-event.sh) skips a partition while its
# lock is held: hold the locks of the old and the new partitions, so it
# neither remounts the old ones nor mounts the new ones mid-copy.
declare -A seen=()
for n in $(card_parts) "${BASE}p1" "${BASE}p2" "${BASE}p3"; do
  if [[ -n "${seen[$n]:-}" ]]; then continue; fi
  seen[$n]=1
  exec {fd}<>"/run/jupiter-automount-$n.lock"   # SteamOS: /var/run is /run
  flock -w 30 "$fd" || die "the card is busy (being mounted or formatted); try again"
done
for n in $(card_parts); do
  if [[ -x /usr/lib/hwsupport/steamos-automount.sh ]]; then
    /usr/lib/hwsupport/steamos-automount.sh remove "$n" >/dev/null 2>&1 || true
  fi
  while read -r m; do
    [[ -n "$m" ]] || continue
    for try in 1 2 3 4 5; do
      if umount "$m" 2>/dev/null; then break; fi
      (( try < 5 )) || die "$m is in use (a game running from the card?); close it and try again. Nothing was written."
      sleep 2
    done
  done < <(findmnt -rno TARGET -S "/dev/$n" | sed 's/\\x20/ /g')
done

log "partitioning the card"
DISK_ID="$(od -An -N4 -tx4 /dev/urandom | tr -d ' ')"
wipefs -a -q "$CARD"
sfdisk -q --wipe always --wipe-partitions always "$CARD" <<EOF
label: dos
label-id: 0x${DISK_ID}
unit: sectors

start=2048, size=$(( BOOT_MIB * 2048 )), type=c, bootable
size=$(( root_bytes / 512 )), type=83
type=83
EOF
blockdev --rereadpt "$CARD" 2>/dev/null || partx -u "$CARD" || true
udevadm settle -t 10 >/dev/null 2>&1 || true
BT="${CARD}p1"; RT="${CARD}p2"; HM="${CARD}p3"
for i in 1 2 3 4 5; do
  if [[ -b "$BT" && -b "$RT" && -b "$HM" ]]; then break; fi
  sleep 1
done
[[ -b "$BT" && -b "$RT" && -b "$HM" ]] || die "the card's new partitions didn't show up; take it out, put it back and try again"

log "formatting"
ROOT_UUID="$(uuidgen)"; HOME_UUID="$(uuidgen)"
mkfs.vfat -F 32 -n BOOT "$BT" >/dev/null
mkfs.ext4 -F -q -L root -U "$ROOT_UUID" -m 1 "$RT"
mkfs.ext4 -F -q -L home -U "$HOME_UUID" -m 0 "$HM"

mnt "$BT" "$WORK/boot"
mnt "$RT" "$WORK/root"
mnt "$HM" "$WORK/home"
# This install's root filesystem alone, read-only (see the installer).
mnt / "$WORK/src" --bind
mount -o remount,bind,ro "$WORK/src"

log "copying the system (a few minutes)"
rsync -aAXH --numeric-ids --info=progress2 "$WORK/src/" "$WORK/root/"

FSTAB="# SteamOS on microSD (copied from internal storage)
UUID=${ROOT_UUID}  /      ext4  defaults,noatime                         0 1
LABEL=BOOT         /boot  vfat  defaults,umask=0077,nofail               0 2
UUID=${HOME_UUID}  /home  ext4  defaults,noatime,commit=30,x-systemd.growfs 0 2
"
printf '%s' "$FSTAB" >"$WORK/root/etc/fstab"
if [[ -d "$WORK/root/var/lib/overlays/etc/upper" ]]; then
  printf '%s' "$FSTAB" >"$WORK/root/var/lib/overlays/etc/upper/fstab"
fi
# home already fills the card; the marker the internal install carries
# keeps the first-boot grow from running.
mkdir -p "$WORK/root/var/lib/steamos-sm8550"
touch "$WORK/root/var/lib/steamos-sm8550/home-expanded"
if [[ -f "$WORK/root/opt/steamos-sm8650/IMAGE.txt" ]]; then
  sed -i -e "s/^root_uuid=.*/root_uuid=${ROOT_UUID}/" \
    -e "s/^root_partuuid=.*/root_partuuid=${DISK_ID}-02/" \
    -e "s/^home_uuid=.*/home_uuid=${HOME_UUID}/" "$WORK/root/opt/steamos-sm8650/IMAGE.txt"
fi

case "$HOME_MODE" in
  all) log "copying /home (games included)" ;;
  essentials) log "copying /home without installed games" ;;
  none) log "copying a fresh /home (Steam client and Decky, no account or personal data)" ;;
esac
copy_home "$HOME_MODE" "$WORK/home"

log "installing KERNEL (root=PARTUUID=${DISK_ID}-02)"
# Whatever else is on the ROCKNIX partition (rocknix_abl, README) comes along.
rsync -r --exclude=/KERNEL --exclude=/KERNEL.md5 /boot/ "$WORK/boot/"
"${BOOTIMG[@]}" retarget /boot/KERNEL "$WORK/boot/KERNEL" --root "PARTUUID=${DISK_ID}-02" >/dev/null
(cd "$WORK/boot" && md5sum KERNEL >KERNEL.md5)
# The installer's backup of Android's partition table, so this card can give
# the space back to Android (ufs-partition.py restore) like the original.
if compgen -G "/home/.ufs-backup/ufs-gpt-*" >/dev/null; then
  mkdir -p "$WORK/boot/ufs-backup"
  cp /home/.ufs-backup/ufs-gpt-* "$WORK/boot/ufs-backup/"
fi
sync

# ----------------------------------------------------------------- verify --
log "checking the result"
info="$("${BOOTIMG[@]}" info "$WORK/boot/KERNEL")"
grep -q "root=PARTUUID=${DISK_ID}-02" <<<"$info" || die "KERNEL cmdline is wrong"
grep -q '^ID_OK=yes' <<<"$info" || die "KERNEL header checksum is wrong"
(cd "$WORK/boot" && md5sum -c --quiet KERNEL.md5) || die "KERNEL.md5 mismatch"
[[ -e "$WORK/root/sbin/init" || -L "$WORK/root/sbin/init" ]] || die "system copy is incomplete (no /sbin/init)"
cmp -s <(printf '%s' "$FSTAB") "$WORK/root/etc/fstab" || die "fstab not written"
[[ -d "$WORK/home/steamos" ]] || die "/home/steamos missing on the card"
[[ "$(stat -c %u "$WORK/home/steamos")" == 1000 ]] || die "/home/steamos has the wrong owner"

cleanup
echo
log "done. SteamOS is on the microSD card."
echo "  - Power off and keep the card in. In the ABL menu (hold Volume Down at power-on),"
echo "    set Boot source to SD, then boot Linux. Boot source Internal still starts the"
echo "    internal install, which is unchanged."
