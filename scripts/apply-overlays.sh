#!/usr/bin/env bash
# Apply the handheld overlay onto the extracted SteamOS Frame rootfs.
# SM8650 port (KONKR Pocket FIT / AYANEO Pocket S2): the Frame is SM8650 /
# Adreno 750 itself, so Valve's Turnip + GPU firmware are kept as shipped.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORKDIR="${STEAMOS_WORK:-/work}"
R="${STEAMOS_ROOTFS:-${WORKDIR}/rootfs}"
MOD="${ROOT}/external-and-mods"
OVL="${ROOT}/steamos-overlay"
KOUT="$(readlink -f "${KERNEL_OUT:-${WORKDIR}/kernel-release/current}")"
KREL="$(basename "$KOUT")"
# Target SoC follows the kernel being installed (build.sh LOCALVERSION
# -<soc>-steamos) unless SOC is set. Device-specific extras key off it.
if [[ -z "${SOC:-}" ]]; then
  case "$KREL" in
    *-sm8550-*) SOC=sm8550 ;;
    *) SOC=sm8650 ;;
  esac
fi
export SOC
# Device-only extras on top of the SoC's (make-steamos-sm8650.sh --device).
# Empty = none: the image suits every device of the SoC. thor = the AYN
# Thor's bottom screen (Barry Launcher, Firefox, Barry Launcher plugin), which
# also needs a gamescope built with the DRM lease patches.
DEVICE="${DEVICE:-}"
case "$DEVICE" in
  "") ;;
  thor) [[ "$SOC" == sm8550 ]] || { echo "ERROR: DEVICE=thor needs SOC=sm8550 (have ${SOC})" >&2; exit 1; } ;;
  *) echo "ERROR: unknown DEVICE ${DEVICE}" >&2; exit 1 ;;
esac
export DEVICE
SM8650_OVL="${ROOT}/sm8650-overlay"
STOCK="${R}/opt/stock-steamos"
GSBUILD="${GAMESCOPE_BUILD:-${WORKDIR}/gamescope-build}"
# The source scripts/build-gamescope-in-rootfs.sh built from (PB-OS's
# gamescope fork): its scripts, looks and udev rule go in the image.
GSSRC="${GAMESCOPE_SRC:-${WORKDIR}/gamescope-src}"
# Optional Turnip override. Empty = keep the Frame's own (built for A750).
MESA_SO="${MESA_SO:-}"
LOG="${WORKDIR}/odin-apply.log"

die() { echo "ERROR: $*" >&2; exit 1; }
log() { echo "$*" | tee -a "$LOG"; }

[[ -d "$R/usr/bin" ]] || die "missing rootfs at $R"
[[ -f "$KOUT/boot/KERNEL" ]] || die "missing kernel $KOUT"
[[ -x "$GSBUILD/src/gamescope" ]] || die "missing built gamescope"
[[ -d "$GSSRC/scripts" ]] || die "missing gamescope source at $GSSRC (run scripts/build-gamescope-in-rootfs.sh)"
if [[ "$DEVICE" == thor ]] && ! grep -qa -- '--lease-connector' "$GSBUILD/src/gamescope"; then
  die "DEVICE=thor needs a gamescope with the DRM lease patches (GAMESCOPE_BUILD=$GSBUILD has none)"
fi
[[ -z "$MESA_SO" || -f "$MESA_SO" ]] || die "missing Mesa $MESA_SO"
[[ -d "$KOUT/modules/$KREL" ]] || die "missing modules $KOUT/modules/$KREL"

: >"$LOG"
log "== $(date -Iseconds) apply Odin mods into $R (SOC=${SOC} DEVICE=${DEVICE:-none})"

backup() {
  local src="$1" dest="$2"
  [[ -e "$src" ]] || return 0
  mkdir -p "$(dirname "$dest")"
  if [[ ! -e "$dest" ]]; then
    cp -a "$src" "$dest"
  fi
}

install_file() {
  local src="$1" dest="$2" mode="${3:-}"
  mkdir -p "$(dirname "$dest")"
  cp -a "$src" "$dest"
  [[ -n "$mode" ]] && chmod "$mode" "$dest"
}

# ---------------------------------------------------------------------------
# Kernel
# ---------------------------------------------------------------------------
log "== kernel ${KREL}"
mkdir -p "$R/boot" "$R/usr/lib/modules" "$R/usr/lib/firmware" "$R/opt/steamos-sm8650"
if [[ -e "$R/boot/KERNEL" && ! -e "$STOCK/boot/KERNEL" ]]; then
  mkdir -p "$STOCK/boot"
  cp -a "$R/boot/KERNEL" "$STOCK/boot/KERNEL" 2>/dev/null || true
fi
cp -a "$KOUT/boot/KERNEL" "$R/boot/KERNEL"
cp -a "$KOUT/boot/KERNEL.md5" "$R/boot/KERNEL.md5"
chmod 0644 "$R/boot/KERNEL" "$R/boot/KERNEL.md5"

# Frame kernel modules are useless with this kernel; keep only ours.
find "$R/usr/lib/modules" -mindepth 1 -maxdepth 1 ! -name "$KREL" -exec rm -rf {} +
# Replace, not merge: with the same $KREL already present (a reused rootfs),
# cp -a nests the new tree at $KREL/$KREL and the old modules stay in use.
rm -rf "$R/usr/lib/modules/$KREL"
cp -a "$KOUT/modules/$KREL" "$R/usr/lib/modules/$KREL"
# Merge firmware without wiping Frame blobs (Frame ships SM8650 GPU fw too;
# the AYANEO-signed ADSP/CDSP/zap live under qcom/sm8650/ayaneo/ps2).
cp -a "$KOUT/firmware/." "$R/usr/lib/firmware/"
# Frame supplies the exact upstream VPU33 firmware under its vendor name.
# Iris requests the upstream alias. Verify before creating that alias.
_vpu="$R/usr/lib/firmware/qcom/vpu/vpu33_4v.mbn"
if [[ -f "$_vpu" ]]; then
  [[ $(sha256sum "$_vpu" | awk '{print $1}') == 7b829fc1c8ce7cca836d10e898b99c5bcbd86e22073b690147168c9d0a5de378 ]] \
    || die "unexpected SM8650 decoder firmware; reverify against upstream"
  ln -sfn vpu33_4v.mbn "$R/usr/lib/firmware/qcom/vpu/vpu33_p4.mbn"
else
  die "missing SM8650 video decoder firmware"
fi
cp -a "$KOUT/config-$KREL" "$KOUT/dtbs" "$R/opt/steamos-sm8650/" 2>/dev/null || true

# ---------------------------------------------------------------------------
# gamescope
# ---------------------------------------------------------------------------
log "== gamescope (MSM + backlight)"
for b in gamescope gamescopectl gamescopereaper gamescopestream; do
  backup "$R/usr/bin/$b" "$STOCK/usr/bin/$b"
  install_file "$GSBUILD/src/$b" "$R/usr/bin/$b" 0755
  mkdir -p "$R/usr/local/bin"
  install_file "$GSBUILD/src/$b" "$R/usr/local/bin/$b" 0755
done
if [[ -f "$GSBUILD/layer/libVkLayer_FROG_gamescope_wsi_aarch64.so" ]]; then
  backup "$R/usr/lib/libVkLayer_FROG_gamescope_wsi_aarch64.so" \
    "$STOCK/usr/lib/libVkLayer_FROG_gamescope_wsi_aarch64.so"
  install_file "$GSBUILD/layer/libVkLayer_FROG_gamescope_wsi_aarch64.so" \
    "$R/usr/lib/libVkLayer_FROG_gamescope_wsi_aarch64.so" 0755
  mkdir -p "$R/usr/local/lib"
  install_file "$GSBUILD/layer/libVkLayer_FROG_gamescope_wsi_aarch64.so" \
    "$R/usr/local/lib/libVkLayer_FROG_gamescope_wsi_aarch64.so" 0755
fi
if [[ -d "${GSSRC}/scripts" ]]; then
  mkdir -p "$R/usr/share/gamescope" "$R/usr/local/share/gamescope"
  rm -rf "$R/usr/share/gamescope/scripts" "$R/usr/local/share/gamescope/scripts"
  cp -a "${GSSRC}/scripts" "$R/usr/share/gamescope/scripts"
  cp -a "${GSSRC}/scripts" "$R/usr/local/share/gamescope/scripts"
  if [[ -d "${GSSRC}/looks" ]]; then
    rm -rf "$R/usr/share/gamescope/looks" "$R/usr/local/share/gamescope/looks"
    cp -a "${GSSRC}/looks" "$R/usr/share/gamescope/looks"
    cp -a "${GSSRC}/looks" "$R/usr/local/share/gamescope/looks"
  fi
fi
install_file "${GSSRC}/scripts/udev/60-gamescope-backlight.rules" \
  "$R/usr/lib/udev/rules.d/60-gamescope-backlight.rules" 0644
# Also land in /lib if SteamOS uses it
mkdir -p "$R/lib/udev/rules.d"
install_file "${GSSRC}/scripts/udev/60-gamescope-backlight.rules" \
  "$R/lib/udev/rules.d/60-gamescope-backlight.rules" 0644

backup "$R/usr/lib/steamos/gamescope-session" "$STOCK/usr/lib/steamos/gamescope-session"
install_file "$OVL/usr/lib/steamos/gamescope-session" \
  "$R/usr/lib/steamos/gamescope-session" 0755
backup "$R/usr/lib/steamos/gamescope-onready" "$STOCK/usr/lib/steamos/gamescope-onready"
install_file "$OVL/usr/lib/steamos/gamescope-onready" \
  "$R/usr/lib/steamos/gamescope-onready" 0755
install_file "$OVL/usr/lib/steamos/sm8550-steam-focus" \
  "$R/usr/lib/steamos/sm8550-steam-focus" 0755
install_file "$OVL/usr/lib/steamos/odin-bin/steamvr" \
  "$R/usr/lib/steamos/odin-bin/steamvr" 0755
backup "$R/usr/bin/steamos-select-branch" "$STOCK/usr/bin/steamos-select-branch"
install_file "$OVL/usr/bin/steamos-select-branch" \
  "$R/usr/bin/steamos-select-branch" 0755
# Official Plasma is already in the image. Switch-to-desktop must clear
# Game Mode QT_QPA_PLATFORM=xcb or plasmashell dies and the screen stays black.
install_file "$OVL/usr/lib/steamos/sm8550-prepare-plasma" \
  "$R/usr/lib/steamos/sm8550-prepare-plasma" 0755
install_file "$OVL/usr/lib/steamos/sm8550-startplasma" \
  "$R/usr/lib/steamos/sm8550-startplasma" 0755
install_file "$OVL/usr/share/steamos-sm8550/kwinoutputconfig-thor.json" \
  "$R/usr/share/steamos-sm8550/kwinoutputconfig-thor.json" 0644
backup "$R/usr/bin/steamos-session-select" "$STOCK/usr/bin/steamos-session-select"
install_file "$OVL/usr/bin/steamos-session-select" \
  "$R/usr/bin/steamos-session-select" 0755
