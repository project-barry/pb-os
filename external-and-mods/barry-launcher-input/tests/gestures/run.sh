#!/usr/bin/env bash
# The Launcher's touch rules (GESTURES.md): PanZoomGesture's frames
# (pan_zoom_test.cpp, no Qt), the bottom touchscreen's geometry and swipe up
# (bottom_panel_test.cpp, no Qt) and TouchScroll / PanZoomArea under Qt's touch
# events (touch_surfaces_test.cpp, offscreen), built and run in a SteamOS
# rootfs (read-only) under bwrap.
#
# Usage: run.sh <rootfs> <work dir>
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
W="${2:?work dir}"

rm -rf "$W"
mkdir -p "$W/src"
run() {
  bwrap --ro-bind "$R" / --bind "$W" /t --dev /dev --proc /proc --tmpfs /tmp \
    --unshare-pid --unshare-net --die-with-parent --chdir /t --setenv PATH /usr/bin --setenv HOME /tmp \
    --setenv XDG_RUNTIME_DIR /tmp --setenv QT_QPA_PLATFORM offscreen --setenv QT_QUICK_BACKEND software "$@"
}

echo "== build"
cp -r "$SRC"/. "$W/src/"
run cmake -S /t/src -B /t/out -G Ninja -DCMAKE_BUILD_TYPE=Release -DBARRY_INPUT_GESTURE_TEST=ON >/dev/null
run cmake --build /t/out --target pan-zoom-test bottom-panel-test touch-surfaces-test >/dev/null

status=0
echo "== pan-zoom-test"
run /t/out/pan-zoom-test || status=1
echo "== bottom-panel-test"
run /t/out/bottom-panel-test || status=1
echo "== touch-surfaces-test"
run /t/out/touch-surfaces-test || status=1
exit $status
