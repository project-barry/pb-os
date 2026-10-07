#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Install the SteamOS running from the microSD card to internal UFS.
# SM8650 (KONKR Pocket FIT, AYANEO Pocket S2) and SM8550 (Retroid Pocket 6,
# AYN Thor) with ROCKNIX ABL.
#
#   ... | userdata (Android, you pick) | ROCKNIX 2G | STORAGE 20G | HOME (rest) |
#
# Android's partitions stay as they are; only userdata's end moves, and it is
# erased. The old partition table is saved to the SD card's /boot/ufs-backup
# first, and `ufs-partition.py restore` puts it back.
#
# Usage (from SteamOS on the SD card, as root):
#   install-masios-to-internal.sh                  interactive
#   install-masios-to-internal.sh --android-gb 32
#   install-masios-to-internal.sh --dry-run        show the plan, write nothing
#   install-masios-to-internal.sh --resume         partitions exist: copy again
#
# Options:
#   --android-gb N     Android userdata size (GiB)
#   --storage-gb N     SteamOS system partition (GiB, default 20)
#   --home all|essentials|none
#                      what to bring from the SD's /home (default all);
#                      essentials leaves installed games behind
#   --expect FP        only write if the table fingerprint is still FP (GUI)
#   --force            no confirmation prompt
set -euo pipefail
export PATH="/usr/sbin:/usr/bin:/sbin:/bin:${PATH:-}"

HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
PART=(python3 "$HERE/ufs-partition.py")
BOOTIMG=(python3 "$HERE/ufs-bootimg.py")

ANDROID_GB=""
STORAGE_GB=20
HOME_MODE=all
EXPECT=""
DRY_RUN=0
FORCE=0
RESUME=0
DISK=""
WORK=/run/ufs-install
BACKUP_DIR=/boot/ufs-backup

log()  { printf '\033[1;32m[ufs]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[ufs] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }
usage() { sed -n '3,25p' "$0" | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --android-gb) ANDROID_GB="${2:?}"; shift 2 ;;
    --storage-gb) STORAGE_GB="${2:?}"; shift 2 ;;
    --home) HOME_MODE="${2:?}"; shift 2 ;;
    --expect) EXPECT="${2:?}"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    --force) FORCE=1; shift ;;
    --resume|--deploy-only) RESUME=1; shift ;;
    --disk) DISK="${2:?}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option $1 (see --help)" ;;
  esac
done
case "$HOME_MODE" in all|essentials|none) ;; *) die "--home all|essentials|none" ;; esac

# ------------------------------------------------------------ preflight ----
[[ $EUID -eq 0 ]] || die "run as root (sudo)"
for t in sfdisk mkfs.vfat mkfs.ext4 rsync findmnt lsblk python3 md5sum blockdev; do
  command -v "$t" >/dev/null || die "missing tool: $t"
done

# Both SoCs use the same ROCKNIX ABL layout (userdata, then ROCKNIX + STORAGE).
tr '\0' '\n' </sys/firmware/devicetree/base/compatible | grep -qxE 'qcom,sm(8650|8550)' \
  || die "this installer is for SM8650 (KONKR Pocket FIT, AYANEO Pocket S2) and SM8550 (Retroid Pocket 6 and Nova, AYN Thor) devices"
MODEL="$(tr -d '\0' </sys/firmware/devicetree/base/model)"

# The initramfs carries no modules, so root on UFS needs the drivers built in.
for m in ufshcd_core ufshcd_pltfrm ufs_qcom phy_qcom_qmp_ufs sd_mod; do
  [[ -e /sys/module/$m/initstate ]] && die "$m is a loadable module; this kernel can't boot from UFS"
done

ROOT_SRC="$(findmnt -no SOURCE /)"
ROOT_DISK="/dev/$(lsblk -no PKNAME "$ROOT_SRC" | head -1)"
[[ "$ROOT_DISK" == /dev/mmcblk* ]] || die "run this from the microSD install (/ is on $ROOT_SRC)"

[[ -f /boot/KERNEL ]] || die "/boot/KERNEL not found (is the SD's boot partition mounted?)"
"${BOOTIMG[@]}" check /boot/KERNEL >/dev/null \
  || die "the SD's KERNEL can't boot from a partition label; update the SD image first"

if [[ -z "$DISK" ]]; then
  for d in /dev/sd[a-z]; do
    [[ -b "$d" && "$d" != "$ROOT_DISK" ]] || continue
    lsblk -rno PARTLABEL "$d" 2>/dev/null | grep -qx userdata && { DISK="$d"; break; }
  done
fi
[[ -n "$DISK" ]] || die "no internal disk with a userdata partition found"
[[ "$DISK" != "$ROOT_DISK" ]] || die "refusing to install onto the disk we're running from"