backup "$R/usr/share/wayland-sessions/plasma.desktop" \
  "$STOCK/usr/share/wayland-sessions/plasma.desktop"
install_file "$OVL/usr/share/wayland-sessions/plasma.desktop" \
  "$R/usr/share/wayland-sessions/plasma.desktop" 0644
install_file "$OVL/usr/lib/systemd/user/sm8550-plasma-env.service" \
  "$R/usr/lib/systemd/user/sm8550-plasma-env.service" 0644
for tgt in plasma-core.target plasma-workspace.target plasma-workspace-wayland.target; do
  mkdir -p "$R/usr/lib/systemd/user/${tgt}.d"
  install_file "$OVL/usr/lib/systemd/user/${tgt}.d/99-odin.conf" \
    "$R/usr/lib/systemd/user/${tgt}.d/99-odin.conf" 0644
done
WAYLAND_DROPIN="$OVL/usr/lib/systemd/user/plasma-plasmashell.service.d/99-odin-wayland.conf"
for svc in plasma-plasmashell plasma-ksplash plasma-ksmserver \
  plasma-kcminit plasma-kcminit-phase1 plasma-kded6 plasma-kwin_wayland \
  plasma-gmenudbusmenuproxy plasma-xembedsniproxy plasma-kaccess \
  plasma-powerdevil plasma-polkit-agent plasma-kglobalaccel plasma-kscreen \
  plasma-xdg-desktop-portal-kde plasma-krunner plasma-kactivitymanagerd \
  plasma-dolphin plasma-ksystemstats plasma-restoresession plasma-baloorunner
do
  mkdir -p "$R/usr/lib/systemd/user/${svc}.service.d"
  install_file "$WAYLAND_DROPIN" \
    "$R/usr/lib/systemd/user/${svc}.service.d/99-odin-wayland.conf" 0644
done
rm -f "$R/usr/lib/steamos/sm8550-desktop-session"
install_file "$OVL/usr/bin/jupiter-initial-firmware-update" \
  "$R/usr/bin/jupiter-initial-firmware-update" 0755
install_file "$OVL/usr/bin/steamos-mandatory-update" \
  "$R/usr/bin/steamos-mandatory-update" 0755
# Steam Software Updates toast: official steamos-update → pkexec/atomupd → 127.
backup "$R/usr/bin/steamos-update" "$STOCK/usr/bin/steamos-update"
install_file "$OVL/usr/bin/steamos-update" \
  "$R/usr/bin/steamos-update" 0755
install_file "$OVL/usr/bin/steamos-polkit-helpers/steamos-update" \
  "$R/usr/bin/steamos-polkit-helpers/steamos-update" 0755
install_file "$OVL/usr/lib/steamos/sm8550-oobe-restart-steam" \
  "$R/usr/lib/steamos/sm8550-oobe-restart-steam" 0755
install_file "$OVL/usr/lib/systemd/system/sm8550-oobe-restart-steam.service" \
  "$R/usr/lib/systemd/system/sm8550-oobe-restart-steam.service" 0644
mkdir -p "$R/usr/lib/systemd/user/steam.service.d"
install_file "$OVL/usr/lib/systemd/user/steam.service.d/99-sm8550-bootstrap.conf" \
  "$R/usr/lib/systemd/user/steam.service.d/99-sm8550-bootstrap.conf" 0644
# Session vars (refresh slider, mangoapp) via a file: onready's import races Steam.
install_file "$OVL/usr/lib/systemd/user/steam.service.d/60-gamescope-env.conf" \
  "$R/usr/lib/systemd/user/steam.service.d/60-gamescope-env.conf" 0644
backup "$R/usr/bin/start-gamescope-session" "$STOCK/usr/bin/start-gamescope-session"
install_file "$OVL/usr/bin/start-gamescope-session" \
  "$R/usr/bin/start-gamescope-session" 0755
backup "$R/usr/share/deckard/RUNSTEAM.sh" "$STOCK/usr/share/deckard/RUNSTEAM.sh"
install_file "$OVL/usr/share/deckard/RUNSTEAM.sh" \
  "$R/usr/share/deckard/RUNSTEAM.sh" 0755
install_file "$OVL/usr/share/deckard/steam-health-check" \
  "$R/usr/share/deckard/steam-health-check" 0755
# Odin 2 has no dock. Missing /usr/bin/jupiter-dock-updater is exit 127
# and Steam shows "Error de actualización". --check must exit 7 (up to date).
log "== dock stub"
mkdir -p "$R/usr/bin/steamos-polkit-helpers"
install_file "$OVL/usr/bin/jupiter-dock-updater" \
  "$R/usr/bin/jupiter-dock-updater" 0755
install_file "$OVL/usr/bin/steamos-polkit-helpers/jupiter-dock-updater" \
  "$R/usr/bin/steamos-polkit-helpers/jupiter-dock-updater" 0755
# steam.service copies this into the user home on each start.
if [[ -d "$R/home/steamos/.local/share/Steam" ]]; then
  install_file "$OVL/usr/share/deckard/RUNSTEAM.sh" \
    "$R/home/steamos/.local/share/Steam/RUNSTEAM.sh" 0755
fi

mkdir -p "$R/usr/lib/systemd/user/gamescope-session.service.d"
mkdir -p "$R/usr/lib/systemd/user/gamescope-session.target.d"
mkdir -p "$R/usr/lib/systemd/user/steam.service.d"
install_file "$OVL/usr/lib/systemd/user/gamescope-session.service.d/99-odin.conf" \
  "$R/usr/lib/systemd/user/gamescope-session.service.d/99-odin.conf" 0644
install_file "$OVL/usr/lib/systemd/user/gamescope-session.target.d/99-odin.conf" \
  "$R/usr/lib/systemd/user/gamescope-session.target.d/99-odin.conf" 0644
install_file "$OVL/usr/lib/systemd/user/steam.service.d/99-odin.conf" \
  "$R/usr/lib/systemd/user/steam.service.d/99-odin.conf" 0644
# Frame leftover: SteamVR must not start on a handheld (Wants= is additive).
mkdir -p "$R/etc/systemd/user" "$R/etc/systemd/system"
for u in steamvr.service steamvr-logs.service steamvr-proxmicmute.service \
         steamvr-v4l2cam.service steamvr-nested-desktop.service; do
  ln -sfn /dev/null "$R/etc/systemd/user/${u}"
done
for u in steamvr-program-ble.service steamvr-v4l2loopback.service \
         steamvr-set-kernel-thread-priorities.service \
         deckard-audio-setup.service \
         deckard-fan-control.service deckard-fpga.service \
         deckard-led-control.service deckard-typec-logger.service \
         set-wifi-mac-address.service iwd.service deckard-charger.service \
         deckard-power-monitor.service deckard-fpga-resume.service \
         deckard-boot-images.service \
         adbd.service adbd-post.service usb-gadget.service usb-gadget.target \
         usb-ncm-gadget@.service usb-ncm-dnsmasq@.service \
         steamos-boot.service efi.mount esp.mount systemd-repart.service \
         firewalld.service atomupd.service; do
  # Frame USB-gadget/ADB/power-monitor: no such hardware here. They crash-loop
  # (1000+ restarts/night) and adbd-post polls ffs.adb/ready at 10 Hz forever,
  # which keeps the SoC out of deep idle and burned battery in standby.
  # steamos-boot (A/B slot bookkeeping) needs efi.mount, which waits for the
  # Deck's EFI partition (by-partsets/self/efi). There's none here, so every
  # boot sat on it for the full 90 s device timeout.
  # systemd-repart (Valve's repart.d/90-home.conf) adds a home partition to
  # the root disk's free space at boot. Our layouts already have /home and
  # steamos-sm8550-expand-home grows it; on internal UFS repart must never
  # touch the partition table.
  # firewalld: the Frame's zone only blocks ports below 1024 (it allows ssh and
  # 1024-65535), and NetworkManager waits for it, so the login screen waited
  # ~18 s on microSD for almost no protection. SSH is off by default here.
  # atomupd: Steam Deck A/B update daemon with no RAUC slots to act on (our
  # steamos-update hooks bypass it); it spent up to ~20 s at every boot
  # competing with Steam's startup.
  ln -sfn /dev/null "$R/etc/systemd/system/${u}"
done

# ---------------------------------------------------------------------------
# Audio UCM + Wi-Fi (wpa, not iwd) + BT power + gamescope Wayland session
# ---------------------------------------------------------------------------
log "== alsa UCM AYN-Odin2 + wifi/wpa + bluetooth + wayland session"
if [[ -d "$OVL/usr/share/alsa/ucm2" ]]; then
  mkdir -p "$R/usr/share/alsa/ucm2"
  cp -r --no-preserve=mode,ownership "$OVL/usr/share/alsa/ucm2/." "$R/usr/share/alsa/ucm2/"
  # root alsaucm cannot read 600 steam:steam UCM (speakers stay silent).
  chown -R root:root "$R/usr/share/alsa/ucm2/AYN" \
    "$R/usr/share/alsa/ucm2/codecs" "$R/usr/share/alsa/ucm2/lib" \
    "$R/usr/share/alsa/ucm2/conf.d/sm8550" 2>/dev/null || true
  find "$R/usr/share/alsa/ucm2/AYN" "$R/usr/share/alsa/ucm2/codecs" \
    "$R/usr/share/alsa/ucm2/lib" "$R/usr/share/alsa/ucm2/conf.d/sm8550" \
    -type d -exec chmod 0755 {} + 2>/dev/null || true
  find "$R/usr/share/alsa/ucm2/AYN" "$R/usr/share/alsa/ucm2/codecs" \
    "$R/usr/share/alsa/ucm2/lib" "$R/usr/share/alsa/ucm2/conf.d/sm8550" \
    -type f -exec chmod 0644 {} + 2>/dev/null || true
fi
# Frame steamclient reads VARIANT_ID=vr and Gamepad UI then throws.
for _osr in "$R/etc/os-release" "$R/usr/lib/os-release" \
  "$R/var/lib/overlays/etc/upper/os-release"; do
  [[ -f "$_osr" ]] || continue
  sed -i 's/^VARIANT_ID=.*/VARIANT_ID="steamdeck"/' "$_osr" || true
  grep -q '^VARIANT_ID=' "$_osr" || echo 'VARIANT_ID="steamdeck"' >> "$_osr"
done
unset _osr
# Dangling Frame VR audio plugins break Chromium/Steam streams.
for _so in \
  "$R/usr/lib/ladspa/vraudiocompositor.so" \
  "$R/usr/lib/ladspa/audiofilter.so" \
  "$R/usr/lib/ladspa/libphonon.so"
do
  if [[ -L "$_so" && ! -e "$_so" ]]; then
    rm -f "$_so"
  fi
done
unset _so
install_file "$OVL/usr/lib/NetworkManager/conf.d/40-sm8550-wifi.conf" \
  "$R/usr/lib/NetworkManager/conf.d/40-sm8550-wifi.conf" 0644
