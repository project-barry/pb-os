#!/usr/bin/env bash
# Install ShadowBlip InputPlumber + SM8550 composites into a SteamOS rootfs:
# a real Steam Deck controller (deck target, USB/IP) + keyboard target (touch
# OSK haptic).
# USB/Bluetooth HID is ignored in the composite so it is not grabbed.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
R="${1:-${ROOT}/rootfs}"
OVL="${ROOT}/steamos-overlay"
CACHE="${ROOT}/external-and-mods/InputPlumber"
# 0.79+ drives the rumble motors of the AYANEO/KONKR pad in its HID mode.
IP_VER="${INPUTPLUMBER_VERSION:-0.81.0}"
IP_SHA256_0_81_0="5509a6f79bec95c240de34833639e3ba8a144752e3745e6eb978b38b8aa05a10"
TGZ="${CACHE}/inputplumber-${IP_VER}-aarch64.tar.gz"
TGZ_URL="https://github.com/ShadowBlip/InputPlumber/releases/download/v${IP_VER}/inputplumber-aarch64.tar.gz"

log() { printf '==> [inputplumber] %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[[ -d "${R}/usr" ]] || die "not a rootfs: ${R}"
mkdir -p "${CACHE}"

fetch() {
  local url="$1" dest="$2"
  [[ -s "$dest" ]] && return 0
  log "Downloading ${url}"
  if command -v curl >/dev/null; then
    curl -fL --retry 3 -o "${dest}.part" "$url"
  else
    wget -O "${dest}.part" "$url"
  fi
  mv -f "${dest}.part" "$dest"
}

install_tarball() {
  if [[ ! -s "$TGZ" ]]; then
    fetch "$TGZ_URL" "$TGZ"
  fi
  [[ -s "$TGZ" ]] || die "missing ${TGZ}"
  # Pinned hash for the default version; for an override, the release's own.
  local want_var="IP_SHA256_${IP_VER//./_}" want got
  want="${!want_var:-}"
  if [[ -z "$want" ]]; then
    fetch "${TGZ_URL}.sha256.txt" "${TGZ}.sha256.txt"
    want="$(awk '{print $1; exit}' "${TGZ}.sha256.txt")"
  fi
  got="$(sha256sum "$TGZ" | awk '{print $1}')"
  if [[ "$want" != "$got" ]]; then
    rm -f "$TGZ"
    die "InputPlumber ${IP_VER} checksum mismatch (got ${got}); removed the download, run again"
  fi
  local stage="${CACHE}/extract"
  rm -rf "$stage"
  mkdir -p "$stage"
  # --no-same-owner: the release tarball is owned by uid 1001, and cp -a
  # below would hand /usr, /usr/lib, /usr/share … to that uid.
  tar -C "$stage" --no-same-owner -xzf "$TGZ"
  chown -R root:root "$stage"
  local src="$stage"
  if [[ ! -x "${src}/usr/bin/inputplumber" ]]; then
    local inner
    inner="$(find "$stage" -type f -name inputplumber -path '*/usr/bin/*' | head -1)"
    [[ -n "$inner" ]] || die "tarball has no usr/bin/inputplumber"
    src="$(cd "$(dirname "$inner")/../.." && pwd)"
  fi
  log "Installing InputPlumber files from ${src}"
  mkdir -p "${R}/usr" "${R}/etc"
  cp -a "${src}/usr/." "${R}/usr/"
  if [[ -d "${src}/etc" ]]; then
    cp -a "${src}/etc/." "${R}/etc/"
  fi
  [[ -x "${R}/usr/bin/inputplumber" ]] || die "inputplumber binary missing after extract"
  # Local 0.81.0 fix prevents the output-only AYANEO rumble source spinning.
  if [[ "$IP_VER" == 0.81.0 ]]; then
    local fixed="$CACHE/inputplumber-0.81.0-pbos"
    [[ -f "$fixed" && -f "$fixed.sha256" ]] || die "missing verified AYANEO polling fix; run build-inputplumber-pbos.sh"
    (cd "$CACHE" && sha256sum -c "$(basename "$fixed").sha256") || die "patched InputPlumber checksum mismatch"
    install -m0755 "$fixed" "$R/usr/bin/inputplumber"
  fi
}

install_libiio() {
  if [[ -e "${R}/usr/lib/libiio.so.0" || -e "${R}/usr/lib/libiio.so" ]]; then
    log "libiio already in rootfs"
    return 0
  fi
  # Minimal local-backend libiio — InputPlumber links it even without IMU.
  local src="${CACHE}/libiio-src"
  if [[ ! -f "${src}/CMakeLists.txt" ]]; then
    rm -rf "$src"
    fetch "https://github.com/analogdevicesinc/libiio/archive/refs/tags/v0.26.tar.gz" \
      "${CACHE}/libiio-0.26.tar.gz"
    mkdir -p "$src"
    tar -C "$src" --strip-components=1 -xzf "${CACHE}/libiio-0.26.tar.gz"
  fi
  command -v cmake >/dev/null || die "cmake required to build libiio"
  local bld="${CACHE}/libiio-src/build-steamos"
  cmake -S "$src" -B "$bld" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DWITH_LOCAL_BACKEND=ON \
    -DWITH_XML_BACKEND=OFF \
    -DWITH_NETWORK_BACKEND=OFF \
    -DWITH_USB_BACKEND=OFF \
    -DWITH_SERIAL_BACKEND=OFF \
    -DHAVE_DNS_SD=OFF \
    -DWITH_ZSTD=OFF \
    -DWITH_EXAMPLES=OFF \
    -DWITH_TESTS=OFF \
    -DWITH_IIOD=OFF \
    -DWITH_AIO=OFF \
    -DWITH_HWMON=ON
  cmake --build "$bld" -j"$(nproc)"
  DESTDIR="$R" cmake --install "$bld"
  if [[ -e "${R}/usr/lib/aarch64-linux-gnu/libiio.so.0" && ! -e "${R}/usr/lib/libiio.so.0" ]]; then
    ln -sfn aarch64-linux-gnu/libiio.so.0 "${R}/usr/lib/libiio.so.0"
  fi
  if [[ -e "${R}/usr/lib/libiio.so" && ! -e "${R}/usr/lib/libiio.so.0" ]]; then
    ln -sfn "$(basename "$(readlink -f "${R}/usr/lib/libiio.so")")" "${R}/usr/lib/libiio.so.0"
  fi
  [[ -e "${R}/usr/lib/libiio.so.0" || -e "${R}/usr/lib64/libiio.so.0" ]] \
    || die "libiio.so.0 missing after build"
  if [[ -e "${R}/usr/lib64/libiio.so.0" && ! -e "${R}/usr/lib/libiio.so.0" ]]; then
    ln -sfn ../lib64/libiio.so.0 "${R}/usr/lib/libiio.so.0"
  fi
  log "Built local-backend libiio into rootfs"
}

install_odin_composite() {
  install -d "${R}/etc/inputplumber/devices.d" \
    "${R}/etc/inputplumber/capability_maps.d" \
    "${R}/usr/share/inputplumber/capability_maps" \
    "${R}/usr/lib/systemd/system/inputplumber.service.d" \
    "${R}/etc/systemd/system/multi-user.target.wants"
  local f
  # ayn_mcu_thor is the Thor's "Xbox positions" map (sm8550-face-buttons);
  # the RP6 and Nova use InputPlumber's own rsinput1 for that.
  # barry_deck_buttons: Barry Launcher's Deck controls on the Thor.
  for f in ayn_mcu.yaml ayn_mcu_nintendo.yaml ayn_mcu_thor.yaml rsinput_nintendo.yaml barry_deck_buttons.yaml; do
    install -m0644 "${OVL}/etc/inputplumber/capability_maps.d/${f}" \
      "${R}/etc/inputplumber/capability_maps.d/${f}"
    install -m0644 "${OVL}/etc/inputplumber/capability_maps.d/${f}" \
      "${R}/usr/share/inputplumber/capability_maps/${f}"
  done
  # One config per device, at InputPlumber's own path. InputPlumber loads
  # every file from /etc/inputplumber/devices.d and /usr/share alike (a
  # same-named /etc file only sorts first), so an /etc copy would be a
  # second config for the same device, not a replacement.
  local dev="${R}/usr/share/inputplumber/devices"
  # AYN Thor: ours replaces the stock one (Nintendo map, Deck target, AYN key
  # left to sm8550-thor-backlightd).
  [[ -f "${dev}/50-ayn_thor.yaml" ]] || die "InputPlumber ${IP_VER} has no 50-ayn_thor.yaml"
  install -m0644 "${OVL}/usr/share/inputplumber/devices/50-ayn_thor.yaml" \
    "${dev}/50-ayn_thor.yaml"
  # AYN Odin 2: the same, matched by model so the stock file's Odin 2 Portal
  # match (Xbox-printed) goes with it.
  [[ -f "${dev}/50-ayn_odin2.yaml" ]] || die "InputPlumber ${IP_VER} has no 50-ayn_odin2.yaml"
  install -m0644 "${OVL}/usr/share/inputplumber/devices/50-ayn_odin2.yaml" \
    "${dev}/50-ayn_odin2.yaml"
  # AYN Odin 2 Portal: its own config (Xbox positions), as InputPlumber has
  # none once the stock Odin 2 file is replaced.
  install -m0644 "${OVL}/usr/share/inputplumber/devices/50-ayn_odin2portal.yaml" \
    "${dev}/50-ayn_odin2portal.yaml"
  # AYN Odin 2 Mini: ours replaces the stock one (positional map like the
  # Portal's, Deck target).
  [[ -f "${dev}/50-ayn_odin2_mini.yaml" ]] || die "InputPlumber ${IP_VER} has no 50-ayn_odin2_mini.yaml"
  install -m0644 "${OVL}/usr/share/inputplumber/devices/50-ayn_odin2_mini.yaml" \
    "${dev}/50-ayn_odin2_mini.yaml"
  # Retroid Pocket Duo (SM7750): InputPlumber has none; ours uses the RP6 /
  # Nova Nintendo map and the Deck target. Untested on a Duo.
  install -m0644 "${OVL}/usr/share/inputplumber/devices/50-retroid_pocket_duo.yaml" \
    "${dev}/50-retroid_pocket_duo.yaml"
  # Retroid Pocket 6 and Nova: InputPlumber's own profiles with the Nintendo
  # face-button map (the printed A is Steam's A).
  for f in 50-retroid_pocket6.yaml 50-retroid_pocket_nova.yaml; do
    [[ -f "${dev}/${f}" ]] || die "InputPlumber ${IP_VER} has no ${f}"
    [[ "$(grep -c 'capability_map_id: rsinput1$' "${dev}/${f}")" == 2 ]] \
      || die "${f}: expected two rsinput1 sources; check the Nintendo layout override"
    sed -i 's/capability_map_id: rsinput1$/capability_map_id: rsinput_nintendo/' "${dev}/${f}"
    # One pad, started as deck-uhid like every device's (InputPlumber's own
    # files say xbox-elite); sm8550-inputplumber-ext-hid switches it to the
    # real Steam Deck controller after steamos-manager's boot reset.
    [[ "$(grep -cE '^ *- (xbox-elite|deck-uhid)$' "${dev}/${f}")" == 1 ]] \
      || die "${f}: expected one gamepad target; check the Deck target override"
    sed -i -E 's/^( *)- (xbox-elite|deck-uhid)$/\1- deck-uhid/' "${dev}/${f}"
    # The built-in accelerometer and gyroscope (pbos-motiond, through the
    # ADSP's sensor hub) as the Deck's motion sensors (InputPlumber patch 0006).
    [[ "$(grep -c '^target_devices:$' "${dev}/${f}")" == 1 ]] \
      || die "${f}: expected one target_devices list; check the motion sensor source"
    awk '/^target_devices:$/ { print "  - group: imu"; print "    evdev:";
      print "      name: PB-OS Motion Sensors"; print "      handler: event*"; print "" } { print }' \
      "${dev}/${f}" >"${dev}/${f}.new" && mv "${dev}/${f}.new" "${dev}/${f}"
  done
  # Earlier builds' second copies of these, in /etc and its overlay upper:
  # gone from the image now, and on updated devices before every
  # InputPlumber start (99-sm8550.conf runs the same script).
  install -m0755 "${OVL}/usr/lib/steamos/sm8550-inputplumber-stale-configs" \
    "${R}/usr/lib/steamos/sm8550-inputplumber-stale-configs"
  "${R}/usr/lib/steamos/sm8550-inputplumber-stale-configs" "${R}"
  install -m0644 "${OVL}/usr/lib/systemd/system/inputplumber.service.d/99-sm8550.conf" \
    "${R}/usr/lib/systemd/system/inputplumber.service.d/99-sm8550.conf"
  install -m0755 "${OVL}/usr/lib/steamos/sm8550-inputplumber-ext-hid" \
    "${R}/usr/lib/steamos/sm8550-inputplumber-ext-hid"
  install -m0644 "${OVL}/usr/lib/systemd/system/sm8550-inputplumber-ext-hid.service" \
    "${R}/usr/lib/systemd/system/sm8550-inputplumber-ext-hid.service"
  install -m0644 "${OVL}/usr/lib/udev/rules.d/71-sm8550-ext-hid.rules" \
    "${R}/usr/lib/udev/rules.d/71-sm8550-ext-hid.rules"
  mkdir -p "${R}/lib/udev/rules.d" \
    "${R}/usr/lib/systemd/system/multi-user.target.wants"
  install -m0644 "${OVL}/usr/lib/udev/rules.d/71-sm8550-ext-hid.rules" \
    "${R}/lib/udev/rules.d/71-sm8550-ext-hid.rules"
  ln -sfn /usr/lib/systemd/system/sm8550-inputplumber-ext-hid.service \
    "${R}/usr/lib/systemd/system/multi-user.target.wants/sm8550-inputplumber-ext-hid.service"
  # Face buttons: printed letters (default) or Xbox positions, chosen in
  # PB-OS Utils; the unit writes the choice back into the config at boot.
  install -m0755 "${OVL}/usr/lib/steamos/sm8550-face-buttons" \
    "${R}/usr/lib/steamos/sm8550-face-buttons"
  install -m0644 "${OVL}/usr/lib/systemd/system/sm8550-face-buttons.service" \
    "${R}/usr/lib/systemd/system/sm8550-face-buttons.service"
  ln -sfn /usr/lib/systemd/system/sm8550-face-buttons.service \
    "${R}/usr/lib/systemd/system/multi-user.target.wants/sm8550-face-buttons.service"
  # Drop Ubuntu-named composites and any leftover mouse composite from the
  # tarball.
  rm -f "${R}/etc/inputplumber/devices.d/"*mouse* \
    "${R}/etc/inputplumber/devices.d/02-ayn-controller.yaml" \
    "${R}/etc/inputplumber/devices.d/01-ayn-controller.yaml"
  ln -sfn /usr/lib/systemd/system/inputplumber.service \
    "${R}/etc/systemd/system/multi-user.target.wants/inputplumber.service"
  # The deck target is a USB/IP device on vhci_hcd, which refuses to suspend
  # (-EBUSY) while it is attached: every sleep failed and the screens came
  # back on. InputPlumber's own unit lets go of it before sleep (HookSleep)
  # and brings it back after (HookWake).
  [[ -f "${R}/usr/lib/systemd/system/inputplumber-suspend.service" ]] \
    || die "inputplumber-suspend.service missing from the InputPlumber install"
  install -d "${R}/etc/systemd/system/sleep.target.wants"
  ln -sfn /usr/lib/systemd/system/inputplumber-suspend.service \
    "${R}/etc/systemd/system/sleep.target.wants/inputplumber-suspend.service"
}

