#!/usr/bin/env bash
# Kernel for SteamOS ARM on Snapdragon handhelds, one SoC per build:
#   SOC=sm8650 (default)  Snapdragon 8 Gen 3 / G3 Gen 3: KONKR Pocket FIT,
#                         AYANEO Pocket S2 (same ROCKNIX dtsi)
#   SOC=sm8550            Snapdragon 8 Gen 2: Retroid Pocket 6 and Nova, AYN Thor
#
# Sources (pinned):
#   linux-${KVER}             kernel.org
#   ROCKNIX distribution      per-SoC patches, DTS, kernel config
#   ROCKNIX extra-firmware    vendor-signed ADSP/CDSP(/zap), audio tplg
#   ROCKNIX chipone_tddi      out-of-tree touchscreen driver (SM8650)
#   linux-firmware            Adreno 740 microcode + zap (SM8550)
#
# Output: output/<release>/{boot/KERNEL, modules/<release>, firmware/}
#
# KERNEL is a ROCKNIX-ABL bootimg (header v0): gzip(Image) + appended DTBs
# + a busybox initramfs (initramfs/init). ABL v1.1.8+ reads `model` from each
# DTB and boots the one matching "Set device model" (e.g. "KONKR Pocket FIT").
# The image builder patches the real root=PARTUUID= into the cmdline.
#
# Must run on aarch64 Linux (native build). Tested host: Ubuntu 24.04 in Colima.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT_ROOT="$(cd "${HERE}/../.." && pwd)"

SOC="${SOC:-sm8650}"
# ROCKNIX's released 7.2 recipe (tag 20260901) on the newest 7.2 stable
# release, for both SoCs. Their 7.2 development branch black-screened the
# Pocket FIT before the console on 2026-09-23; a GCC 13 build of 7.2 fails
# the same way on the RP6, so 7.2 is built with GCC 15 (see below).
# The previous SM8650 kernel: KVER=7.1.2 ROCKNIX_REF=20260801.
_kver=7.2.8 _rocknix_ref=20260901
KVER="${KVER:-$_kver}"
# ROCKNIX's version patch dir is named after the series (patches/7.2).
KSERIES="${KVER%.*}"
LOCALVERSION="${LOCALVERSION:--${SOC}-steamos}"
ROCKNIX_REF="${ROCKNIX_REF:-$_rocknix_ref}"
ROCKNIX_DIR="${ROCKNIX_DIR:-${PORT_ROOT}/../rocknix-${ROCKNIX_REF}}"
EXTRA_FW_REF="${EXTRA_FW_REF:-88b363e67d4f730feb2c3124724d26dfaa88ce76}"
TDDI_REF="${TDDI_REF:-af27029fa2b27c4a77d16809298ed5d03c9da5a6}"
# DTBs to append to KERNEL (ABL shows one menu entry per DTB model).
# Same order ROCKNIX appends them (alphabetical glob); ABL maps the chosen
# model back into this list.
# Port patches, DTS appends and the extra config fragment are per SoC: the
# SM8650 ones are Pocket FIT drivers (KONKR MCU, AR14 panel modes).
case "$SOC" in
  sm8650)
    DTBS="${DTBS:-sm8650-ayaneo-ps2 sm8650-konkr-pf}"
    PORT_DIR="${HERE}"
    ;;
  sm8550)
    # ABL boots the DTB whose model matches "Set device model": "AYN Thor",
    # "Retroid Pocket 6", "Retroid Pocket 6 TOP-DPAD" and "Retroid Pocket Nova"
    # (ROCKNIX's Nova DTS is the RP6 one with its own panel, touch and sticks).
    DTBS="${DTBS:-qcs8550-ayn-thor qcs8550-retroidpocket-rp6 qcs8550-retroidpocket-rp6-top-dpad qcs8550-retroidpocket-rpnova}"
    PORT_DIR="${HERE}/sm8550"
    ;;
  *) echo "unsupported SOC=${SOC} (sm8650, sm8550)" >&2; exit 1 ;;
