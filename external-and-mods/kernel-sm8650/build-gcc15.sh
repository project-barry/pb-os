#!/usr/bin/env bash
# Build the kernel (Linux 7.2.8) with GCC 15, as the Retroid Pocket 6
# image runs it. The same recipe built with Ubuntu 24.04's GCC 13 dies before
# the initramfs on the RP6 (52.75 MiB image vs 47.1 MiB with GCC 15; an ABL
# load-size limit near 52 MiB is suspected but not proven), so 7.2 builds
# go through a Fedora 43 container. Everything else is ./build.sh unchanged.
#
# Usage (aarch64 host with Docker; paths are the build VM's):
#   SOC=sm8550 WORK=/work/kernel-sm8550-72 ROCKNIX_DIR=/work/rocknix-20260901 \
#     external-and-mods/kernel-sm8650/build-gcc15.sh
# The port tree, WORK and ROCKNIX_DIR must sit under one directory that is
# mounted at the same path inside the container (MOUNT, default /work).
# The initramfs step needs a static busybox; the host's is bind-mounted.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${IMAGE:-fedora:43}"
MOUNT="${MOUNT:-/work}"
BUSYBOX="${BUSYBOX:-/bin/busybox}"
SOC="${SOC:-sm8550}"
: "${WORK:?set WORK (kernel work dir, under ${MOUNT})}"
: "${ROCKNIX_DIR:?set ROCKNIX_DIR (ROCKNIX checkout, under ${MOUNT})}"
for p in "$HERE" "$WORK" "$ROCKNIX_DIR"; do
  [[ "$p" == "$MOUNT"/* ]] || { echo "$p is not under $MOUNT" >&2; exit 1; }
done
[[ -x "$BUSYBOX" ]] || { echo "no busybox at $BUSYBOX" >&2; exit 1; }

PKGS="file gcc make bc bison flex python3 curl tar xz gzip cpio kmod patch perl rsync
      openssl-devel elfutils-libelf-devel dwarves diffutils findutils hostname which git"
exec docker run --rm \
  -v "$MOUNT:$MOUNT" -v "$BUSYBOX:/bin/busybox:ro" \
  -e SOC="$SOC" -e WORK="$WORK" -e ROCKNIX_DIR="$ROCKNIX_DIR" \
  -e KERNEL_BUILD_IN_CONTAINER=1 \
  "$IMAGE" bash -c "
    set -e
    dnf -q -y install $(echo $PKGS) >/dev/null
    git config --global --add safe.directory '*'
    gcc --version | head -1
    cd '$HERE' && ./build.sh"
