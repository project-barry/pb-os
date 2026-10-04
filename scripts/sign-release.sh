#!/usr/bin/env bash
# Sign a release's SHA256SUMS with the pb-os release key. Devices download an
# update only when SHA256SUMS.sig verifies against
# external-and-mods/konkr-update/allowed_signers (shipped in every image).
#
# Usage: scripts/sign-release.sh <folder with SHA256SUMS> [private key]
#   default key: ~/.ssh/pb-os-release; asks for its passphrase.
# Then upload SHA256SUMS and SHA256SUMS.sig with the release files.
set -euo pipefail
dir="${1:?folder with SHA256SUMS}"
key="${2:-$HOME/.ssh/pb-os-release}"
here="$(cd "$(dirname "$0")/.." && pwd)"
[[ -f "$dir/SHA256SUMS" ]] || { echo "no $dir/SHA256SUMS" >&2; exit 1; }
rm -f "$dir/SHA256SUMS.sig"
ssh-keygen -Y sign -f "$key" -n pb-os-update "$dir/SHA256SUMS"
ssh-keygen -Y verify -f "$here/external-and-mods/konkr-update/allowed_signers" \
  -I pb-os-release -n pb-os-update -s "$dir/SHA256SUMS.sig" <"$dir/SHA256SUMS"
echo "signed: $dir/SHA256SUMS.sig"
