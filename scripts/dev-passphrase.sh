#!/usr/bin/env bash
# Make a tester's dev-updates passphrase: asks for it twice and writes the
# tester's line (name, salt, public key; never the passphrase) into
# external-and-mods/pbos-update/dev_testers, replacing that name's old line.
# The next image or update carries it; then the passphrase turns on dev
# updates (PB-OS Utils: tap the Installed line 7 times).
#
# Usage: scripts/dev-passphrase.sh <name>
#   <name>: a handle, never a real name (letters, digits, . _ -).
set -euo pipefail
name="${1:?usage: $0 <name>}"
here="$(cd "$(dirname "$0")/.." && pwd)"
file="$here/external-and-mods/pbos-update/dev_testers"
line="$(python3 "$here/external-and-mods/pbos-update/pbos-update.py" tester-line "$name")"
grep -v "^$name v1 " "$file" >"$file.new" || true
echo "$line" >>"$file.new"
mv "$file.new" "$file"
echo "added to $file:"
echo "$line"