verify_needed() {
  python3 - "${R}/usr/bin/inputplumber" "${R}" <<'PY'
import pathlib, struct, sys
binary, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
data = binary.read_bytes()
if data[:4] != b"\x7fELF":
    raise SystemExit("not ELF")
needed = []
# Parse DT_NEEDED via a cheap scan of dynamic strings after readelf-less fallback
try:
    import subprocess
    out = subprocess.check_output(["readelf", "-d", str(binary)], text=True)
    for line in out.splitlines():
        if "NEEDED" in line and "[" in line:
            needed.append(line.split("[", 1)[1].split("]", 1)[0])
except Exception:
    pass
missing = []
libdirs = [root / "usr/lib", root / "usr/lib64", root / "lib"]
for soname in needed:
    if soname in {"linux-vdso.so.1", "ld-linux-aarch64.so.1"}:
        continue
    if any((d / soname).exists() for d in libdirs):
        continue
    missing.append(soname)
print("NEEDED:", ", ".join(needed) or "(unknown)")
if missing:
    raise SystemExit("missing libraries in rootfs: " + ", ".join(missing))
print("all NEEDED libs present in rootfs")
PY
}

install_tarball
install_libiio
install_odin_composite
verify_needed
log "Steam will see a real Steam Deck controller (deck target)"
log "Keyboard target for OSK haptic; USB/BT HID disables that target"