install_file "$OVL/usr/lib/modprobe.d/ath12k.conf" \
  "$R/usr/lib/modprobe.d/ath12k.conf" 0644
install_file "$OVL/usr/lib/systemd/network/99-sm8550-wlan0.link" \
  "$R/usr/lib/systemd/network/99-sm8550-wlan0.link" 0644
install_file "$OVL/usr/lib/steamos/sm8550-wifi-backend" \
  "$R/usr/lib/steamos/sm8550-wifi-backend" 0755
install_file "$OVL/usr/lib/systemd/system/sm8550-wifi-backend.service" \
  "$R/usr/lib/systemd/system/sm8550-wifi-backend.service" 0644
install_file "$OVL/usr/lib/systemd/system/sm8550-wifi-backend.path" \
  "$R/usr/lib/systemd/system/sm8550-wifi-backend.path" 0644
mkdir -p "$R/usr/lib/systemd/system/NetworkManager.service.d"
install_file "$OVL/usr/lib/systemd/system/NetworkManager.service.d/99-sm8550-wpa.conf" \
  "$R/usr/lib/systemd/system/NetworkManager.service.d/99-sm8550-wpa.conf" 0644
install_file "$OVL/usr/lib/steamos/sm8550-audio-setup" \
  "$R/usr/lib/steamos/sm8550-audio-setup" 0755
install_file "$OVL/usr/lib/steamos/sm8550-audio-pipewire" \
  "$R/usr/lib/steamos/sm8550-audio-pipewire" 0755
install_file "$OVL/usr/lib/steamos/sm8550-volume-keys" \
  "$R/usr/lib/steamos/sm8550-volume-keys" 0755
install_file "$OVL/usr/lib/systemd/system/sm8550-audio-setup.service" \
  "$R/usr/lib/systemd/system/sm8550-audio-setup.service" 0644
install_file "$OVL/usr/lib/systemd/user/sm8550-audio-pipewire.service" \
  "$R/usr/lib/systemd/user/sm8550-audio-pipewire.service" 0644
install_file "$OVL/usr/lib/systemd/user/sm8550-volume-keys.service" \
  "$R/usr/lib/systemd/user/sm8550-volume-keys.service" 0644
install_file "$OVL/usr/share/wireplumber/wireplumber.conf.d/51-sm8550-hifi-priority.conf" \
  "$R/usr/share/wireplumber/wireplumber.conf.d/51-sm8550-hifi-priority.conf" 0644
install_file "$OVL/usr/share/wireplumber/wireplumber.conf.d/52-sm8550-alsa.conf" \
  "$R/usr/share/wireplumber/wireplumber.conf.d/52-sm8550-alsa.conf" 0644
install_file "$OVL/etc/wireplumber/wireplumber.conf.d/52-sm8550-alsa.conf" \
  "$R/etc/wireplumber/wireplumber.conf.d/52-sm8550-alsa.conf" 0644
install_file "$OVL/usr/share/pipewire/pipewire.conf.d/99-sm8550-buffers.conf" \
  "$R/usr/share/pipewire/pipewire.conf.d/99-sm8550-buffers.conf" 0644
install_file "$OVL/usr/share/pipewire/pipewire-pulse.conf.d/99-sm8550-buffers.conf" \
  "$R/usr/share/pipewire/pipewire-pulse.conf.d/99-sm8550-buffers.conf" 0644
install_file "$OVL/usr/lib/udev/rules.d/90-sm8550-audio.rules" \
  "$R/usr/lib/udev/rules.d/90-sm8550-audio.rules" 0644
install_file "$OVL/etc/wireplumber/wireplumber.conf.d/99-sm8550-no-vr-spatial.conf" \
  "$R/etc/wireplumber/wireplumber.conf.d/99-sm8550-no-vr-spatial.conf" 0644
if [[ -f "$R/etc/wireplumber/wireplumber.conf.d/50-alsa-config.conf" ]]; then
  sed -i 's/api.acp.disable-pro-audio = true/api.acp.disable-pro-audio = false/' \
    "$R/etc/wireplumber/wireplumber.conf.d/50-alsa-config.conf" || true
  sed -i 's/node.force-quantum    = 480/node.force-quantum    = 512/' \
    "$R/etc/wireplumber/wireplumber.conf.d/50-alsa-config.conf" || true
  sed -i 's/api.alsa.period-size  = 256/api.alsa.period-size  = 1024/' \
    "$R/etc/wireplumber/wireplumber.conf.d/50-alsa-config.conf" || true
fi
# Frame spatializer is required= and its .so is a /run dangling symlink.
for _sp in 60-spatial-audio.conf 70-spatial-node-config.conf; do
  if [[ -f "$R/etc/wireplumber/wireplumber.conf.d/${_sp}" ]]; then
    mv -f "$R/etc/wireplumber/wireplumber.conf.d/${_sp}" \
      "$R/etc/wireplumber/wireplumber.conf.d/${_sp}.disabled" || true
  fi
done
unset _sp
install_file "$OVL/usr/lib/steamos/sm8550-patch-steamui" \
  "$R/usr/lib/steamos/sm8550-patch-steamui" 0755
install_file "$OVL/usr/lib/steamos/sm8550-bluetooth-setup" \
  "$R/usr/lib/steamos/sm8550-bluetooth-setup" 0755
install_file "$OVL/usr/lib/systemd/system/sm8550-bluetooth-setup.service" \
  "$R/usr/lib/systemd/system/sm8550-bluetooth-setup.service" 0644
# Steam writes this fragment to force iwd; pin wpa in /etc and the overlay upper.
for dest in \
  "$R/etc/NetworkManager/conf.d/99-valve-wifi-backend.conf" \
  "$R/var/lib/overlays/etc/upper/NetworkManager/conf.d/99-valve-wifi-backend.conf"
do
  install_file "$OVL/etc/NetworkManager/conf.d/99-valve-wifi-backend.conf" "$dest" 0644
done
mkdir -p "$R/etc/systemd/system/multi-user.target.wants" \
  "$R/etc/systemd/system/NetworkManager.service.wants" \
  "$R/etc/systemd/system/bluetooth.target.wants" \
  "$R/etc/systemd/system/sound.target.wants" \
  "$R/etc/systemd/user/default.target.wants"
ln -sfn /usr/lib/systemd/system/sm8550-wifi-backend.service \
  "$R/etc/systemd/system/multi-user.target.wants/sm8550-wifi-backend.service"
ln -sfn /usr/lib/systemd/system/sm8550-wifi-backend.service \
  "$R/etc/systemd/system/NetworkManager.service.wants/sm8550-wifi-backend.service"
ln -sfn /usr/lib/systemd/system/sm8550-wifi-backend.path \
  "$R/etc/systemd/system/multi-user.target.wants/sm8550-wifi-backend.path"
# Audio setup runs when the card appears (udev + sound.target), never from
# multi-user.target, which it used to hold back. Drop links older builds made.
rm -f "$R/etc/systemd/system/multi-user.target.wants/sm8550-audio-setup.service" \
  "$R/usr/lib/systemd/system/multi-user.target.wants/sm8550-audio-setup.service" \
  "$R/var/lib/overlays/etc/upper/systemd/system/multi-user.target.wants/sm8550-audio-setup.service"
mkdir -p "$R/usr/lib/systemd/system/sound.target.wants"
ln -sfn ../sm8550-audio-setup.service \
  "$R/usr/lib/systemd/system/sound.target.wants/sm8550-audio-setup.service"
ln -sfn /usr/lib/systemd/system/sm8550-audio-setup.service \
  "$R/etc/systemd/system/sound.target.wants/sm8550-audio-setup.service"
mkdir -p "$R/usr/lib/systemd/system/multi-user.target.wants"
mkdir -p "$R/usr/lib/systemd/user/default.target.wants"
ln -sfn /usr/lib/systemd/user/sm8550-audio-pipewire.service \
  "$R/usr/lib/systemd/user/default.target.wants/sm8550-audio-pipewire.service"
ln -sfn /usr/lib/systemd/user/sm8550-volume-keys.service \
  "$R/usr/lib/systemd/user/default.target.wants/sm8550-volume-keys.service"
ln -sfn /usr/lib/systemd/user/sm8550-volume-keys.service \
  "$R/etc/systemd/user/default.target.wants/sm8550-volume-keys.service"
ln -sfn /usr/lib/systemd/system/sm8550-bluetooth-setup.service \
  "$R/etc/systemd/system/multi-user.target.wants/sm8550-bluetooth-setup.service"
ln -sfn /usr/lib/systemd/system/sm8550-bluetooth-setup.service \
  "$R/etc/systemd/system/bluetooth.target.wants/sm8550-bluetooth-setup.service"
install_file "$OVL/etc/systemd/journald.conf.d/99-sm8550-persist.conf" \
  "$R/etc/systemd/journald.conf.d/99-sm8550-persist.conf" 0644
# Leave the initramfs/fsck console text (modprobe + "root: clean").
# Do not unbind fbcon: on MSM the last console frame looks hung.
rm -f "$R/usr/lib/steamos/sm8550-hide-console" \
  "$R/usr/lib/systemd/system/sm8550-hide-console.service" \
  "$R/lib/systemd/system/sm8550-hide-console.service" \
  "$R/etc/systemd/system/graphical.target.wants/sm8550-hide-console.service" \
  "$R/etc/systemd/system/sysinit.target.wants/sm8550-hide-console.service" \
  "$R/etc/systemd/system/multi-user.target.wants/sm8550-hide-console.service" \
  "$R/usr/lib/systemd/system/graphical.target.wants/sm8550-hide-console.service" \
  "$R/usr/lib/systemd/system/sysinit.target.wants/sm8550-hide-console.service" \
  "$R/usr/lib/systemd/system/multi-user.target.wants/sm8550-hide-console.service"
# Debug dumps must not paint the panel.
rm -f "$R/etc/systemd/system/multi-user.target.wants/sm8550-boot-debug.service" \
      "$R/etc/systemd/system/graphical.target.wants/sm8550-boot-debug-late.service"
ln -sfn /usr/lib/systemd/user/sm8550-audio-pipewire.service \
  "$R/etc/systemd/user/default.target.wants/sm8550-audio-pipewire.service"
# Host enables these explicitly; socket-only leaves gamescope without a sink.
mkdir -p "$R/etc/systemd/user/default.target.wants" \
  "$R/etc/xdg/systemd/user/default.target.wants"
for u in pipewire.service pipewire-pulse.service; do
  if [[ -f "$R/usr/lib/systemd/user/${u}" ]]; then
    ln -sfn "/usr/lib/systemd/user/${u}" \
      "$R/etc/systemd/user/default.target.wants/${u}"
    ln -sfn "/usr/lib/systemd/user/${u}" \
      "$R/etc/xdg/systemd/user/default.target.wants/${u}"
  fi