# ---------------------------------------------------------------- inspect --
eval "$("${PART[@]}" detect --disk "$DISK" --storage-gib "$STORAGE_GB" \
       | sed -n "s/^\([A-Z_]*\)=\([^']*\)$/P_\1='\2'/p")"
log "$MODEL: internal disk $DISK (${P_DISK_GIB} GiB, ${P_SECTOR_SIZE}-byte sectors)"

case "$P_MODE" in
  fresh)
    (( RESUME )) && die "--resume needs an existing install; this disk has none" ;;
  installed)
    (( RESUME )) || die "SteamOS partitions already exist. Use SteamOS Update to preserve games and saves. --resume reformats system and HOME, erasing internal user data. For a fresh install, remove them first in the ABL menu (UNINSTALL CFW)." ;;
  occupied)
    die "unknown partitions after userdata (${P_OCCUPIED}). Remove them with the ABL menu (UNINSTALL CFW) first; nothing was changed." ;;
  toosmall)
    die "not enough room after userdata for SteamOS" ;;
  *) die "unexpected disk state ${P_MODE}" ;;
esac
[[ -z "$EXPECT" || "$EXPECT" == "$P_TABLE_FINGERPRINT" ]] \
  || die "the partition table changed since it was checked; check again"

part_of() {   # GPT name -> /dev node on $DISK, read from the table (not udev)
  "${PART[@]}" detect --disk "$DISK" --storage-gib "$STORAGE_GB" | sed -n "s/^NODE_$1=//p"
}

# shellcheck source=home-copy.sh
source "$HERE/home-copy.sh"

# ------------------------------------------------------------------ plan ---
if (( ! RESUME )); then
  if [[ -z "$ANDROID_GB" ]]; then
    (( FORCE )) && die "--android-gb is required with --force"
    echo
    echo "Android keeps its system; its user data (apps, photos, accounts) is erased."
    echo "Android userdata can be ${P_ANDROID_MIN_GIB}..${P_ANDROID_MAX_GIB} GiB; SteamOS gets the rest."
    read -r -p "Android size in GiB: " ANDROID_GB
  fi
  [[ "$ANDROID_GB" =~ ^[0-9]+$ ]] || die "Android size must be a whole number of GiB"
  (( ANDROID_GB >= P_ANDROID_MIN_GIB && ANDROID_GB <= P_ANDROID_MAX_GIB )) \
    || die "Android size must be ${P_ANDROID_MIN_GIB}..${P_ANDROID_MAX_GIB} GiB"
  PLAN="$("${PART[@]}" apply --disk "$DISK" --storage-gib "$STORAGE_GB" \
          --android-gib "$ANDROID_GB" --expect "$P_TABLE_FINGERPRINT" \
          --backup-dir "$BACKUP_DIR" --dry-run | sed -n '1,4p')"
  home_size=$(awk '/^HOME/ {print $5}' <<<"$PLAN")
  home_size=$(( home_size * P_SECTOR_SIZE ))
else
  home_size=$(blockdev --getsize64 "$(part_of HOME)")
fi

need=$(home_bytes "$HOME_MODE")
root_used=$(df -B1 --output=used / | tail -1)
(( root_used < STORAGE_GB * (1 << 30) * 9 / 10 )) \
  || die "the SD system ($(( root_used >> 30 )) GiB) doesn't fit a ${STORAGE_GB} GiB STORAGE"
(( need < home_size * 95 / 100 )) \
  || die "/home to copy ($(( need >> 30 )) GiB) doesn't fit HOME ($(( home_size >> 30 )) GiB); try --home essentials"

echo
echo "Plan for $DISK:"
if (( RESUME )); then
  echo "  REINSTALL: reformat ROCKNIX / STORAGE / HOME; all internal Linux games, saves and login data are ERASED"
else
  sed 's/^/  /' <<<"$PLAN"
  echo "  Android userdata is ERASED (Android sets itself up again on its next boot)"
  echo "  the old partition table is saved to the SD card first: $BACKUP_DIR"
fi
echo "  system: a copy of this SD install ($(( root_used >> 30 )) GiB)"
case "$HOME_MODE" in
  all) echo "  /home: copied from the SD card, games included ($(( need >> 30 )) GiB)" ;;
  essentials) echo "  /home: copied without installed games ($(( need >> 30 )) GiB)" ;;
  none) echo "  /home: fresh, no account or personal data (Steam starts from its first-run setup)" ;;
esac
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

if (( ! RESUME )); then
  log "repartitioning (one write; the old table goes to $BACKUP_DIR first)"
  "${PART[@]}" apply --disk "$DISK" --storage-gib "$STORAGE_GB" \
    --android-gib "$ANDROID_GB" --expect "$P_TABLE_FINGERPRINT" --backup-dir "$BACKUP_DIR"
