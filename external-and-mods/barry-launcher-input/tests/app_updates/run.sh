#!/usr/bin/env bash
# App updates after a PB-OS update (barry_updates) and barry_apps' "minApi"
# (test_app_updates.py, a fake GitHub), and shell/AppUpdates.qml offscreen
# (tst_appupdates.qml, qmltestrunner), in a SteamOS rootfs (read-only)
# under bwrap, as root in the build VM; no network.
#
# Usage: sudo run.sh <rootfs> <sm8550-overlay> <Barry.Input module dir>
#   <Barry.Input module dir>: build.sh's output (.../qml/Barry/Input)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"; OV="$(cd "${2:?sm8550-overlay}" && pwd)"; MOD="$(cd "${3:?module dir}" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/qml/Barry" "$work/view"
cp -a "$MOD" "$work/qml/Barry/Input"
cp "$HERE/test_app_updates.py" "$work/"
cp "$HERE/tst_appupdates.qml" "$OV/usr/share/barry_launcher/shell/AppUpdates.qml" "$work/view/"
run() {
  bwrap --ro-bind "$R" / --dev /dev --proc /proc --tmpfs /tmp --tmpfs /run --bind "$work" /t \
    --ro-bind "$OV" /t/ov --unshare-net --unshare-pid --die-with-parent --chdir /t \
    --setenv PATH /usr/bin:/usr/lib/qt6/bin --setenv HOME /tmp --setenv QML_IMPORT_PATH /t/qml \
    --setenv QT_QPA_PLATFORM offscreen --setenv XDG_RUNTIME_DIR /tmp "$@"
}
run python3 -I /t/test_app_updates.py /t/ov
run qmltestrunner -input /t/view/tst_appupdates.qml
