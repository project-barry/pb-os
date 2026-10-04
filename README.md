# pb-os: SteamOS ARM for Snapdragon handhelds

> [!IMPORTANT]
> **Project Barry's additions to this fork were built with a coding agent:
> [Claude Code](https://www.anthropic.com/claude-code), running Anthropic's
> Claude Opus 5.5 (`claude-opus-5-5`).** This applies only to what Project
> Barry added on top of hashtagbasit's
> [SteamOS-ARM-Handhelds](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds):
> Claude wrote that code, its commit messages and this README, **except the
> statement right below**, which lavachemist, a human, wrote by hand. The
> upstream code, and the projects it builds on, is its own authors' work.
> People set the goals, made the decisions and did the hands-on testing.
> Review the code before you rely on it.

### A note from lavachemist (written by a human)

Project Barry is a playground for testing ideas using GenAI. Use any of this
code at your own discretion.

If you see anything that doesn't seem to credit someone, open an issue and we
will rectify that immediately. It's not intentional and would be unacceptable.
Every repo in this project should clearly state which generative AI model was
used, and it should clearly credit all sources.

We're not trying to trick anyone and we will neither defend nor deny the fact
that these repos use GenAI. That's simply the entire point of this
experimental project.

We sincerely hope that if you see an idea you like and want to implement it in
a traditional human coded project, that you'll do so. Credit us or don't, we
just want our toys to do cool shit.

lavachemist

---

pb-os runs Valve's SteamOS for ARM (the Steam Frame build) on Snapdragon
handhelds. It is a fork of hashtagbasit's
[SteamOS-ARM-Handhelds](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds).
Alpha images for the Retroid Pocket 6 and the KONKR Pocket FIT are on the
[Releases](https://github.com/project-barry/pb-os/releases) page, with
flashing steps. The AYN Thor follows as alpha v0.2. You can also build your own.

## Tested devices

Physical testing was done on one unit of each:

| Device | Chip | Tested |
|--------|------|--------|
| Retroid Pocket 6 | Snapdragon 8 Gen 2 (SM8550) | Yes, daily: from microSD, and from internal storage for several days |
| AYN Thor | Snapdragon 8 Gen 2 (SM8550) | Yes, by a remote tester, from microSD (build with `--device thor`) |
| KONKR Pocket FIT | Snapdragon 8 Gen 3 (SM8650) | Yes: from microSD, and from internal storage for several days |

Installing to internal storage (UFS, next to Android) has run for several
days on the Retroid Pocket 6 and the KONKR Pocket FIT; see
[external-and-mods/ufs-install](external-and-mods/ufs-install/README.md). On
the AYN Thor it is untested.

Here is a link to all of our benchmark tests: [docs/benchmark-results.md](docs/benchmark-results.md)

## How it differs from upstream

- **Snapdragon 8 Gen 2 (SM8550) support** for the Retroid Pocket 6 and AYN
  Thor, in one image.
- **Linux 7.2.8** on both chips, from ROCKNIX's recipes, built with GCC 15.
- **Valve's stable channel** (SteamOS 0.3.0) as the base, not the
  development branch.
- **Shared tuning:** EAS scheduling by default, no hard CPU pinning, GPU
  interrupts kept off the little cores, zstd zram, and fixes for GPU hangs,
  sleep, UFS and udisks CPU storms.
- **Tailscale** installed but switched off, with no account in the image.
- **AYN Thor extras** (only with `--device thor`): the bottom screen as a
  second display in Game Mode, the Barry Launcher (apps, keyboard,
  trackpad, performance dashboard) and a Barry Launcher Decky plugin.
  Anyone can make apps for Barry Launcher and install them from a zip:
  [barry-launcher-apps](https://github.com/project-barry/barry-launcher-apps).
  DS, 3DS and Wii U games on both screens are in progress:
  [docs/thor-dual-screen-emulators.md](docs/thor-dual-screen-emulators.md).

Each commit message explains its change.

## Credits

The full list, with what came from where, is in [CREDITS.md](CREDITS.md).

- **[hashtagbasit](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds)**:
  the upstream project this forks, including the KONKR Pocket FIT port. Support
  them on [Ko-fi](https://ko-fi.com/aimalb) or
  [PayPal](https://paypal.me/Basit2000).
- **[MaSi](https://github.com/MaSieS4Fun/SteamOS-ARM-SM8550)**: the image
  builder and SteamOS ARM overlay the project grew from.
- **[ROCKNIX](https://github.com/ROCKNIX/distribution)**: kernel recipes,
  device trees, drivers, firmware and [ROCKNIX ABL](https://github.com/ROCKNIX/abl).
- **[Armada](https://github.com/armada-os/armada)**: SM8550 sleep and GPU
  patches, fan curves, device-tree fixes and gamescope lease patches
  (virtudude).
- **[OpenGamingCollective gamescope](https://github.com/OpenGamingCollective/gamescope)**:
  DRM leasing for dual screens (Kyle Gospodnetich, pacoa-kdbg).
- **[thorch](https://github.com/thorch-os/thorch)** (Luke Johnson): AYN Thor
  suspend fixes.
- **[InputPlumber](https://github.com/ShadowBlip/InputPlumber)**,
  **[lsfg-vk](https://github.com/PancakeTAS/lsfg-vk)**,
  **[decky-lsfg-vk](https://github.com/xXJSONDeruloXx/decky-lsfg-vk)**,
  **[Decky Loader](https://github.com/SteamDeckHomebrew/decky-loader)**,
  **[MangoHud](https://github.com/flightlessmango/MangoHud)**,
  **[box64](https://github.com/ptitSeb/box64)**,
  **[linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware)** and
  **[Tabler Icons](https://github.com/tabler/tabler-icons)**.
- **Valve**, for SteamOS, Steam, gamescope and the Frame's ARM work.

## License

Scripts and overlays are GPL-2.0. Everything in `external-and-mods/` keeps its
own license. See [LICENSE](LICENSE).
