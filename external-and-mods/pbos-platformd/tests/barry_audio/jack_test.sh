#!/usr/bin/env bash
# jack_test.cpp, built and run inside the rootfs (read-only), in the VM.
# Usage: sudo jack_test.sh <rootfs>
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "${1:?rootfs}" && pwd)"
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
cp "$HERE"/jack_test.cpp "$HERE"/../../*.cpp "$HERE"/../../*.h "$HERE"/../../../pbos-qam/daemon/{loop,util,json}.{cpp,h} "$t/"
bwrap --ro-bind "$R" / --bind "$t" /build --dev /dev --proc /proc --tmpfs /tmp --chdir /build --setenv PATH /usr/bin \
  sh -c 'g++ -std=c++20 -O1 -pthread -I/usr/include/pipewire-0.3 -I/usr/include/spa-0.2 -ffunction-sections -Wl,--gc-sections \
    -o jack_test jack_test.cpp barry_audio.cpp loop.cpp util.cpp json.cpp -lsystemd -lpulse && ./jack_test'