esac
SOC_UC="${SOC^^}"
# 7.2 only boots when built with GCC 15 (see build-gcc15.sh, which runs this
# script in a Fedora 43 container). GCC 13's kernel dies before the initramfs
# on the Retroid Pocket 6.
if [[ "$KSERIES" != 7.1 && "${ALLOW_OLD_GCC:-0}" != 1 ]]; then
  _gcc_major="$(${CC:-gcc} -dumpversion 2>/dev/null | cut -d. -f1)"
  if [[ -z "$_gcc_major" || "$_gcc_major" -lt 15 ]]; then
    echo "Linux ${KVER} needs GCC 15 (found ${_gcc_major:-none}): use build-gcc15.sh" >&2
    exit 1
  fi
fi
# Adreno 740 microcode + zap for SM8550 (the Frame rootfs only has A750's).
# a740_sqe.fw here is the one MaSi's SM8550 build verified (md5 0211fdf6…);
# Armbian's copy glitches RPCS3.
LINUX_FW_REF="${LINUX_FW_REF:-20260916}"

WORK="${WORK:-/work/kernel-${SOC}}"
CACHE="${WORK}/cache"
SRC="${WORK}/linux-${KVER}"
OUT_BASE="${OUT_BASE:-${WORK}/output}"
JOBS="${JOBS:-$(nproc)}"

