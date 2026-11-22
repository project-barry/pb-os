#!/usr/bin/env bash
# Build barry_launcher_keyd (when Barry Launcher's keyboards come up) inside
# a SteamOS rootfs, as barry-launcher-imd's build.sh does: the rootfs has
# g++, pkg-config and the AT-SPI, GLib, GUdev and JSON-GLib headers.
#
# Usage: build.sh <rootfs> <output binary>
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
OUT="${2:?output binary}"

command -v bwrap >/dev/null || { echo "bwrap required" >&2; exit 1; }
[[ -x "$R/usr/bin/g++" && -x "$R/usr/bin/pkg-config" && -d "$R/usr/include/at-spi-2.0" \
   && -d "$R/usr/include/gudev-1.0" && -d "$R/usr/include/json-glib-1.0" ]] \
  || { echo "rootfs needs g++, pkg-config and the AT-SPI, GUdev and JSON-GLib headers" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# bottompanel.h: where a touch is on the bottom screen, shared with Barry.Input.
cp "$HERE/barry_launcher_keyd.cpp" "$HERE/../barry-launcher-input/bottompanel.h" "$work/"
bwrap --bind "$R" / --bind "$work" /build --dev /dev --proc /proc --tmpfs /tmp \
  --unshare-pid --die-with-parent --chdir /build --setenv PATH /usr/bin \
  sh -c 'g++ -std=c++17 -O2 -Wall -Wextra -Werror -o barry_launcher_keyd barry_launcher_keyd.cpp \
    $(pkg-config --cflags --libs atspi-2 gio-unix-2.0 gudev-1.0 json-glib-1.0)'
install -D -m0755 "$work/barry_launcher_keyd" "$OUT"
echo "built $OUT"
