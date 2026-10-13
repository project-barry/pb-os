#!/usr/bin/env bash
# Build KDE plasma-keyboard 0.1.0 (Qt6 touch keyboard on Qt VirtualKeyboard,
# Wayland input-method-v1) inside the Frame rootfs via bubblewrap so it
# matches its Qt/glibc. Plasma had no touch keyboard: only Steam's (Steam+X),
# which needs the Steam Deck controller layout. KWin shows this one when a
# text field is tapped once it's the Wayland virtual keyboard (kwinrc); the
# image does not make it that by default (the Steam Deck has none either),
# it is a choice in Settings > Virtual Keyboard and the Thor's Barry fallback.
# Later releases need KF6 >= 6.22; the Frame has 6.14. Maliit is Qt5-only.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
R="$(cd "${1:-${ROOT}/rootfs}" && pwd)"
VER=v0.1.0
TOP=/tmp/plasma-keyboard
SRC="$TOP/plasma-keyboard-$VER"
BUILD="$TOP/build"

command -v bwrap >/dev/null 2>&1 || { echo "ERROR: bwrap required" >&2; exit 1; }
mkdir -p "$TOP"
if [[ ! -f "$SRC/CMakeLists.txt" ]]; then
  curl -fsSL --max-time 120 -o "$TOP/src.tar.gz" \
    "https://invent.kde.org/plasma/plasma-keyboard/-/archive/${VER}/plasma-keyboard-${VER}.tar.gz"
  tar -xzf "$TOP/src.tar.gz" -C "$TOP"
fi

run_in_rootfs() {
  bwrap --bind "$R" / --bind /tmp /tmp --dev /dev --proc /proc --tmpfs /run \
    --unshare-pid --die-with-parent --chdir /tmp "$@"
}
rm -rf "$BUILD"
run_in_rootfs /usr/bin/cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
  -DBUILD_TESTING=OFF
run_in_rootfs /usr/bin/cmake --build "$BUILD" -j"$(nproc)"
run_in_rootfs /usr/bin/cmake --install "$BUILD"
ls "$R"/usr/share/applications/*keyboard*.desktop >/dev/null
echo "OK: installed plasma-keyboard ${VER} into $R"
