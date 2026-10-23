#!/usr/bin/env bash
# Build pbos-sound-check inside a SteamOS rootfs (as pbos-platformd/build.sh
# does): the rootfs has g++ and the SDL2 and SDL2_ttf headers.
#
# Usage: build.sh <rootfs> <output binary>
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
OUT="${2:?output binary}"

command -v bwrap >/dev/null || { echo "bwrap required" >&2; exit 1; }
[[ -x "$R/usr/bin/g++" && -f "$R/usr/include/SDL2/SDL.h" && -f "$R/usr/include/SDL2/SDL_ttf.h" ]] \
  || { echo "rootfs needs g++ and the SDL2 and SDL2_ttf headers" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$HERE"/main.cpp "$work/"
run() {
  # Read-only: the build writes nothing into the rootfs.
  bwrap --ro-bind "$R" / --bind "$work" /build --dev /dev --proc /proc --tmpfs /tmp \
    --unshare-pid --die-with-parent --chdir /build --setenv PATH /usr/bin "$@"
}
run g++ -std=c++20 -O2 -g0 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -fstack-protector-strong -Wall -Wextra -Werror -fno-plt -pthread \
  -I/usr/include/SDL2 -D_REENTRANT -o pbos-sound-check main.cpp -lSDL2 -lSDL2_ttf
run strip pbos-sound-check
install -D -m0755 "$work/pbos-sound-check" "$OUT"
echo "built $OUT"
