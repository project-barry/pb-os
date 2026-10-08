# Decky LSFG-VK

> [!IMPORTANT]
> **PB-OS edition (0.14.4-pbos.1).** Upstream decky-lsfg-vk 0.14.4, changed for
> PB-OS on ARM with Claude Code (Claude Opus 5.5); only these changes are PB-OS's
> (source: `external-and-mods/Decky/decky-lsfg-vk-pbos/` in pb-os):
> - It drives lsfg-vk **1.x** for aarch64, which ships in the image
>   (`/usr/lib/liblsfg-vk-arm64.so`): lsfg-vk 2.x draws garbage on Adreno (Turnip).
>   Per-game profiles stay; they are kept in `~/.config/lsfg-vk/profiles.toml` and
>   rendered into the 1.x `conf.toml`, and `~/.lsfg` picks each game's entry with
>   `LSFG_PROCESS=pbos-<appid>`.
> - It sets itself up when it loads (no download), imports the per-game profiles
>   of an lsfg-vk 2.0 config (kept as `conf.toml.lsfg-vk-2.0`), and clears the x86
>   files lsfg-vk 2.0 left in `$HOME`.
> - FP16 is always off (slower and stutters on Adreno). Present-mode and swapchain
>   options are gone (lsfg-vk 1.x has none). No Flatpak support: upstream's
>   extensions are x86-only.
> - Lossless Scaling stays on its `lsfg-vk` Steam branch; 1.x reads its `Lossless.dll`.

> This is an **unofficial community plugin**. It is independently developed and **not officially supported** by the creators of Lossless Scaling or lsfg-vk. For support, please use the [decky-lsfg-vk Discord Channel](https://discord.gg/TwvHdVucC3).


<p align="center">
   <img src="assets/decky-lossless-logo.png" alt="decky-lsfg-vk Logo" width="200"/>
</p>
<p align="center">
   <a href="https://ko-fi.com/B0B71HZTAX" target="_blank" rel="noopener noreferrer">
      <img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support on Ko-fi"/>
   </a>
</p>


Decky LSFG-VK is a Decky plugin that streamlines the installation of **lsfg-vk** ([Lossless Scaling Frame Generation Vulkan layer](https://lsfg-vk.dev/)) on Steam Deck, allowing you to use the Lossless Scaling frame generation features on Linux with a controller friendly UI in SteamOS, Bazzite, or any other Linux platform compatible with Decky Loader.

## Installation

1. **Download the plugin** from the [releases tab](https://github.com/xXJSONDeruloXx/decky-lsfg-vk/releases)
   - Download the "Decky LSFG-VK.zip" file to your Steam Deck
2. **Install manually through Decky**:
   - In Game Mode, go to the settings cog in the top right of the Decky Loader tab
   - Enable "Developer Mode"
   - Go to "Developer" tab and select "Install Plugin from Zip"
   - Select the downloaded "Decky LSFG-VK.zip" file

## How to Use

1. **Purchase and install** [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) from Steam
2. **Switch Branch** In the Lossless Scaling Steam app, go to Properties > Game Versions & Betas > select "lsfg-vk" branch, and let Steam download the new version
2. **Open the plugin** from the Decky menu
3. **Click "Install lsfg-vk"** to automatically set up the lsfg-vk vulkan layer
4. **Configure settings** using the plugin's UI - select Default or a running/configured game and adjust the upstream lsfg-vk settings
5. **Configure Flatpak apps** in the Flatpak tab individually or with **Enable all** and **Remove all profiles**
6. **Launch your game** - frame generation activates when the game's Steam AppID matches an assigned upstream profile

### Core Settings

- **FPS Multiplier**: Choose 2x, 3x, or 4x frame generation
- **Flow Scale**: Adjust motion estimation quality (lower = better performance, higher = better quality)
- **Performance Mode**: Uses a lighter processing model (recommended for most games)
- **FP16 Acceleration**: Use half-precision acceleration when supported

## Feedback and Support

For per-game feedback and community support, please join the [decky-lsfg-vk Discord Channel](https://discord.gg/TwvHdVucC3)

## Credits

- **[PancakeTAS](https://lsfg-vk.dev/)** for creating the lsfg-vk Vulkan compatibility layer
- **[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/)** developers for the original frame generation technology
- **[Deck Wizard](https://www.youtube.com/@DeckWizard)**  - Extensive community support including comprehensive guides, promotional content, thorough testing and feedback, custom artworks, and tutorial videos. His passionate advocacy and continuous support have been instrumental in this plugin's success.
- The **Decky Loader** team for the plugin framework
- Community contributors and testers for feedback and bug reports