fi
udevadm settle -t 10 >/dev/null 2>&1 || true
RK="$(part_of ROCKNIX)"; ST="$(part_of STORAGE)"; HM="$(part_of HOME)"
[[ -b "$RK" && -b "$ST" && -b "$HM" ]] || die "new partitions didn't show up; reboot to the SD and run with --resume"
for p in "$RK" "$ST" "$HM"; do
  if findmnt -rno TARGET -S "$p" >/dev/null; then die "$p is mounted; unmount it first"; fi
done

log "formatting"
mkfs.vfat -F 32 -S "$P_SECTOR_SIZE" -n ROCKNIX "$RK" >/dev/null
mkfs.ext4 -F -q -L STORAGE -m 1 "$ST"
mkfs.ext4 -F -q -L home -m 0 "$HM"

mnt "$RK" "$WORK/boot"
mnt "$ST" "$WORK/root"
mnt "$HM" "$WORK/home"
# The SD's root filesystem alone, read-only: no /proc, /run, /home or /boot
# submounts, and /etc as its two overlay layers rather than the merged view,
# so the copy is exactly what's on the card.
mnt / "$WORK/src" --bind
mount -o remount,bind,ro "$WORK/src"

log "copying the system (a few minutes)"
rsync -aAXH --numeric-ids --info=progress2 "$WORK/src/" "$WORK/root/"

FSTAB='# SteamOS on internal UFS (partitions by GPT name)
PARTLABEL=STORAGE  /      ext4  defaults,noatime                  0 1
PARTLABEL=ROCKNIX  /boot  vfat  defaults,umask=0077,nofail        0 2
PARTLABEL=HOME     /home  ext4  defaults,noatime,commit=30        0 2
'
printf '%s' "$FSTAB" >"$WORK/root/etc/fstab"
if [[ -d "$WORK/root/var/lib/overlays/etc/upper" ]]; then
  printf '%s' "$FSTAB" >"$WORK/root/var/lib/overlays/etc/upper/fstab"
fi
# HOME already fills its partition; the SD's first-boot grow must not run.
mkdir -p "$WORK/root/var/lib/steamos-sm8550"
touch "$WORK/root/var/lib/steamos-sm8550/home-expanded"

case "$HOME_MODE" in
  all) log "copying /home (games included)" ;;
  essentials) log "copying /home without installed games" ;;
  none) log "copying a fresh /home (Steam client and Decky, no account or personal data)" ;;
esac
copy_home "$HOME_MODE" "$WORK/home"

log "installing KERNEL (root=PARTLABEL=STORAGE)"
"${BOOTIMG[@]}" retarget /boot/KERNEL "$WORK/boot/KERNEL" --root PARTLABEL=STORAGE >/dev/null
(cd "$WORK/boot" && md5sum KERNEL >KERNEL.md5)
if compgen -G "$BACKUP_DIR/ufs-gpt-*" >/dev/null; then
  mkdir -p "$WORK/home/.ufs-backup"
  cp "$BACKUP_DIR"/ufs-gpt-* "$WORK/home/.ufs-backup/"
fi
sync

# ----------------------------------------------------------------- verify --
log "checking the result"
info="$("${BOOTIMG[@]}" info "$WORK/boot/KERNEL")"
grep -q 'root=PARTLABEL=STORAGE' <<<"$info" || die "KERNEL cmdline is wrong"
grep -q '^ID_OK=yes' <<<"$info" || die "KERNEL header checksum is wrong"
(cd "$WORK/boot" && md5sum -c --quiet KERNEL.md5) || die "KERNEL.md5 mismatch"
[[ -e "$WORK/root/sbin/init" || -L "$WORK/root/sbin/init" ]] || die "system copy is incomplete (no /sbin/init)"
cmp -s <(printf '%s' "$FSTAB") "$WORK/root/etc/fstab" || die "fstab not written"
[[ -d "$WORK/home/steamos" ]] || die "/home/steamos missing on HOME"
[[ "$(stat -c %u "$WORK/home/steamos")" == 1000 ]] || die "/home/steamos has the wrong owner"

cleanup
echo
log "done. SteamOS is on internal storage."
echo "  - Power off and take out the SD card. In the ABL menu (hold Volume Down at power-on),"
echo "    set Boot source to Internal, then boot Linux. With Boot source on SD it stops at"
echo "    \"no volumes match boot source\"."
echo "  - Android: pick it in the ABL menu; it sets itself up again (userdata was erased)."
if (( ! RESUME )); then
  echo "  - Old partition table: $BACKUP_DIR on the SD card (a copy is in /home/.ufs-backup)."
  echo "    To give the space back to Android, boot the SD card and run:"
  echo "      sudo ufs-partition.py restore --backup <that .sfdisk file>"
fi
