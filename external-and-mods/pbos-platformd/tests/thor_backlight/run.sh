#!/usr/bin/env bash
# Off-device equivalence test of the thor-backlight module against the
# Python sm8550-thor-backlightd (see scenario.py; the maths alone: maths.sh).
# Root in the build VM. Each run gets its own mount namespace with an
# overlay over the rootfs (the rootfs itself is never written; the overlay
# carries the runuser stub and Barry's desktop switch), a fake
# /sys/class/backlight, /run, /var/lib and /home, and real uinput devices
# (gpio-keys-ayn with KEY_F24, bottom_touchscreen). The daemons bind-mount
# for real, inside that namespace. Not --unshare-pid: the kernel's process
# events are for the first pid namespace only.
#
# No power key is made, but as the other uinput tests do, the whole run
# holds a logind inhibitor for the power and suspend keys.
#
# Usage: sudo run.sh <rootfs> <pbos-platformd> <sm8550-thor-backlightd> <outdir> [CASE...]
set -euo pipefail
if [[ "${PB_TEST_INHIBITED:-}" != 1 ]]; then
  command -v systemd-inhibit >/dev/null || { echo "systemd-inhibit required" >&2; exit 1; }
  exec env PB_TEST_INHIBITED=1 systemd-inhibit --what=handle-power-key:handle-suspend-key --mode=block \
    --who=pbos-platformd-test --why="fake input devices" bash "$0" "$@"
fi
systemd-inhibit --list --no-pager 2>/dev/null | grep -q pbos-platformd-test \
  || { echo "no inhibitor held; refusing" >&2; exit 1; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
BIN="${2:?pbos-platformd}"
PY="${3:?sm8550-thor-backlightd}"
OUT="${4:?outdir}"
shift 4
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
b="$(mktemp -d -p /var/tmp)"
trap 'rm -rf "$b"' EXIT
cp "$HERE"/{scenario.py,preload.c,fakeproc.c} "$b/"
cp "$BIN" "$b/pbos-platformd"
cp "$PY" "$b/sm8550-thor-backlightd"
bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --bind "$b" /tmp/b --chdir /tmp/b --setenv PATH /usr/bin \
  sh -c 'gcc -O2 -Wall -shared -fPIC -o preload.so preload.c -ldl && gcc -O2 -o fakeproc fakeproc.c'
cp "$b/fakeproc" "$b/gamescope"
mv "$b/fakeproc" "$b/kwin_wayland"
printf 'ayn,thor\0qcom,sm8550\0' >"$b/compat"
printf 'retroidpocket,rpduo\0qcom,eliza\0' >"$b/compat-duo"
modprobe uinput 2>/dev/null || true
cases=("$@")
[[ ${#cases[@]} -gt 0 ]] || read -ra cases <<<"$(python3 "$b/scenario.py" --list)"
for c in "${cases[@]}"; do
  # duo-*: the Retroid Pocket Duo's profile, the module alone (no Python).
  impls=(py cpp)
  compat=compat
  if [[ "$c" == duo-* ]]; then
    impls=(cpp)
    compat=compat-duo
  fi
  for impl in "${impls[@]}"; do
    t="$(mktemp -d -p /var/tmp)"
    mkdir -p "$t"/{upper/usr/bin,upper/usr/lib/barry_launcher,work,root,bl,run,varlib,home,ctl}
    cp "$HERE/runuser" "$t/upper/usr/bin/runuser"
    printf '#!/bin/sh\necho "desktop-switch $*" >>/tmp/ctl/cmds\n' >"$t/upper/usr/lib/barry_launcher/barry_launcher_desktop_switch"
    chmod 0755 "$t/upper/usr/bin/runuser" "$t/upper/usr/lib/barry_launcher/barry_launcher_desktop_switch"
    echo "== $c ($impl)"
    unshare -m --propagation private bash -c '
      set -e
      mount -t overlay overlay -o "lowerdir=$1,upperdir=$2/upper,workdir=$2/work" "$2/root"
      exec bwrap --bind "$2/root" / --dev-bind /dev /dev --proc /proc --ro-bind /sys /sys \
        --bind /sys/devices/virtual/input /sys/devices/virtual/input \
        --tmpfs /sys/firmware --dir /sys/firmware/devicetree/base \
        --ro-bind "$3/$7" /sys/firmware/devicetree/base/compatible \
        --bind "$2/bl" /sys/class/backlight --bind "$2/run" /run --bind "$2/varlib" /var/lib --bind "$2/home" /home \
        --tmpfs /tmp --bind "$2/bl" /tmp/bl --bind "$2/ctl" /tmp/ctl --bind "$3" /tmp/t --bind "$4" /tmp/out \
        --cap-add ALL --die-with-parent --chdir /tmp/t --setenv PATH /usr/bin \
        python3 /tmp/t/scenario.py "$5" "$6"' _ "$R" "$t" "$b" "$OUT" "$impl" "$c" "$compat" \
      || echo "!! $c ($impl) failed"
    [[ -n "${KEEP:-}" ]] && echo "kept $t" || rm -rf "$t"
  done
done
python3 "$HERE/compare.py" "$OUT" "${cases[@]}"