log() { printf '[kernel-%s] %s\n' "$SOC" "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

[[ "$(uname -m)" == aarch64 ]] || die "build on aarch64 Linux (Colima VM), not $(uname -m)"

check_deps() {
  local missing=() c
  for c in make gcc bc bison flex python3 curl tar xz gzip cpio kmod patch perl rsync; do
    command -v "$c" >/dev/null || missing+=("$c")
  done
  [[ -f /usr/include/openssl/ssl.h ]] || missing+=(libssl-dev)
  [[ -f /usr/include/gelf.h ]] || missing+=(libelf-dev)
  if ((${#missing[@]})); then
    die "missing: ${missing[*]}  (sudo apt-get install -y build-essential bc bison flex libssl-dev libelf-dev python3 curl xz-utils cpio kmod patch rsync dwarves)"
  fi
  # BTF for sched_ext (steamos.config). pahole < 1.26 (Ubuntu 24.04 ships
  # 1.25) writes kfunc prototypes scx_lavd rejects ("malformed scx kfunc
  # prototype(s)"), so SteamOS's default scheduler would silently not start.
  local pv
  pv="$(pahole --version 2>/dev/null | tr -dc 0-9)"
  [[ -n "$pv" && "$pv" -ge 126 ]] \
    || die "need pahole >= 1.26 for sched_ext BTF (have: $(pahole --version 2>&1 | head -1)); build it from git.kernel.org/pub/scm/devel/pahole/pahole.git into /usr/local"
}

fetch() {
  local url="$1" dest="$2"
  [[ -s "$dest" ]] && return 0
  mkdir -p "$(dirname "$dest")"
  log "download ${url}"
  curl -fL --retry 3 -o "${dest}.part" "$url"
  mv -f "${dest}.part" "$dest"
}

rocknix_path() { echo "${ROCKNIX_DIR}/$1"; }

prepare_source() {
  if [[ -d "$ROCKNIX_DIR/.git" ]]; then
    [[ "$(git -C "$ROCKNIX_DIR" rev-parse HEAD)" == "$(git -C "$ROCKNIX_DIR" rev-parse "${ROCKNIX_REF}^{commit}")" ]] || die "ROCKNIX checkout does not match pinned ${ROCKNIX_REF}"
  else
    die "ROCKNIX source needs Git metadata to verify the pinned revision"
  fi
  local tarball="${CACHE}/linux-${KVER}.tar.xz"
  fetch "https://cdn.kernel.org/pub/linux/kernel/v${KVER%%.*}.x/linux-${KVER}.tar.xz" "$tarball"
  local patch_digest
  local -a digest_src=() x
  for x in "${PORT_DIR}/patches" "${PORT_DIR}/dts" "${PORT_DIR}/rocknix-skip"; do
    [[ -e "$x" ]] && digest_src+=("$x")
  done
  # The DTB list is part of it: DTBs are registered in the Makefile only while
  # the source is (re)patched below.
  patch_digest="$({ find "${digest_src[@]}" -type f -print0 | sort -z | xargs -0 -r sha256sum; echo "DTBS=$DTBS"; } | sha256sum | cut -d" " -f1)"
  if [[ -f "${SRC}/.${SOC}-patched" && "$(cat "${SRC}/.${SOC}-patched")" == "$patch_digest" ]]; then
    log "source already patched: ${SRC}"
    return 0
  fi
  rm -rf "$SRC"
  mkdir -p "$WORK"
  log "extract linux-${KVER}"
  tar -C "$WORK" -xf "$tarball"

  # Same order ROCKNIX uses: PKG_PATCH_DIRS="${LINUX} mainline ${DEVICE} default"
  # (${LINUX} is the series dir, e.g. 7.2).
  # ROCKNIX patches this port leaves out (rocknix-skip: one basename per line).
  local -A skip=()
  local line
  if [[ -f "${PORT_DIR}/rocknix-skip" ]]; then
    while IFS= read -r line; do
      line="${line%%#*}"; line="${line//[[:space:]]/}"
      [[ -n "$line" ]] && skip["$line"]=1
    done <"${PORT_DIR}/rocknix-skip"
  fi
  local d p
  local -a dirs=(
    "projects/ROCKNIX/packages/linux/patches/${KSERIES}"
    "projects/ROCKNIX/packages/linux/patches/mainline"
    "projects/ROCKNIX/devices/${SOC_UC}/patches/linux"
    "packages/linux/patches/default"
    "@port"
  )
  for d in "${dirs[@]}"; do
    local pdir
    if [[ "$d" == "@port" ]]; then pdir="${PORT_DIR}/patches"; else pdir="$(rocknix_path "$d")"; fi
    [[ -d "$pdir" ]] || { log "skip missing patch dir $d"; continue; }
    for p in "$pdir"/*.patch; do
      [[ -e "$p" ]] || continue
      case "$(basename "$p")" in
        9900-i915-10bit-hack.patch) continue ;;  # x86 only
        # perf's Rust target names for ROCKNIX's toolchain; we don't build perf
        9999-fix-rust-build-error.patch) continue ;;
      esac
      if [[ "$d" != "@port" && -n "${skip[$(basename "$p")]:-}" ]]; then
        log "skip $(basename "$d")/$(basename "$p") (rocknix-skip)"
        continue
      fi
      log "patch $(basename "$d")/$(basename "$p")"
      patch -d "$SRC" -p1 -N --no-backup-if-mismatch -s <"$p" \
        || die "patch failed: $p"
    done
  done

  log "install ROCKNIX ${SOC_UC} DTS"
  cp -v "$(rocknix_path "projects/ROCKNIX/devices/${SOC_UC}/linux/dts/qcom")"/*.dts* \
    "${SRC}/arch/arm64/boot/dts/qcom/" >&2
  # DT patches go after the copy: the ROCKNIX .dts/.dtsi files are not in the
  # kernel tree while the code patches above are applied.
  for p in "${PORT_DIR}"/dts/*.patch; do
    [[ -e "$p" ]] || continue
    log "patch dts/$(basename "$p")"
    patch -d "$SRC" -p1 -N --no-backup-if-mismatch -s <"$p" \
      || die "patch failed: $p"
  done
  local app
  for app in "${PORT_DIR}"/dts/*.append; do
    [[ -e "$app" ]] || continue
    log "append $(basename "$app")"
    cat "$app" >>"${SRC}/arch/arm64/boot/dts/qcom/$(basename "$app" .append).dts"
  done
  local mk="${SRC}/arch/arm64/boot/dts/qcom/Makefile" dtb
  for dtb in $DTBS; do
    grep -q "${dtb}.dtb" "$mk" || echo "dtb-\$(CONFIG_ARCH_QCOM) += ${dtb}.dtb" >>"$mk"
  done
  printf "%s\n" "$patch_digest" > "${SRC}/.${SOC}-patched"
}

stage_builtin_firmware() {
  # GPU microcode + zap and the regulatory db are needed before the rootfs
  # is mounted, so they go into the kernel image (like ROCKNIX does).
  local fwtar="${CACHE}/extra-firmware-${EXTRA_FW_REF}.tar.gz"
  fetch "https://github.com/ROCKNIX/extra-firmware/archive/${EXTRA_FW_REF}.tar.gz" "$fwtar"
  EXTRA_FW_SRC="${WORK}/extra-firmware"
  if [[ ! -d "${EXTRA_FW_SRC}/${SOC_UC}" ]]; then
    rm -rf "$EXTRA_FW_SRC"; mkdir -p "$EXTRA_FW_SRC"
    tar -C "$EXTRA_FW_SRC" --strip-components=1 -xzf "$fwtar"
  fi
  local regdb="${CACHE}/wireless-regdb"
  if [[ ! -f "${regdb}/regulatory.db" ]]; then
    fetch "https://git.kernel.org/pub/scm/linux/kernel/git/wens/wireless-regdb.git/plain/regulatory.db" "${regdb}/regulatory.db"
    fetch "https://git.kernel.org/pub/scm/linux/kernel/git/wens/wireless-regdb.git/plain/regulatory.db.p7s" "${regdb}/regulatory.db.p7s"
  fi
  local ext="${SRC}/external-firmware"
  rm -rf "$ext"
  case "$SOC" in
    sm8650)
      mkdir -p "${ext}/qcom/sm8650/ayaneo/ps2"
      cp -L "${EXTRA_FW_SRC}/SM8650/qcom/"{gen70900_aqe.fw,gen70900_sqe.fw,gmu_gen70900.bin} "${ext}/qcom/"
      cp -L "${EXTRA_FW_SRC}/SM8650/qcom/sm8650/ayaneo/ps2/gen70900_zap.mbn" "${ext}/qcom/sm8650/ayaneo/ps2/"
      ;;
    sm8550)
      local lfw="${CACHE}/linux-firmware-${LINUX_FW_REF}" f sum
      local -A want=(
        [qcom/a740_sqe.fw]=96fee336424b139100fc60b5b45a907360e4b3936d7e1d00406b9bd80ca48473
        [qcom/gmu_gen70200.bin]=1a2a419c39046d3141fc5fed5aa7f971de2db40cc7a1d89693c3e26fad64dd98
        [qcom/sm8550/a740_zap.mbn]=386bbdc25ae94a9398e33e7580eecfdcef6c472682a1e3123e1374bcffe7cde3
      )
      for f in "${!want[@]}"; do
        fetch "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/${LINUX_FW_REF}/${f}" "${lfw}/${f}"
        sum="$(sha256sum "${lfw}/${f}" | cut -d" " -f1)"
        [[ "$sum" == "${want[$f]}" ]] || die "${f}: sha256 ${sum} != pinned ${want[$f]}"
        install -D -m0644 "${lfw}/${f}" "${ext}/${f}"
      done
      ;;
  esac
  cp -L "${regdb}/regulatory.db" "${regdb}/regulatory.db.p7s" "$ext/"
  (cd "$ext" && find . -type f | sed 's|^\./||' | sort | xargs) >"${WORK}/extra-firmware.list"
}

configure() {
  local cfg
  cfg="$(rocknix_path "projects/ROCKNIX/devices/${SOC_UC}/linux/linux.aarch64.conf")"
  [[ -f "$cfg" ]] || die "missing ROCKNIX config $cfg"
  cp "$cfg" "${SRC}/.config"
  local sc="${SRC}/scripts/config --file ${SRC}/.config"
  # ROCKNIX embeds its own initramfs through this placeholder; we boot without one.
  $sc --set-str INITRAMFS_SOURCE ""
  $sc --set-str LOCALVERSION "$LOCALVERSION"
  $sc --disable LOCALVERSION_AUTO
  $sc --set-str EXTRA_FIRMWARE "$(cat "${WORK}/extra-firmware.list")"
  $sc --set-str EXTRA_FIRMWARE_DIR "external-firmware"
  # Merge the SteamOS fragment (see steamos.config for why each is needed)
  # plus the per-SoC one (steamos-${SOC}.config).
  local frag
  frag="$(mktemp)"
  cat "${HERE}/steamos.config" >"$frag"
  [[ -f "${HERE}/steamos-${SOC}.config" ]] && cat "${HERE}/steamos-${SOC}.config" >>"$frag"
  local line opt val
  while IFS= read -r line; do
    [[ -z "$line" || "$line" == \#* ]] && continue
    opt="${line%%=*}"; val="${line#*=}"; opt="${opt#CONFIG_}"
    case "$val" in
      y) $sc --enable "$opt" ;;
      m) $sc --module "$opt" ;;
      n) $sc --disable "$opt" ;;
      \"*) $sc --set-str "$opt" "$(eval echo "$val")" ;;
      *) $sc --set-val "$opt" "$val" ;;
    esac
  done <"$frag"
  make -C "$SRC" olddefconfig >/dev/null
  # Report anything from the fragment that Kconfig refused.
  local bad=0
  while IFS= read -r line; do
    [[ -z "$line" || "$line" == \#* ]] && continue
    opt="${line%%=*}"; val="${line#*=}"
    if [[ "$val" == n ]]; then
      grep -q "^${opt}=" "${SRC}/.config" && { log "WARN ${opt} still set"; bad=1; }
    elif ! grep -qx "${opt}=${val}" "${SRC}/.config"; then
      log "WARN ${opt}=${val} not applied (got: $(grep -E "^(# )?${opt}[= ]" "${SRC}/.config" || echo unset))"; bad=1
    fi
  done <"$frag"
  rm -f "$frag"
  ((bad)) && log "some fragment options did not stick (see WARN lines)"
  return 0
}

build_kernel() {
  log "make -j${JOBS} Image modules dtbs"
  make -C "$SRC" -j"$JOBS" Image modules dtbs
  KREL="$(make -s -C "$SRC" kernelrelease)"
  log "kernel release ${KREL}"
}

build_tddi() {
  # ChipOne TDDI touch is the Pocket FIT's; the RP6 uses in-tree edt-ft5x06.
  [[ "$SOC" == sm8650 ]] || return 0
  local tar="${CACHE}/chipone_tddi-${TDDI_REF}.tar.gz" d="${WORK}/chipone_tddi"
  fetch "https://github.com/ROCKNIX/chipone_tddi/archive/${TDDI_REF}.tar.gz" "$tar"
  rm -rf "$d"; mkdir -p "$d"
  tar -C "$d" --strip-components=1 -xzf "$tar"
  make -C "$SRC" M="$d" -j"$JOBS" modules
}

build_initramfs() {
  # Tiny busybox initramfs (initramfs/init): mounts the SD root and writes
  # bootlog.txt to the FAT partition. The configuration verified to boot on
  # the Pocket FIT; also the only log available when the screen stays black.
  local bb=/bin/busybox d="${WORK}/initramfs"
  file "$bb" 2>/dev/null | grep -q "statically linked" \
    || die "need a static busybox (apt install busybox-static)"
  rm -rf "$d"; mkdir -p "$d/root/bin" "$d/root/dev" "$d/root/proc" "$d/root/sys"
  cp "$bb" "$d/root/bin/busybox"
  install -m0755 "${HERE}/initramfs/init" "$d/root/init"
  install -m0755 "${HERE}/initramfs/konkr-update-recover" "$d/root/konkr-update-recover"
  (cd "$d/root" && find . | cpio -o -H newc --owner=0:0 2>/dev/null | gzip -9) >"$d/initrd.gz"
  INITRD="$d/initrd.gz"
}

pack_kernel_img() {
  local out="$1" img="${SRC}/arch/arm64/boot/Image" payload dtb f
  payload="$(mktemp)"
  gzip -9 -n -c "$img" >"$payload"
  for dtb in $DTBS; do
    f="${SRC}/arch/arm64/boot/dts/qcom/${dtb}.dtb"
    [[ -f "$f" ]] || die "missing ${f}"
    cat "$f" >>"$payload"
  done
  # Placeholder root; make-steamos-sm8650.sh patches the real PARTUUID in.
  local cmdline
  cmdline="$(SOC="$SOC" bash -c "source '${HERE}/cmdline.sh'; build_cmdline 00000000-02")"
  python3 "${HERE}/mkbootimg-v0.py" --kernel "$payload" --ramdisk "$INITRD" \
    --cmdline "$cmdline" --out "$out"
  rm -f "$payload"
  md5sum "$out" | awk '{print $1"  KERNEL"}' >"$(dirname "$out")/KERNEL.md5"
}

install_output() {
  local o="${OUT_BASE}/${KREL}"
  rm -rf "$o"
  mkdir -p "$o/boot" "$o/modules" "$o/firmware" "$o/dtbs"
  log "modules_install"
  make -C "$SRC" INSTALL_MOD_PATH="$o/staging" INSTALL_MOD_STRIP=1 modules_install >/dev/null
  if [[ "$SOC" == sm8650 ]]; then
    make -C "$SRC" M="${WORK}/chipone_tddi" INSTALL_MOD_PATH="$o/staging" INSTALL_MOD_STRIP=1 \
      INSTALL_MOD_DIR=extra modules_install >/dev/null
  fi
  depmod -b "$o/staging" "$KREL"
  mv "$o/staging/lib/modules/${KREL}" "$o/modules/${KREL}"
  rm -rf "$o/staging"
  rm -f "$o/modules/${KREL}/build" "$o/modules/${KREL}/source"

  log "firmware (rootfs part: remoteprocs, audio topology, Wi-Fi/BT)"
  cp -a "${EXTRA_FW_SRC}/${SOC_UC}/." "$o/firmware/"
  # Built-in copies are enough for the GPU; keep rootfs copies too for tooling.
  cp -a "${SRC}/external-firmware/." "$o/firmware/"

  local dtb
  for dtb in $DTBS; do cp "${SRC}/arch/arm64/boot/dts/qcom/${dtb}.dtb" "$o/dtbs/"; done
  cp "${SRC}/.config" "$o/config-${KREL}"
  cp "${SRC}/System.map" "$o/System.map-${KREL}"
  pack_kernel_img "$o/boot/KERNEL"
  ln -sfn "$KREL" "${OUT_BASE}/current"
  log "done: $o"
  ls -la "$o/boot" >&2
}

main() {
  if [[ "${1:-}" == --repack-boot ]]; then
    [[ -s "$SRC/arch/arm64/boot/Image" ]] || die "no previously built kernel Image"
    KREL="$(make -s -C "$SRC" kernelrelease)"
    [[ -d "$OUT_BASE/$KREL" ]] || die "no previously built kernel output"
    build_initramfs
    pack_kernel_img "$OUT_BASE/$KREL/boot/KERNEL"
    log "repacked initramfs: $OUT_BASE/$KREL/boot/KERNEL"
    return
  fi
  check_deps
  [[ -d "$ROCKNIX_DIR/projects/ROCKNIX/devices/${SOC_UC}" ]] \
    || die "ROCKNIX tree not found at ${ROCKNIX_DIR} (sparse clone of ROCKNIX/distribution@${ROCKNIX_REF})"
  mkdir -p "$CACHE"
  prepare_source
  stage_builtin_firmware
  configure
  build_kernel
  build_tddi
  build_initramfs
  install_output
}

main "$@"
