# pb-os: SteamOS ARM for Snapdragon handhelds

> [!IMPORTANT]
> **Project Barry's additions to this fork were built with a coding agent:
> [Claude Code](https://www.anthropic.com/claude-code), running Anthropic's
> Claude Opus 5.5 (`claude-opus-5-5`).** This applies only to what Project
> Barry added on top of hashtagbasit's
> [SteamOS-ARM-Port](https://github.com/hashtagbasit/SteamOS-ARM-Port):
> Claude wrote that code and its commit messages. **This README was written
> by lavachemist, a human, with Claude's assistance**; the statement right
> below is lavachemist's own, written by hand. The upstream code, and the
> projects it builds on, is its own authors' work. People set the goals, made the decisions and did the hands-on testing.
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

> [!TIP]
> **Join the Project Barry community on Discord:** https://discord.gg/euPurKCWc4
>
> **Watch Project Barry on YouTube:** https://www.youtube.com/@Project-Barry

pb-os runs Valve's SteamOS for ARM (the Steam Frame build) on Snapdragon
handhelds. It is a fork of hashtagbasit's
[SteamOS-ARM-Port](https://github.com/hashtagbasit/SteamOS-ARM-Port).
Alpha v0.5.4, one image for the Retroid Pocket 6, Retroid Pocket Nova, AYN
Thor and KONKR Pocket FIT, is on the
[Releases](https://github.com/project-barry/pb-os/releases) page, with
flashing steps. You can also build your own.

## Tested devices

Physical testing was done on one unit of each:

| Device | Chip | Tested |
|--------|------|--------|
| Retroid Pocket 6 | Snapdragon 8 Gen 2 (SM8550) | Yes: from microSD and internal storage |
| Retroid Pocket Nova | Snapdragon 8 Gen 2 (QCS8550) | Yes: from microSD and internal storage |
| AYN Thor | Snapdragon 8 Gen 2 (SM8550) | Yes: from microSD |
| KONKR Pocket FIT | Snapdragon 8 Gen 3 (SM8650) | Yes: from microSD and internal storage |

Installing to internal storage (UFS, next to Android) has run for several
days on the Retroid Pocket 6, the Retroid Pocket Nova and the KONKR Pocket
FIT; see [external-and-mods/ufs-install](external-and-mods/ufs-install/README.md).
On the AYN Thor it is untested.

The image also includes support for the AYN Odin 2, Odin 2 Portal, Odin 2
Mini and AYANEO Pocket S2. None of it has been tested on those devices, so
they are not officially supported.

Here is a link to all of our benchmark tests: [docs/benchmark-results.md](docs/benchmark-results.md)

## PB-OS Features

- **One image and one kernel** for every device, Snapdragon 8 Gen 2 and 8
  Gen 3.
- **Valve's stable channel** (SteamOS 0.4.5) as the base.
- **EAS scheduling by default.**
- **AYN Thor: Barry Launcher** on the bottom screen, with apps, a keyboard, a trackpad, a performance
  dashboard, and its settings in the PB-OS tab.
- **PB-OS tab in Quick Access** (Barry's head): PB-OS Tools and Barry Launcher settings with one native
  backend, instead of Decky plugins.
- Two screen Gamescope mod - enabling native DS and 3DS emulation in Game Mode on supported dual screen devices.
- **MTP over USB**, off by default: switch on **USB File Transfer** in the
  PB-OS tab and a PC can browse and copy your files over the USB-C cable,
  with nothing to install on either side and the handheld still running.

## Credits

The full list, with what came from where, is in [CREDITS.md](CREDITS.md).

- **[hashtagbasit](https://github.com/hashtagbasit/SteamOS-ARM-Port)**:
  the upstream project this forks, including the KONKR Pocket FIT port. Support
  them on [Ko-fi](https://ko-fi.com/aimalb) or
  [PayPal](https://paypal.me/Basit2000).
- **[MaSi](https://github.com/MaSieS4Fun/SteamOS-ARM-SM8550)**: the image
  builder and SteamOS ARM overlay the project grew from.
- **[ROCKNIX](https://github.com/ROCKNIX/distribution)**: kernel recipes,
  device trees, drivers and firmware.
- **[OpenABL](https://github.com/OpenHandheldCollective/OpenABL)** (Open
  Handheld Collective): the boot menu PB-OS starts from.
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