done
if [[ -f "$R/usr/lib/systemd/system/wpa_supplicant.service" ]]; then
  ln -sfn /usr/lib/systemd/system/wpa_supplicant.service \
    "$R/etc/systemd/system/multi-user.target.wants/wpa_supplicant.service"
  ln -sfn /usr/lib/systemd/system/wpa_supplicant.service \
    "$R/etc/systemd/system/NetworkManager.service.wants/wpa_supplicant.service"
fi
rm -f "$R/etc/systemd/system/multi-user.target.wants/iwd.service"
# Official Wayland Plasma, started via sm8550-startplasma (clears Game Mode xcb).
install_file "$OVL/usr/share/wayland-sessions/plasma.desktop" \
  "$R/usr/share/wayland-sessions/plasma.desktop" 0644
rm -f "$R/usr/lib/steamos/sm8550-desktop-session"
if [[ -d "$R/usr/share/steamos-manager/devices" ]]; then
  install_file "$OVL/usr/share/steamos-manager/devices/ayn-odin2.toml" \
    "$R/usr/share/steamos-manager/devices/ayn-odin2.toml" 0644
  install_file "$SM8650_OVL/usr/share/steamos-manager/devices/konkr-pocketfit.toml" \
    "$R/usr/share/steamos-manager/devices/konkr-pocketfit.toml" 0644
fi
# Steam "Switch to Desktop" listed only plasmax11. Hide Frame X11 sessions.
mkdir -p "$R/usr/share/steamos/hidden-xsessions"
for s in plasmax11.desktop openbox.desktop openbox-kde.desktop; do
  if [[ -f "$R/usr/share/xsessions/$s" ]]; then
    mv -f "$R/usr/share/xsessions/$s" "$R/usr/share/steamos/hidden-xsessions/$s"
  fi
done

# ---------------------------------------------------------------------------
# Native rsinput ±740, then InputPlumber deck-uhid + keyboard (OSK haptic).
# USB/Bluetooth HID is ignored in the composite so it is not grabbed.
# ---------------------------------------------------------------------------
log "== gamepad (rsinput ±740 + InputPlumber deck-uhid)"
install_file "$OVL/usr/lib/steamos/sm8550-fixpad" \
  "$R/usr/lib/steamos/sm8550-fixpad" 0755
install_file "$OVL/usr/lib/systemd/system/sm8550-fixpad.service" \
  "$R/usr/lib/systemd/system/sm8550-fixpad.service" 0644
mkdir -p "$R/etc/systemd/system/multi-user.target.wants"
ln -sfn /usr/lib/systemd/system/sm8550-fixpad.service \
  "$R/etc/systemd/system/multi-user.target.wants/sm8550-fixpad.service"
install_file "$OVL/usr/lib/udev/rules.d/70-sm8550-gamepad.rules" \
  "$R/usr/lib/udev/rules.d/70-sm8550-gamepad.rules" 0644
mkdir -p "$R/lib/udev/rules.d"
install_file "$OVL/usr/lib/udev/rules.d/70-sm8550-gamepad.rules" \
  "$R/lib/udev/rules.d/70-sm8550-gamepad.rules" 0644
install_file "$OVL/etc/sdl2/qcom-gamecontrollerdb.txt" \
  "$R/etc/sdl2/qcom-gamecontrollerdb.txt" 0644
install_file "$OVL/usr/lib/environment.d/60-sm8550-gamepad.conf" \
  "$R/usr/lib/environment.d/60-sm8550-gamepad.conf" 0644
install_file "$OVL/etc/profile.d/sm8550-gamepad.sh" \
  "$R/etc/profile.d/sm8550-gamepad.sh" 0644
"${SCRIPT_DIR}/install-inputplumber-sm8550.sh" "$R"

# ---------------------------------------------------------------------------
# SM8650 device overlay: Pocket FIT pad (XInput → deck-uhid), APS2 UCM,
# steamos-manager device, dual-SoC audio setup.
# ---------------------------------------------------------------------------
log "== SM8650 overlay (KONKR Pocket FIT / AYANEO Pocket S2)"
# Speaker limiter for wireplumber.conf.d/55-konkr-speaker.conf.
if [[ ! -f "$R/usr/lib/lv2/dpl.lv2/dpl.so" ]]; then
  "${SCRIPT_DIR}/build-dpl-lv2-in-rootfs.sh" "$R"
fi
cp -r --no-preserve=mode,ownership "$SM8650_OVL/." "$R/"
# konkrd/konkrctl were renamed pbosd/pbosctl. The build rootfs is reused, so
# drop the old daemon (two would fight over the fan); keep the old command
# name for scripts and habits.
rm -f "$R/usr/lib/konkr/konkrd" "$R/usr/lib/systemd/system/konkrd.service" \
  "$R/etc/konkrd.conf" "$R/etc/systemd/system/multi-user.target.wants/konkrd.service" \
  "$R/var/lib/overlays/etc/upper/konkrd.conf" \
  "$R/var/lib/overlays/etc/upper/systemd/system/multi-user.target.wants/konkrd.service"
ln -sfn pbosctl "$R/usr/bin/konkrctl"
if [[ "$SOC" != sm8650 ]]; then
  # SM8550 keeps the Odin 2 audio policy (sm8550-audio-pipewire: pro-audio +
  # MI2S speaker routes). 53 forces the SM8650-APS2 UCM profile and software
  # volume on the whole card; 55 is the Pocket FIT speaker tuning.
  rm -f "$R/etc/wireplumber/wireplumber.conf.d/53-konkr-audio.conf" \
    "$R/etc/wireplumber/wireplumber.conf.d/55-konkr-speaker.conf"
fi
chmod 0755 "$R/usr/lib/konkr/pocket-s2-controller"
chmod 0644 "$R/usr/lib/liblsfg-vk-layer-arm64.so" "$R/usr/lib/liblsfg-vk-layer-arm64.so.README"
# Audio: the Frame (also SM8650) hides the raw speaker node from every client
# so its VR speaker filter chain owns it; that chain is disabled here, which
# left the speakers unreachable. Drop the speaker from Valve's access rules.
ACCESS="$R/etc/wireplumber/wireplumber.conf.d/10-access.conf"
if [[ -f "$ACCESS" ]]; then
  cp -n "$ACCESS" "$R/etc/wireplumber/10-access.conf.frame-orig"
  sed -i '/node.name = "alsa_output.platform-sound.HiFi__Speaker__sink"/d' "$ACCESS"
