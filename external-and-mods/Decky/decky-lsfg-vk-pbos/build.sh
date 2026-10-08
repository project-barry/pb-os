#!/usr/bin/env bash
# Rebuild the bundled Decky LSFG-VK (PB-OS edition) from upstream + pbos-0.14.4.patch.
# Needs git, node and pnpm (the build Mac: brew install node pnpm).
# Output replaces external-and-mods/Decky/Plug-ins/homebrew/plugins/decky-lsfg-vk.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${HERE}/../Plug-ins/homebrew/plugins/decky-lsfg-vk"
WORK="${1:-${TMPDIR:-/tmp}/decky-lsfg-vk-pbos}"
UPSTREAM=https://github.com/xXJSONDeruloXx/decky-lsfg-vk.git
TAG=v0.14.4

for tool in git node pnpm; do
  command -v "$tool" >/dev/null || { echo "ERROR: $tool missing" >&2; exit 1; }
done

if [[ ! -d "$WORK/.git" ]]; then
  git clone -q "$UPSTREAM" "$WORK"
fi
git -C "$WORK" checkout -q -f "$TAG"
git -C "$WORK" clean -qfdx -e node_modules
git -C "$WORK" apply "$HERE/pbos-0.14.4.patch"

(cd "$WORK" && pnpm install --frozen-lockfile && pnpm run build)

rm -rf "$DEST"
mkdir -p "$DEST/dist"
cp "$WORK/dist/index.js" "$DEST/dist/"
cp -R "$WORK/dist/assets" "$DEST/dist/"
cp -R "$WORK/py_modules" "$WORK/main.py" "$WORK/plugin.json" "$WORK/package.json" \
  "$WORK/LICENSE" "$WORK/README.md" "$DEST/"
cp -R "$WORK/defaults/." "$DEST/"
find "$DEST" -name __pycache__ -prune -exec rm -rf {} +
echo "==> $(grep -m1 '"version"' "$DEST/package.json" | tr -d ' ,') in $DEST"
