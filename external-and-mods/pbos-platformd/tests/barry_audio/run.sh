#!/usr/bin/env bash
# Off-device equivalence test: the Python barry_launcher_audio and
# pbos-platformd's barry-audio, against a real PipeWire, WirePlumber and
# pipewire-pulse from the rootfs: null sinks as the real outputs (with a
# device.api, as hardware has), the "Top screen" loopback config, a fake
# quick.json and a fake Barry Launcher gamescope whose children play (one
# from inside a pid namespace, as a Flatpak's). Compares the sound server's
# state after each step, the state file and the log lines. Root (bwrap
# without user namespaces, unshare); the rootfs is bound read-only.
#
# Usage: sudo run.sh <rootfs> <pbos-platformd> <worktree> <outdir>
#        (sudo SCENARIOS="idle" run.sh ... for the CPU and memory numbers)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
BIN="${2:?pbos-platformd}"
SRC="$(cd "${3:?worktree}" && pwd)"
OUT="${4:?outdir}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
t="$(mktemp -d)"; chmod 0755 "$t"
trap 'rm -rf "${t:?}"' EXIT
cp "$HERE"/inner.sh "$HERE"/snapshot.py "$t/"
cp "$BIN" "$t/pbos-platformd"
cp "$SRC/sm8550-overlay/usr/lib/barry_launcher/barry_launcher_audio" "$t/"
# Imported from /usr/lib/barry_launcher, else from beside the script.
cp "$SRC/sm8550-overlay/usr/lib/barry_launcher/barry_quick.py" "$t/"
bl=()
[[ -d "$R/usr/lib/barry_launcher" ]] && bl=(--ro-bind "$t" /usr/lib/barry_launcher)
cp "$SRC/sm8550-overlay/usr/share/pb-os/thor-audio/60-pbos-top-screen.conf" "$t/"
printf 'ayn,thor\0qcom,sm8650\0' >"$t/compat"
bw() {
  bwrap --ro-bind "$R" / --dev-bind /dev /dev --proc /proc --ro-bind /sys /sys --tmpfs /tmp \
    --tmpfs /sys/firmware/devicetree --perms 0755 --dir /sys/firmware/devicetree/base \
    --ro-bind "$t/compat" /sys/firmware/devicetree/base/compatible "${bl[@]}" \
    --bind "$t" /tmp/t --bind "$OUT" /tmp/out --unshare-pid --die-with-parent --setenv PATH /usr/bin "$@"
}
rc=0
for scen in ${SCENARIOS:-startup switch apps usr1 linkgroup saved notop restart}; do
  for impl in python cpp; do
    for f in snap log msgs run cpu; do rm -f "${OUT:?}/${scen:?}.${impl:?}.$f"; done
    bw /bin/bash /tmp/t/inner.sh "$scen" "$impl" >"$OUT/$scen.$impl.run" 2>&1 || echo "$scen $impl: runner failed"
    # Its log lines, without pbos-platformd's own nor the C++'s event lines
    # for problem reports (the loopback's output, a real output gone, unmuted).
    grep -v -E '^pbos-platformd|pbos_top_screen_out (plays on|stream gone)|gone, none in its place|was muted: unmuted' \
      "$OUT/$scen.$impl.log" >"$OUT/$scen.$impl.msgs" 2>/dev/null || true
  done
  if [[ $scen == idle ]]; then
    cat "$OUT/idle.python.cpu" "$OUT/idle.cpp.cpu"
    continue
  fi
  if diff "$OUT/$scen.python.snap" "$OUT/$scen.cpp.snap" >"$OUT/$scen.diff" &&
     diff "$OUT/$scen.python.msgs" "$OUT/$scen.cpp.msgs" >>"$OUT/$scen.diff"; then
    echo "$scen SAME ($(grep -c '^==' "$OUT/$scen.cpp.snap") steps, $(wc -l <"$OUT/$scen.cpp.msgs") log lines)"
  else
    echo "$scen DIFF (see $OUT/$scen.diff)"
    rc=1
  fi
done
exit $rc