fi
# Same as ayn_mcu: InputPlumber loads capability maps from /usr/share too.
mkdir -p "$R/usr/share/inputplumber/capability_maps"
cp -f "$SM8650_OVL"/etc/inputplumber/capability_maps.d/*.yaml "$R/usr/share/inputplumber/capability_maps/"
# InputPlumber ships its own Pocket FIT / Pocket S2 profiles (matched on the
# DT compatible). Ours cover the same pads with the Deck target, back buttons
# and the MCU keys; two matching profiles would fight over the same sources.
rm -f "$R/usr/share/inputplumber/devices/50-konkr_pocket_fit.yaml" \
  "$R/usr/share/inputplumber/devices/50-ayaneo_pocket_s2.yaml"
chown -R root:root "$R/usr/share/alsa/ucm2/Qualcomm/sm8650" "$R/usr/share/alsa/ucm2/conf.d/sm8650" \
  "$R/etc/inputplumber" 2>/dev/null || true
chmod 0755 "$R/usr/lib/steamos/sm8550-audio-setup" "$R/usr/lib/konkr/pbosd" \
  "$R/usr/bin/pbosctl" "$R/usr/bin/konkr-game" "$R/usr/lib/konkr/konkr-standby" \
  "$R/usr/lib/konkr/konkr-volume" "$R/usr/lib/konkr/konkr-sleep" \
  "$R/usr/lib/konkr/konkr-suspend" "$R/usr/lib/konkr/konkr-focusfix" \
  "$R/usr/bin/konkr-apk" "$R/usr/lib/konkr/apk-info" \
  "$R/usr/lib/NetworkManager/dispatcher.d/60-konkr-timesync"
# Game mode: re-activate the game after Quick Access / Steam menu closes.
mkdir -p "$R/usr/lib/systemd/user/gamescope-session.target.wants"
ln -sfn ../konkr-focusfix.service \
  "$R/usr/lib/systemd/user/gamescope-session.target.wants/konkr-focusfix.service"
# Android apps (konkr-apk + Lepton): .apk/.apkm/.xapk/.apks open in it, and
# ~/Android/Inbox auto-installs. /etc links vanish under the /etc overlay.
mkdir -p "$R/usr/lib/systemd/user/default.target.wants"
ln -sfn ../konkr-apk-inbox.path \
  "$R/usr/lib/systemd/user/default.target.wants/konkr-apk-inbox.path"
# First login adds a Google Play Store title.
ln -sfn ../konkr-android-setup.service \
  "$R/usr/lib/systemd/user/default.target.wants/konkr-android-setup.service"
# Lepton rootfs overlays: Play Store, keyboard, pad layout, framework fixes
# (external-and-mods/konkr-android; binaries are fetched/built, not in git).
KAPAY="${ROOT}/external-and-mods/konkr-android/payload"
if [[ -f "$KAPAY/common/system/product/priv-app/Phonesky/Phonesky.apk" ]]; then
  rm -rf "$R/usr/share/konkr-android"
  mkdir -p "$R/usr/share/konkr-android"
  # exFAT/macOS leave ._* AppleDouble files; a ._x.apk breaks PackageManager.
  rsync -a --no-owner --no-group --exclude '._*' --exclude '.DS_Store' "$KAPAY/" "$R/usr/share/konkr-android/"
  chown -R root:root "$R/usr/share/konkr-android"
  find "$R/usr/share/konkr-android" -type d -exec chmod 0755 {} +
  find "$R/usr/share/konkr-android" -type f -exec chmod 0644 {} +
  # Valve's prebaked dalvik-cache files are 0755; keep ours the same.
  find "$R/usr/share/konkr-android" -path '*/data/dalvik-cache/*' -type f -exec chmod 0755 {} +
else
  log "WARN: no konkr-android payload (external-and-mods/konkr-android/build-payload.sh); Android apps will lack the Play Store and fixes"
fi
chroot "$R" update-mime-database /usr/share/mime
chroot "$R" update-desktop-database -q /usr/share/applications
# Opt-in s2idle (pbosctl sleep s2idle): konkr-sleep.service prepares
# Wi-Fi/touch/audio/wake sources. Default sleep is konkr-standby.
mkdir -p "$R/usr/lib/systemd/system/sleep.target.wants"
ln -sfn ../konkr-sleep.service "$R/usr/lib/systemd/system/sleep.target.wants/konkr-sleep.service"
# SSH stays off, like on the Steam Deck: a public image should not listen on
# every user's network. /etc/ssh/sshd_config.d/10-konkr.conf (password login
# for steamos, root off) applies once a user runs `passwd` and
# `sudo systemctl enable --now sshd`. The build rootfs is reused, so drop a
# link left by earlier builds.
rm -f "$R/usr/lib/systemd/system/multi-user.target.wants/sshd.service" \
  "$R/etc/systemd/system/multi-user.target.wants/sshd.service" \
  "$R/var/lib/overlays/etc/upper/systemd/system/multi-user.target.wants/sshd.service"
# Discover: fetch the Flathub catalog (never downloaded on a fresh image).
mkdir -p "$R/usr/lib/systemd/system/timers.target.wants"
ln -sfn ../konkr-flatpak-appstream.timer \
  "$R/usr/lib/systemd/system/timers.target.wants/konkr-flatpak-appstream.timer"
# Speaker volume curve (user session). Vendor wants dir: /etc links written at
# runtime are not seen at boot on SteamOS (overlay mounted late).
mkdir -p "$R/usr/lib/systemd/user/default.target.wants"
ln -sfn ../konkr-volume.service "$R/usr/lib/systemd/user/default.target.wants/konkr-volume.service"
# pbosd: fan curve (ROCKNIX leaves the fan at 70/255), profiles, game-thread
# boost, extra buttons, LEDs. ExecCondition keeps it off non-KONKR devices.
mkdir -p "$R/etc/systemd/system/multi-user.target.wants" "$R/var/lib/pbosd"
ln -sfn /usr/lib/systemd/system/pbosd.service \
  "$R/etc/systemd/system/multi-user.target.wants/pbosd.service"
if [[ -d "$R/var/lib/overlays/etc/upper" ]]; then
  mkdir -p "$R/var/lib/overlays/etc/upper/systemd/system/multi-user.target.wants" \

  ln -sfn /usr/lib/systemd/system/pbosd.service \
    "$R/var/lib/overlays/etc/upper/systemd/system/multi-user.target.wants/pbosd.service"
  # MCU link is on by default (verified on the Pocket FIT: Quick Access and
  # Performance buttons, stick RGB). pbosctl mcu disable re-blacklists it.
  # The build rootfs is reused across builds, so drop a blacklist left by
  # testing `pbosctl mcu disable` — v1.0/v1.1 shipped with the buttons dead.
  rm -f "$R/etc/modprobe.d/konkr-mcu.conf" "$R/var/lib/overlays/etc/upper/modprobe.d/konkr-mcu.conf"
  cp -f "$SM8650_OVL/etc/pbosd.conf" "$R/var/lib/overlays/etc/upper/pbosd.conf"
  # The base image has its own powerdevilrc in the upper layer, which would
  # shadow ours (pbosd owns the power button, Plasma must not act on it).
  mkdir -p "$R/var/lib/overlays/etc/upper/xdg"
  cp -f "$SM8650_OVL/etc/xdg/powerdevilrc" "$R/var/lib/overlays/etc/upper/xdg/powerdevilrc"
  mkdir -p "$R/var/lib/overlays/etc/upper/systemd/coredump.conf.d"
  cp -f "$SM8650_OVL/etc/systemd/coredump.conf.d/10-konkr-sd.conf" \
    "$R/var/lib/overlays/etc/upper/systemd/coredump.conf.d/"
  cp -r "$SM8650_OVL/etc/inputplumber/." "$R/var/lib/overlays/etc/upper/inputplumber/" 2>/dev/null \
    || { mkdir -p "$R/var/lib/overlays/etc/upper/inputplumber"; cp -r "$SM8650_OVL/etc/inputplumber/." "$R/var/lib/overlays/etc/upper/inputplumber/"; }
  # cp keeps the source modes; a tree that went through the exFAT HDD has 0755
  # files (systemd warns about an executable coredump.conf) and 0700 dirs.
  chmod 0644 "$R/var/lib/overlays/etc/upper/pbosd.conf" \
    "$R/var/lib/overlays/etc/upper/xdg/powerdevilrc" \
    "$R/var/lib/overlays/etc/upper/systemd/coredump.conf.d/10-konkr-sd.conf"
  find "$R/var/lib/overlays/etc/upper/inputplumber" \
    \( -type d -exec chmod 0755 {} + \) -o \( -type f -exec chmod 0644 {} + \)
fi

# ---------------------------------------------------------------------------
# SM8550 device overlay (Retroid Pocket 6, AYN Thor): fan curve (Armada's, see
# sm8550-fand). ROCKNIX leaves the RP6 fan at full speed. A rootfs reused from
# another target may carry a stale copy, so remove it on other SoCs.
# ---------------------------------------------------------------------------
SM8550_OVL="${ROOT}/sm8550-overlay"
# Barry Launcher's files under the names earlier builds used.
remove_old_bottom_session() {
  rm -rf "$R/usr/share/steamos-sm8550/thor-dashboard" "$R/usr/share/steamos-sm8550/bottom-shell"
  rm -f "$R/usr/share/steamos-sm8550/bottom-home.qml" \
    "$R/usr/lib/steamos-sm8550/thor-dashboard" "$R/usr/lib/steamos-sm8550/thor-statsd" \
    "$R/usr/lib/steamos-sm8550/thor-shelld" "$R/usr/lib/steamos-sm8550/sm8550-bottom-session" \
    "$R/usr/lib/systemd/user/sm8550-bottom-session.service" \
    "$R/usr/lib/systemd/user/gamescope-session.target.wants/sm8550-bottom-session.service"
}
# The AYN Thor's bottom-screen extras (DEVICE=thor), gone from any other
# build, so a rootfs reused from a Thor build keeps none of them.
remove_thor_bottom_screen() {
  remove_old_bottom_session
  rm -rf "$R/usr/lib/barry_launcher" "$R/usr/share/barry_launcher"
  rm -f "$R/usr/bin/barry-app" \
    "$R/usr/lib/steamos-sm8550/sm8550-run-bottom" \
    "$R/usr/lib/steamos-sm8550/sm8550-thor-backlightd" \
    "$R/usr/lib/systemd/user/barry_launcher.service" \
    "$R/usr/lib/systemd/user/gamescope-session.target.wants/barry_launcher.service" \
    "$R/etc/inputplumber/devices.d/50-ayn_thor.yaml" \
    "$R/var/lib/overlays/etc/upper/inputplumber/devices.d/50-ayn_thor.yaml" \
    "$R/usr/lib/systemd/system/sm8550-thor-backlightd.service" \
    "$R/usr/lib/systemd/system/multi-user.target.wants/sm8550-thor-backlightd.service" \
    "$R/usr/lib/steamos-sm8550/sm8550-thor-controlsd" \
    "$R/usr/lib/systemd/system/sm8550-thor-controlsd.service" \
    "$R/usr/lib/systemd/system/multi-user.target.wants/sm8550-thor-controlsd.service" \
    "$R/usr/lib/systemd/user/barry_launcher_inputd.service" \
    "$R/usr/lib/systemd/user/default.target.wants/barry_launcher_inputd.service" \
    "$R/usr/share/applications/barry_launcher_desktop.desktop" \
    "$R/usr/lib/systemd/user/barry_launcher_desktop.service" \
    "$R/usr/lib/systemd/user/plasma-workspace.target.wants/barry_launcher_desktop.service" \
    "$R/usr/share/applications/org.barry_launcher.keyboard.desktop"
}
if [[ "$SOC" == sm8550 ]]; then
  log "== SM8550 overlay (fan curve, power button, thread boost, steamos-manager devices, Thor touch)"
  install_file "$SM8550_OVL/usr/lib/steamos-sm8550/sm8550-fand" \
    "$R/usr/lib/steamos-sm8550/sm8550-fand" 0755
  install_file "$SM8550_OVL/usr/share/sm8550-fand/fan.conf" \
    "$R/usr/share/sm8550-fand/fan.conf" 0644
  install_file "$SM8550_OVL/usr/lib/systemd/system/sm8550-fand.service" \
    "$R/usr/lib/systemd/system/sm8550-fand.service" 0644
  for t in retroid-pocket6.toml ayn-thor.toml; do
    install_file "$SM8550_OVL/usr/share/steamos-manager/devices/$t" \
      "$R/usr/share/steamos-manager/devices/$t" 0644
  done
  install_file "$SM8550_OVL/usr/lib/steamos-sm8550/sm8550-powerbuttond" \
    "$R/usr/lib/steamos-sm8550/sm8550-powerbuttond" 0755
  install_file "$SM8550_OVL/usr/lib/systemd/system/sm8550-powerbuttond.service" \
    "$R/usr/lib/systemd/system/sm8550-powerbuttond.service" 0644
  install_file "$SM8550_OVL/usr/lib/steamos-sm8550/sm8550-touch-inhibit" \
    "$R/usr/lib/steamos-sm8550/sm8550-touch-inhibit" 0755
  if [[ "$DEVICE" == thor ]]; then
    log "== AYN Thor bottom screen (lease helper, Barry Launcher, backlight and controls daemons)"
    install_file "$SM8550_OVL/usr/lib/steamos-sm8550/sm8550-run-bottom" \
      "$R/usr/lib/steamos-sm8550/sm8550-run-bottom" 0755
    # Barry Launcher: the AYN Thor bottom screen's home screen, apps, keyboard
    # and performance dashboard. Replaced whole, so removed files do not
    # linger; earlier builds' names (thor-*, sm8550-bottom-session) go.
    remove_old_bottom_session
    rm -rf "$R/usr/lib/barry_launcher" "$R/usr/share/barry_launcher"
    mkdir -p "$R/usr/lib/barry_launcher" "$R/usr/share"
    for f in barry_launcher_session barry_launcher_dashboard barry_launcher_statsd barry_launcher_shelld \
      barry_launcher_inputd barry_launcher_desktop barry-app; do
      install_file "$SM8550_OVL/usr/lib/barry_launcher/$f" "$R/usr/lib/barry_launcher/$f" 0755
    done
    # Glide typing's decoder, the Desktop Mode (KWin) side and user apps
    # (barry_apps, also barry-app's), imported by barry_launcher_shelld.
    for f in barry_glide.py barry_desktop.py barry_trackpad.py barry_apps.py; do
      install_file "$SM8550_OVL/usr/lib/barry_launcher/$f" "$R/usr/lib/barry_launcher/$f" 0644
    done
    # barry-app: install and make Barry Launcher apps from a terminal.
    ln -sfn ../lib/barry_launcher/barry-app "$R/usr/bin/barry-app"
    # Desktop Mode: Barry's keyboard as KWin's input method, a small Wayland
    # client built here against the rootfs's libwayland.
    "${ROOT}/external-and-mods/barry-launcher-imd/build.sh" "$R" "$R/usr/lib/barry_launcher/barry_launcher_imd" \
      || die "cannot build barry_launcher_imd (the rootfs needs gcc, wayland-scanner and wayland-protocols)"
    cp -r "$SM8550_OVL/usr/share/barry_launcher" "$R/usr/share/"
    chmod -R u=rwX,go=rX "$R/usr/share/barry_launcher"
    install_file "$SM8550_OVL/usr/lib/systemd/user/barry_launcher.service" \
      "$R/usr/lib/systemd/user/barry_launcher.service" 0644
    mkdir -p "$R/usr/lib/systemd/user/gamescope-session.target.wants"
    ln -sfn ../barry_launcher.service \
      "$R/usr/lib/systemd/user/gamescope-session.target.wants/barry_launcher.service"
    # The top screen's trackpad and keyboard (uinput), in Game Mode and
    # Desktop Mode; the Desktop Mode window starts from the applications menu.
    install_file "$SM8550_OVL/usr/lib/systemd/user/barry_launcher_inputd.service" \
      "$R/usr/lib/systemd/user/barry_launcher_inputd.service" 0644
    mkdir -p "$R/usr/lib/systemd/user/default.target.wants"
    ln -sfn ../barry_launcher_inputd.service \
      "$R/usr/lib/systemd/user/default.target.wants/barry_launcher_inputd.service"
    install_file "$SM8550_OVL/usr/share/applications/barry_launcher_desktop.desktop" \
      "$R/usr/share/applications/barry_launcher_desktop.desktop" 0644
    # Desktop Mode: Barry on the bottom screen with the Plasma session, and
    # its keyboard registered as a virtual keyboard KWin can use.
    install_file "$SM8550_OVL/usr/lib/systemd/user/barry_launcher_desktop.service" \
      "$R/usr/lib/systemd/user/barry_launcher_desktop.service" 0644
    mkdir -p "$R/usr/lib/systemd/user/plasma-workspace.target.wants"
    ln -sfn ../barry_launcher_desktop.service \
      "$R/usr/lib/systemd/user/plasma-workspace.target.wants/barry_launcher_desktop.service"
    install_file "$SM8550_OVL/usr/share/applications/org.barry_launcher.keyboard.desktop" \
      "$R/usr/share/applications/org.barry_launcher.keyboard.desktop" 0644
    # AYN Thor: InputPlumber leaves the AYN button to sm8550-thor-backlightd,
    # which uses it to show the bottom-screen dashboard.
    ip_thor="$R/usr/share/inputplumber/devices/50-ayn_thor.yaml"
    if [[ -f "$ip_thor" ]]; then
      for etc in "$R/etc" "$R/var/lib/overlays/etc/upper"; do
        [[ "$etc" == "$R/etc" || -d "$etc" ]] || continue
        mkdir -p "$etc/inputplumber/devices.d"
        python3 "$SM8550_OVL/usr/share/steamos-sm8550/ip-thor-without-ayn-key.py" \
          "$ip_thor" "$etc/inputplumber/devices.d/50-ayn_thor.yaml"
      done
    fi
    # Steer Steam's brightness writes to the Thor's top panel (see the .inc).
    if [[ -f "$R/usr/bin/steamos-polkit-helpers/steamos-priv-write" ]]; then
      python3 "$SM8550_OVL/usr/share/steamos-sm8550/insert-priv-write-backlight.py" \
        "$R/usr/bin/steamos-polkit-helpers/steamos-priv-write" \
        "$SM8550_OVL/usr/share/steamos-sm8550/priv-write-backlight.inc"
    fi
    install_file "$SM8550_OVL/usr/lib/steamos-sm8550/sm8550-thor-backlightd" \
      "$R/usr/lib/steamos-sm8550/sm8550-thor-backlightd" 0755
    install_file "$SM8550_OVL/usr/lib/systemd/system/sm8550-thor-backlightd.service" \
      "$R/usr/lib/systemd/system/sm8550-thor-backlightd.service" 0644
    mkdir -p "$R/usr/lib/systemd/system/multi-user.target.wants"
    ln -sfn ../sm8550-thor-backlightd.service \
      "$R/usr/lib/systemd/system/multi-user.target.wants/sm8550-thor-backlightd.service"
    # Barry Launcher's quick controls: fan profile and stick lighting (root).
    install_file "$SM8550_OVL/usr/lib/steamos-sm8550/sm8550-thor-controlsd" \
      "$R/usr/lib/steamos-sm8550/sm8550-thor-controlsd" 0755
    install_file "$SM8550_OVL/usr/lib/systemd/system/sm8550-thor-controlsd.service" \
      "$R/usr/lib/systemd/system/sm8550-thor-controlsd.service" 0644
    ln -sfn ../sm8550-thor-controlsd.service \
      "$R/usr/lib/systemd/system/multi-user.target.wants/sm8550-thor-controlsd.service"
  else
    remove_thor_bottom_screen
  fi
  # uclamp boost for game and Steam UI threads (no affinity).
  install_file "$SM8550_OVL/usr/lib/steamos-sm8550/sm8550-boostd" \
    "$R/usr/lib/steamos-sm8550/sm8550-boostd" 0755
  install_file "$SM8550_OVL/usr/lib/systemd/system/sm8550-boostd.service" \
    "$R/usr/lib/systemd/system/sm8550-boostd.service" 0644
  install_file "$SM8550_OVL/usr/lib/udev/rules.d/73-sm8550-ufs-sleep.rules" \
    "$R/usr/lib/udev/rules.d/73-sm8550-ufs-sleep.rules" 0644
  install_file "$SM8550_OVL/usr/lib/udev/rules.d/74-sm8550-ufs-serial.rules" \
    "$R/usr/lib/udev/rules.d/74-sm8550-ufs-serial.rules" 0644
  # Output volume across reboots (pro-audio outputs have no saved routes).
  install_file "$SM8550_OVL/usr/lib/steamos-sm8550/sm8550-volume-keeper" \
    "$R/usr/lib/steamos-sm8550/sm8550-volume-keeper" 0755
  install_file "$SM8550_OVL/usr/lib/systemd/user/sm8550-volume-keeper.service" \
    "$R/usr/lib/systemd/user/sm8550-volume-keeper.service" 0644
  mkdir -p "$R/usr/lib/systemd/user/default.target.wants"
  ln -sfn ../sm8550-volume-keeper.service \
    "$R/usr/lib/systemd/user/default.target.wants/sm8550-volume-keeper.service"
  # Earlier Thor builds kept the bottom backlight root-only; the backlight
  # daemon needs Steam to write it directly again.
  rm -f "$R/usr/lib/udev/rules.d/74-sm8550-thor-backlight.rules" \
    "$R/usr/lib/udev/rules.d/99-sm8550-thor-backlight.rules"
  install_file "$SM8550_OVL/usr/lib/udev/rules.d/72-sm8550-touch-inhibit.rules" \
    "$R/usr/lib/udev/rules.d/72-sm8550-touch-inhibit.rules" 0644
  mkdir -p "$R/usr/lib/systemd/system/multi-user.target.wants"
  for u in sm8550-fand.service sm8550-powerbuttond.service \
           sm8550-boostd.service; do
    ln -sfn ../$u "$R/usr/lib/systemd/system/multi-user.target.wants/$u"
  done
else
  remove_old_bottom_session
  rm -rf "$R/usr/lib/steamos-sm8550" "$R/usr/share/sm8550-fand" \
    "$R/usr/lib/barry_launcher" "$R/usr/share/barry_launcher"
  rm -f "$R/usr/share/steamos-manager/devices/retroid-pocket6.toml" \
    "$R/usr/share/steamos-manager/devices/ayn-thor.toml" \
    "$R/usr/lib/udev/rules.d/72-sm8550-touch-inhibit.rules" \
    "$R/usr/lib/systemd/system/sm8550-powerbuttond.service" \
    "$R/usr/lib/systemd/system/multi-user.target.wants/sm8550-powerbuttond.service" \
    "$R/usr/lib/systemd/system/sm8550-fand.service" \
    "$R/usr/lib/systemd/system/multi-user.target.wants/sm8550-fand.service" \
    "$R/usr/lib/udev/rules.d/73-sm8550-ufs-sleep.rules" \
    "$R/usr/lib/systemd/user/barry_launcher.service" \
    "$R/usr/lib/systemd/user/gamescope-session.target.wants/barry_launcher.service" \
    "$R/etc/inputplumber/devices.d/50-ayn_thor.yaml" \
    "$R/var/lib/overlays/etc/upper/inputplumber/devices.d/50-ayn_thor.yaml" \
    "$R/usr/lib/systemd/system/sm8550-thor-backlightd.service" \
    "$R/usr/lib/systemd/system/multi-user.target.wants/sm8550-thor-backlightd.service" \
    "$R/usr/lib/systemd/system/sm8550-boostd.service" \
    "$R/usr/lib/systemd/system/multi-user.target.wants/sm8550-boostd.service" \
    "$R/usr/lib/udev/rules.d/74-sm8550-ufs-serial.rules" \
    "$R/usr/lib/systemd/user/sm8550-volume-keeper.service" \
    "$R/usr/lib/systemd/user/default.target.wants/sm8550-volume-keeper.service"
fi

# CPU, GPU and memory tuning shared by SM8550 and SM8650 (measured on the
# Retroid Pocket 6, carried to the Pocket FIT).
log "== CPU/GPU/memory tuning (GPU IRQs, no CPU pins, zram, TEO, backlight uevents)"
# GPU interrupts off the little cores: the A740's GMU wedges otherwise (the
# A750 has the same GMU).
install_file "$SM8550_OVL/usr/lib/steamos-sm8550/sm8550-irq-affinity" \
  "$R/usr/lib/steamos-sm8550/sm8550-irq-affinity" 0755
install_file "$SM8550_OVL/usr/lib/systemd/system/sm8550-irq-affinity.service" \
  "$R/usr/lib/systemd/system/sm8550-irq-affinity.service" 0644
mkdir -p "$R/usr/lib/systemd/system/multi-user.target.wants"
ln -sfn ../sm8550-irq-affinity.service \
  "$R/usr/lib/systemd/system/multi-user.target.wants/sm8550-irq-affinity.service"
# No hard CPU pins: the Frame keeps system services on cpu1-4, the session
# on cpu0-4 and games on cpu3-7. Give all of them every core; the scheduler
# places them.
install_file "$SM8550_OVL/usr/lib/systemd/system.conf.d/60-sm8550-cpu-affinity.conf" \
  "$R/usr/lib/systemd/system.conf.d/60-sm8550-cpu-affinity.conf" 0644
install_file "$SM8550_OVL/usr/lib/systemd/user.conf.d/60-sm8550-cpu-affinity.conf" \
  "$R/usr/lib/systemd/user.conf.d/60-sm8550-cpu-affinity.conf" 0644
install_file "$SM8550_OVL/usr/lib/systemd/user/steam.service.d/70-sm8550-cpu-affinity.conf" \
  "$R/usr/lib/systemd/user/steam.service.d/70-sm8550-cpu-affinity.conf" 0644
# Brightness changes no longer flood udisksd through systemd device units.
install_file "$SM8550_OVL/usr/lib/udev/rules.d/99-zz-sm8550-backlight-nosystemd.rules" \
  "$R/usr/lib/udev/rules.d/99-zz-sm8550-backlight-nosystemd.rules" 0644
# zram: zstd, RAM-sized up to 8 GB (the 8 GB models ran out of swap in Palworld).
install_file "$SM8550_OVL/usr/lib/systemd/zram-generator.conf.d/60-sm8550-zram.conf" \
  "$R/usr/lib/systemd/zram-generator.conf.d/60-sm8550-zram.conf" 0644
# TEO cpuidle governor (same fps as menu, ~4 % less power in game).
install_file "$SM8550_OVL/usr/lib/tmpfiles.d/sm8550-cpuidle-teo.conf" \
  "$R/usr/lib/tmpfiles.d/sm8550-cpuidle-teo.conf" 0644
# Default CPU scheduler: EAS, not LAVD (sm8550-boostd or pbosd add the
# uclamp boost). On 7.2, LAVD 1.1.2 pins the little and mid cores at max
# clock in games for no gain. RP6, EDC: 58.5 fps at 7.4 W on EAS vs 58.1 fps
# at 8.5 W on LAVD. Pocket FIT, Tomb Raider (2x SSAA): 38.4 vs 38.9 fps, but
# 1 % low +5.7 %, 0.1 % low +19 % and 4 % less power on EAS.
for f in "$R/etc/default/steamos-scheduler" \
         "$R/var/lib/overlays/etc/upper/default/steamos-scheduler"; do
  if [[ -f "$f" ]]; then sed -i 's/^SCHEDULER=.*/SCHEDULER=none/' "$f"; fi
