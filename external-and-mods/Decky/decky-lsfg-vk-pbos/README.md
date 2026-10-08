# Decky LSFG-VK, PB-OS edition

The bundled Decky LSFG-VK (`../Plug-ins/homebrew/plugins/decky-lsfg-vk`) is
upstream [decky-lsfg-vk](https://github.com/xXJSONDeruloXx/decky-lsfg-vk)
v0.14.4 with `pbos-0.14.4.patch` on top. `build.sh` rebuilds it (git, node,
pnpm; on the build Mac `brew install node pnpm`).

Why: lsfg-vk 2.x draws garbage on Adreno (Turnip), so PB-OS ships lsfg-vk 1.x
for aarch64 in the image (`sm8650-overlay/usr/lib/liblsfg-vk-arm64.so`). 0.14.4
is a 2.x plugin, but its per-game profiles are what users want, so the patch
keeps its interface and puts it on the 1.x layer:

- Profiles stay in upstream's format, in `~/.config/lsfg-vk/profiles.toml`.
  Every save renders the 1.x `~/.config/lsfg-vk/conf.toml`: one `[[game]]` per
  App ID, `exe = "pbos-<appid>"` (none for multiplier 1, so the layer stays
  off). `~/.lsfg` (the launch option) sets `LSFG_PROCESS=pbos-<appid>` and the
  game's workarounds (base FPS cap and so on), as upstream did for 2.0.
- No x86 runtime or CLI: the layer is in the image, the plugin sets itself up
  when it loads and checks configs in Python. It imports an lsfg-vk 2.0
  config's profiles (kept as `conf.toml.lsfg-vk-2.0`), removes the x86 files
  2.0 left in `$HOME`, takes over the `~/.lsfg` shim of the 0.12.2-based PB-OS
  build and turns its `~/lsfg` into a shim.
- FP16 is always off (slower and stutters on Adreno). The 2.x-only present-mode
  and swapchain options and the Flatpak tab (x86-only extensions) are gone.

Upstream's unmodified v0.14.4 build reproduces the `index.js` PB-OS shipped
before byte for byte (sha256 988282e2…), so a diff in a rebuild comes from the
patch. Changes to the plugin go into the patch: edit a checkout with it
applied, `git format-patch -1 --stdout > pbos-0.14.4.patch`, then `build.sh`.