done

# Tailscale: in every image but off, with no account or keys (see the
# script). TAILSCALE=0 leaves it out; BUNDLE_TAILSCALE=0 still works too.
if [[ "${TAILSCALE:-${BUNDLE_TAILSCALE:-1}}" == 1 ]]; then
  "${SCRIPT_DIR}/install-tailscale.sh" "$R" install
else
  "${SCRIPT_DIR}/install-tailscale.sh" "$R" remove
fi

# ---------------------------------------------------------------------------
# MangoHud: keep SteamOS stock binaries. Host Ubuntu mangoapp needs GLIBC_2.43
# (SteamOS is 2.39) and crash-loops gamescopereaper / the session.
# Build from external-and-mods/MangoHud against SteamOS glibc before replacing.
# ---------------------------------------------------------------------------
log "== MangoHud (stock SteamOS — host Ubuntu mango needs GLIBC_2.43)"
for b in mangohud mangoapp mangohudctl; do
  if [[ -f "$STOCK/usr/bin/$b" ]]; then
    install_file "$STOCK/usr/bin/$b" "$R/usr/bin/$b" 0755
  fi
done
for lib in libMangoHud.so libMangoHud_opengl.so libMangoHud_shim.so libMangoHud-next.so; do
  if [[ -f "$STOCK/usr/lib/$lib" ]]; then
    install_file "$STOCK/usr/lib/$lib" "$R/usr/lib/$lib" 0755
  fi
done

# ---------------------------------------------------------------------------
# lsfg-vk
# ---------------------------------------------------------------------------
log "== lsfg-vk 2.0 (unmodified ARM layer + Decky x86 runtime)"
# Reused roots must not register the old 1.x layer alongside version 2.
for prefix in "$R/usr" "$R/usr/local"; do
  rm -f "$prefix/lib/liblsfg-vk.so" "$prefix/lib/liblsfg-vk-arm64.so" \
    "$prefix/share/vulkan/implicit_layer.d/VkLayer_LS_frame_generation.json" \
    "$prefix/share/vulkan/implicit_layer.d/VkLayer_LS_frame_generation_arm64.json"
done
[[ -r "$R/usr/lib/liblsfg-vk-layer-arm64.so" ]] || die "missing LSFG v2 ARM layer"

# ---------------------------------------------------------------------------
# Mesa Turnip
# ---------------------------------------------------------------------------
# Pin the Frame's Turnip for mangoapp (zink must match its own Turnip; see
# /usr/lib/steamos-sm8650/bin/mangoapp). Taken before any override below.
log "== pin Frame Turnip for the Performance Overlay"
FRAME_TURNIP="$R/usr/lib/libvulkan_freedreno.so"
[[ -f "$STOCK/usr/lib/libvulkan_freedreno.so" ]] && FRAME_TURNIP="$STOCK/usr/lib/libvulkan_freedreno.so"
mkdir -p "$R/usr/lib/steamos-sm8650/frame-turnip" "$R/usr/share/steamos-sm8650"
install_file "$FRAME_TURNIP" "$R/usr/lib/steamos-sm8650/frame-turnip/libvulkan_freedreno.so" 0755
cat >"$R/usr/share/steamos-sm8650/frame-turnip_icd.aarch64.json" <<'JSON'
{
    "ICD": {
        "api_version": "1.4.362",
        "library_arch": "64",
        "library_path": "/usr/lib/steamos-sm8650/frame-turnip/libvulkan_freedreno.so"
    },
    "file_format_version": "1.0.1"
}
JSON
chmod 0755 "$R/usr/lib/steamos-sm8650/bin/mangoapp" 2>/dev/null || true

if [[ -n "$MESA_SO" ]]; then
  log "== Mesa override $MESA_SO"
  backup "$R/usr/lib/libvulkan_freedreno.so" "$STOCK/usr/lib/libvulkan_freedreno.so"
  install_file "$MESA_SO" "$R/usr/lib/libvulkan_freedreno.so" 0755
else
  log "== Mesa: keeping Frame Turnip (Adreno 750 = this SoC)"
fi

# ---------------------------------------------------------------------------
# User home (steamos uid 1000)
# ---------------------------------------------------------------------------
log "== home/steamos (Decky plugin + configs)"
if [[ -n "${STEAMOS_HOME:-}" ]]; then
  HOME_DST="$STEAMOS_HOME"
elif [[ -d /run/media/steam/home/steamos && "$R" == /run/media/steam/root ]]; then
  HOME_DST=/run/media/steam/home/steamos
else
  HOME_DST="$R/home/steamos"
fi
mkdir -p "$HOME_DST"
# Copy plugin tree (resolve lsfg .so symlink into a real file if needed)
# .local/lib/liblsfg-vk.so is a link to the SM8550 builder's host library;
# skip it (decky-lsfg-vk installs its own copy).
rsync -a --copy-links --exclude '.local/lib/liblsfg-vk.so' "${MOD}/Decky/Plug-ins/" "$HOME_DST/"
# Decky itself. Upstream left it to a first-boot installer in ARM-Manager
# that most people never found — no Decky, so no PB-OS Control either.
DECKY_VERSION=v3.2.9
DECKY_LOADER="${MOD}/Decky/loader/PluginLoader-${DECKY_VERSION}"
if [[ ! -s "$DECKY_LOADER" ]]; then
  mkdir -p "${DECKY_LOADER%/*}"
  curl -fL -o "$DECKY_LOADER.part" \
    "https://github.com/SteamDeckHomebrew/decky-loader/releases/download/${DECKY_VERSION}/PluginLoader" &&
    mv "$DECKY_LOADER.part" "$DECKY_LOADER"
fi
[[ -s "$DECKY_LOADER" ]] || die "Decky loader ${DECKY_VERSION} missing and download failed"
mkdir -p "$HOME_DST/homebrew/services" "$HOME_DST/homebrew/settings" "$HOME_DST/homebrew/data" "$HOME_DST/homebrew/logs"
# ~/.cache must exist (user-owned, see chown below) before anything running
# as root with HOME=/home/steamos can create it root-owned.
mkdir -p "$HOME_DST/.cache"
install -m0755 "$DECKY_LOADER" "$HOME_DST/homebrew/services/PluginLoader"
printf '%s' "$DECKY_VERSION" >"$HOME_DST/homebrew/services/.loader.version"
mkdir -p "$R/usr/lib/systemd/system/multi-user.target.wants"
ln -sfn ../plugin_loader.service "$R/usr/lib/systemd/system/multi-user.target.wants/plugin_loader.service"
# Fix lsfg-vk home paths
if [[ -f "$HOME_DST/.config/lsfg-vk/conf.toml" ]]; then
  sed -i 's|/home/steam/|/home/steamos/|g' "$HOME_DST/.config/lsfg-vk/conf.toml"
fi
if [[ -f "$HOME_DST/.local/share/vulkan/implicit_layer.d/VkLayer_LS_frame_generation.json" ]]; then
  python3 - "$HOME_DST" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]) / ".local/share/vulkan/implicit_layer.d/VkLayer_LS_frame_generation.json"
txt = p.read_text()
txt = txt.replace("/home/steam/", "/home/steamos/")
p.write_text(txt)
PY
fi
rm -f "$HOME_DST/.local/lib/liblsfg-vk.so" \
  "$HOME_DST/.local/share/vulkan/implicit_layer.d/VkLayer_LS_frame_generation.json"
rm -f "$HOME_DST/LEEME-ODIN.txt" "$HOME_DST/README-ODIN.txt"

# Frame steam.tar.zst is an incomplete client (spinner, no package zips).
# Bake a complete ARM client (seed binaries from host if present), then
# strip login/account data so first boot is a clean Steam Deck login.
STEAM_HOME="$HOME_DST/.local/share/Steam"
log "== complete Steam ARM client"
mkdir -p "$STEAM_HOME"
if [[ -x "${SCRIPT_DIR}/install-complete-steam-client.sh" ]]; then
  "${SCRIPT_DIR}/install-complete-steam-client.sh" "$STEAM_HOME" \
    || die "complete Steam client installation failed"
fi
if [[ -x "$R/usr/lib/steamos/sm8550-patch-steamui" && -d "$STEAM_HOME/steamui" ]]; then
  "$R/usr/lib/steamos/sm8550-patch-steamui" "$STEAM_HOME/steamui" || true
fi
touch "$STEAM_HOME/.install-complete"
# Steam UI through ANGLE-Vulkan on Turnip instead of ANGLE -> GL -> zink
# (see pbosd ensure_webhelper_vulkan, which keeps it after client updates).
WH="$STEAM_HOME/steamrtarm64/steamwebhelper.sh"
if [[ -f "$WH" ]] && ! grep -q KONKR_CEF_FLAGS "$WH"; then
  python3 - "$WH" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1]); s = p.read_text()
old = 'exec taskset 0x7c $(pwd)/steamwebhelper "$@" &> ~/.steam/steam/logs/steamwebhelper.log'
new = ('KONKR_CEF_FLAGS="--use-gl=angle --use-angle=vulkan '
       '--enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE"\n'
       'exec taskset 0x7c $(pwd)/steamwebhelper "$@" $KONKR_CEF_FLAGS &> ~/.steam/steam/logs/steamwebhelper.log')
if old in s:
    p.write_text(s.replace(old, new))
PY
fi
install_file "$OVL/usr/share/deckard/RUNSTEAM.sh" \
  "$STEAM_HOME/RUNSTEAM.sh" 0755
if [[ -d "$STEAM_HOME/linuxarm64" && -d "$STEAM_HOME/steamrtarm64" ]]; then
  for _lib in steamclient.so crashhandler.so steam-launch-wrapper; do
    if [[ -s "$STEAM_HOME/steamrtarm64/${_lib}" && ! -s "$STEAM_HOME/linuxarm64/${_lib}" ]]; then
      cp -f "$STEAM_HOME/steamrtarm64/${_lib}" "$STEAM_HOME/linuxarm64/${_lib}"
    fi
  done
  unset _lib
fi

# Desktop: only Return to Gaming Mode. Decky lives in ARM-Manager.
mkdir -p "$HOME_DST/Desktop" "$R/usr/share/applications" "$R/usr/share/icons/hicolor/scalable/apps"
rm -f "$HOME_DST/Desktop/Decky Loader.desktop" "$HOME_DST/Desktop/install-decky.desktop"

# ---------------------------------------------------------------------------
# Plasma extras + ARM-Manager + LSFG/Thor/Decky plugins
# ---------------------------------------------------------------------------
log "== plasma extras (holo kate/ark/networkmanager-qt/…)"
# AYN Thor: xdotool for the bottom-screen keyboard (Firefox below).
extra_pkgs=""
[[ "$DEVICE" == thor ]] && extra_pkgs="libxss xdotool"
STEAMOS_HOME="$HOME_DST" EXTRA_PKGS="$extra_pkgs" "${SCRIPT_DIR}/install-plasma-extras.sh" "$R" \
  || log "WARN: plasma extras incomplete"

# AYN Thor: Firefox for the bottom screen (Barry Launcher's browser
# and Discord). Mozilla's own Linux ARM64 build: self-contained (its own
# NSS; needs glibc 2.28) and current, where the arm64 build in Valve's
# repos lags behind (152). Its updater is off: updates come with our images.
FIREFOX_VERSION=157.0
FIREFOX_SHA256=73fc3d6f6f4d3fcdeee59db90156568af9959405120ad686e535f572995074d0
if [[ "$DEVICE" == thor ]]; then
  log "== Firefox ${FIREFOX_VERSION} (Mozilla, linux-aarch64)"
  ff_tar="${WORKDIR}/cache/firefox-${FIREFOX_VERSION}-linux-aarch64.tar.xz"
  if [[ ! -s "$ff_tar" ]]; then
    mkdir -p "${ff_tar%/*}"
    curl -fL -o "$ff_tar.part" \
      "https://archive.mozilla.org/pub/firefox/releases/${FIREFOX_VERSION}/linux-aarch64/en-US/firefox-${FIREFOX_VERSION}.tar.xz" &&
      mv "$ff_tar.part" "$ff_tar"
  fi
  [[ -s "$ff_tar" && $(sha256sum "$ff_tar" | awk '{print $1}') == "$FIREFOX_SHA256" ]] \
    || die "Firefox ${FIREFOX_VERSION} missing, or its checksum does not match"
  # Replaces Valve's package from earlier images (same path: Barry Launcher
  # takes the Firefox logo from it).
  rm -rf "$R/usr/lib/firefox" "$R/usr/bin/firefox"
  tar -xJf "$ff_tar" -C "$R/usr/lib"
  mkdir -p "$R/usr/lib/firefox/distribution"
  printf '{"policies": {"DisableAppUpdate": true}}\n' >"$R/usr/lib/firefox/distribution/policies.json"
  ln -sfn ../lib/firefox/firefox "$R/usr/bin/firefox"
  install -D -m0644 /dev/stdin "$R/usr/share/applications/firefox.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Firefox
GenericName=Web Browser
Exec=firefox %u
Icon=/usr/lib/firefox/browser/chrome/icons/default/default128.png
Categories=Network;WebBrowser;
MimeType=text/html;x-scheme-handler/http;x-scheme-handler/https;
StartupWMClass=firefox
EOF
elif [[ -f "$R/usr/lib/firefox/distribution/policies.json" ]] \
  && ! grep -qsx 'usr/lib/firefox/' "$R"/var/lib/pacman/local/*/files; then
  # Our Mozilla build in a rootfs reused from a Thor build (no package owns it).
  log "== remove the Thor build's Firefox"
  rm -rf "$R/usr/lib/firefox" "$R/usr/bin/firefox" "$R/usr/share/applications/firefox.desktop"
fi
if [[ ! -f "$R/usr/lib/qt6/plugins/plasma/kcms/systemsettings/kcm_kscreen.so" ]]; then
  log "== official Plasma kscreen 6.2.5 KCM"
  "${SCRIPT_DIR}/build-kscreen-6.2.5.sh" "$R" \
    || die "kscreen 6.2.5 is required (Display Configuration)"
fi
if [[ ! -f "$R/usr/lib/qt6/plugins/plasma/kcms/systemsettings_qwidgets/kcm_networkmanagement.so" ]]; then
  log "== KF6 NetworkManagerQt 6.14 (plasma-nm needs >= 6.5)"
  "${SCRIPT_DIR}/build-kf6-nm-qt-6.14.sh" "$R" \
    || die "networkmanager-qt 6.14 is required"
  log "== official Plasma plasma-nm 6.2.5 (Network Manager)"
  "${SCRIPT_DIR}/build-plasma-nm-6.2.5.sh" "$R" \
    || die "plasma-nm 6.2.5 is required (Network Manager)"
fi
if [[ ! -x "$R/usr/bin/plasma-keyboard" ]]; then
  log "== plasma-keyboard 0.1.0 (desktop touch keyboard)"
  "${SCRIPT_DIR}/build-plasma-keyboard.sh" "$R" \
    || die "plasma-keyboard is required (Desktop Mode touch keyboard)"
fi
# extras skip ALARM Gear (Qt_6.11). Build official 26.04.2 for Qt 6.8.
needs_gear_qt68() {
  local bin="$R/usr/bin/$1"
  [[ ! -x "$bin" ]] && return 0
  strings "$bin" 2>/dev/null | grep 'Qt_6\.11' >/dev/null
}
for _gear in ark kcalc filelight gwenview okular; do
  if needs_gear_qt68 "$_gear"; then
    log "== official ${_gear} 26.04.2 for Qt 6.8"
    "${SCRIPT_DIR}/build-kde-gear-26.04.2.sh" "$R" "$_gear" \
      || die "required desktop app ${_gear} build failed"
  fi
done
log "== vendor apps (UFS, MESA, Proton-ARM, Non-Steam, SRM)"
STEAMOS_HOME="$HOME_DST" "${SCRIPT_DIR}/install-vendor-apps.sh" "$R" \
  || die "required vendor apps (installer/updater) failed"
log "== system fixes (LSFG-VK, Thor, Decky plugins, Return icon)"
STEAMOS_HOME="$HOME_DST" "${SCRIPT_DIR}/install-system-fixes.sh" "$R" \
  || log "WARN: system fixes incomplete"

# ---------------------------------------------------------------------------
# Ownership / extras
# ---------------------------------------------------------------------------
log "== permissions"
chown -R 1000:1000 "$HOME_DST"
chmod 0755 "$HOME_DST"
# NetworkManager refuses plugins/scripts not owned by root (wifi/bt stay dead).
if [[ -d "$R/usr/lib/NetworkManager" ]]; then
  chown -R root:root "$R/usr/lib/NetworkManager" || true
  find "$R/usr/lib/NetworkManager" -type f -name '*.so' -exec chmod 0755 {} + || true
fi
if [[ -d "$R/etc/NetworkManager" ]]; then
  chown -R root:root "$R/etc/NetworkManager" || true
fi
if [[ -d "$R/var/lib/overlays/etc/upper/NetworkManager" ]]; then
  chown -R root:root "$R/var/lib/overlays/etc/upper/NetworkManager" || true
fi
# User session PipeWire.
if [[ -d "$HOME_DST" ]]; then
  mkdir -p "$HOME_DST/.config/systemd/user/default.target.wants"
  for u in pipewire.service pipewire-pulse.service sm8550-audio-pipewire.service sm8550-volume-keys.service; do
    src="/usr/lib/systemd/user/${u}"
    [[ -f "$R${src}" ]] || continue
    ln -sfn "$src" "$HOME_DST/.config/systemd/user/default.target.wants/${u}"
  done
fi
# SteamOS empty-password user stays as extracted (steamos:: in shadow)

# ldconfig cache is arch-specific; skip. Dynamic linker will still find /usr/lib.

# Empty mount points the bwrap builds (gamescope/box64) leave in the rootfs.
rmdir "$R/src/box64" "$R/src/gamescope" "$R/src" "$R/build-parent" 2>/dev/null || true

# Desktop Mode look: Frame's steamos-set-plasma-theme only picks the Deck
# theme on Jupiter/Galileo boards. com.valve.vapor.desktop has no splash,
# so Plasma shows its stock "KDE Plasma" one instead of the Steam logo.
if [[ -f "$R/etc/xdg/kdeglobals" ]]; then
  sed -i 's/^LookAndFeelPackage=.*/LookAndFeelPackage=com.valve.vapor.deck.desktop/' "$R/etc/xdg/kdeglobals"
fi

log "== source permissions"
# cp -a keeps the source tree's modes, and a copy that went through the
# exFAT HDD has 0700 dirs and 0600 files. gamescope runs as steamos: with an
# unreadable /usr/share/gamescope/scripts it never defines debug(), the
# KONKR display script aborts it and Game Mode is a black screen.
for d in usr/share usr/local/share usr/lib/konkr usr/lib/steamos etc/gamescope etc/inputplumber; do
  [[ -d "$R/$d" ]] || continue
  find "$R/$d" -xdev \( -path '*/guestos' -o -path '*/factory/root' \) -prune -o \
    -type d \( ! -perm -o=rx -o ! -perm -u=x \) -exec chmod u+rwx,go+rx {} + -o \
    -type f ! -perm -o=r -exec chmod go+r {} +
done
find "$R" -xdev -name '._*' -type f -delete 2>/dev/null || true
# Nothing under /usr or /etc belongs to a regular user. A tarball unpacked
# with its owner (uid 1001) made /usr unowned: polkit, D-Bus and sshd then
# ignore their files, and Discover, Decky and more break.
find "$R/usr" "$R/etc" -xdev \( -uid +999 -o -gid +999 \) -exec chown -h root:root {} + 2>/dev/null || true

log "== summary"
{
  echo "gamescope: $(file -b "$R/usr/bin/gamescope")"
  echo "KERNEL:    $(file -b "$R/boot/KERNEL")"
  echo "modules:   $R/usr/lib/modules/$KREL"
  echo "mesa:      $(ls -l "$R/usr/lib/libvulkan_freedreno.so")"
  echo "display-info.so.3: $(ls -l "$R/usr/lib/libdisplay-info.so.3" 2>/dev/null || echo missing)"
  echo "lsfg:      $(ls -l "$R/usr/local/lib/liblsfg-vk.so" 2>/dev/null || echo missing)"
  echo "fixpad:    $(ls -l "$R/usr/lib/steamos/sm8550-fixpad" 2>/dev/null || echo missing)"
  echo "inputplumber: $(ls -l "$R/usr/bin/inputplumber" 2>/dev/null || echo missing)"
  echo "deck-uhid: $(grep -A2 target_devices "$R/etc/inputplumber/devices.d/02-ayn-odin.yaml" 2>/dev/null || echo missing)"
  echo "home:      $(find "$HOME_DST" -maxdepth 3 -printf '%p\n' | head -40)"
} | tee -a "$LOG"

log "OK"
