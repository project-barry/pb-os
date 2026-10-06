# Credits

**SteamOS-ARM-Handhelds** brings official SteamOS ARM to the KONKR Pocket FIT
(SM8650). Some of its groundwork (the image builder and a few scripts) came
from MaSi's **SteamOS-ARM-SM8550**, so everything MaSi credits below still applies.

## This project

| Source | URL | What we use |
|--------|-----|-------------|
| **MaSi / SteamOS-ARM-SM8550** | https://github.com/MaSieS4Fun/SteamOS-ARM-SM8550 | The whole base: image builder, SteamOS ARM overlay, Box64/Decky setup, scripts |
| **ROCKNIX SM8650** | https://github.com/ROCKNIX/distribution | Kernel recipe (20260801, Linux 7.1.2), Pocket FIT panel/touch/MCU patches, device tree, firmware, audio UCM |
| **ROCKNIX SM8550** | https://github.com/ROCKNIX/distribution | Retroid Pocket 6 kernel (20260901 recipe, Linux 7.2.8): RP6 and Nova device trees and panels, RSInput pad, HTR3212 LED and haptics drivers, AYN-signed firmware |
| **linux-firmware** | https://gitlab.com/kernel-firmware/linux-firmware | Adreno 740 microcode and zap shader (SM8550), pinned by tag and SHA-256 |
| **MaSi haptics trace fix** | `external-and-mods/kernel/patches/masi/1001-qcom-haptics-trace-7.0.patch` | Basis of the SM8550 port patch that builds the haptics driver with tracing on |
| **Armada** | https://github.com/armada-os/armada | SM8550 fan curves and fan loop (`sm8550-fand`, ported from `armada-powerd`); SM8550 GPU power-rail and s2idle kernel patches and the common AYN/Retroid DT fixes (`external-and-mods/kernel-sm8650/sm8550/`); the PCIe iommu-map fixes the SM8650 kernel also carries (`external-and-mods/kernel-sm8650/patches/0005`–`0006`, the first by Manivannan Sadhasivam); gamescope bottom-screen lease patches by virtudude and JPyke3 (commits on the `dual-screen` branch of [PB-OS's gamescope fork](https://github.com/project-barry/gamescope/tree/dual-screen)); the AYN Thor's stick LED names, from `armada-rgb`'s device profiles (`sm8550-thor-controlsd`). GPL-2.0-or-later |
| **OpenGamingCollective gamescope** | https://github.com/OpenGamingCollective/gamescope | DRM leasing for dual-screen devices (Kyle Gospodnetich; leased-plane fix by pacoa-kdbg), the base of the AYN Thor bottom-screen support (the first commits on the fork's `dual-screen` branch) |
| **Luke Johnson (thorch)** | https://github.com/thorch-os/thorch | Root-cause work behind the SM8550 PCIe suspend-OPP and RSInput suspend fixes (via Armada) |
| **ROCKNIX ABL** | https://github.com/ROCKNIX/abl | Bootloader with device model selection; the backup/flash/restore scripts in `abl/` (GPL-2.0) |
| **Armada ABL staging** | https://github.com/armada-os/armada | The `rocknix_abl/` folder on the card, the approved-hash list (`abl/releases.tsv`, 1.1.8 rows) and the README steps. GPL-2.0-or-later |
| **lsfg-vk** | https://github.com/PancakeTAS/lsfg-vk · https://github.com/xXJSONDeruloXx/lsfg-vk | Frame generation layer, rebuilt for aarch64 with our patches |
| **decky-lsfg-vk** | https://github.com/xXJSONDeruloXx/decky-lsfg-vk | Frame generation Decky plugin |

---

## From SteamOS-ARM-SM8550 (MaSi)

**SteamOS-ARM-SM8550** adapts official SteamOS ARM to Qualcomm SM8550
handhelds. This file lists the sources this repository is built from.

Original licenses remain with their authors. Project glue (scripts,
overlays) is **GPL-2.0** — see [`LICENSE`](LICENSE).

If a credit is missing or incorrect, please open an issue or pull request.

---

## Previous project (required credit)

This work is based on **[SteamOS-Ubuntu](https://github.com/MaSieS4Fun/SteamOS-Ubuntu)**
by the same author.

| From SteamOS-Ubuntu | Path here | Notes |
|---------------------|-----------|--------|
| **SM8550 gaming kernel** | `external-and-mods/kernel/` | Same tree as [MaSi-OS Kernel Updater](https://github.com/MaSieS4Fun/MaSi-OS-Kernel-Updater). Kernel-only detail: [`external-and-mods/kernel/CREDITS.md`](external-and-mods/kernel/CREDITS.md). |
| **Decky SM8550-Power** | `external-and-mods/Decky/sm8550/power-managment/` | Energy / power-control plugin (CPU/GPU profiles, fan, thermals). |
| **Decky SM8550-LED** | `external-and-mods/Decky/sm8550/color-leds/` | Controller RGB LED panel. |

Those two Decky plugins were adapted in SteamOS-Ubuntu from
**Hooandee**:

- Power UI: [Hooandee/panel-de-control](https://github.com/Hooandee/panel-de-control)
- LED UI: [Hooandee/decky-colores](https://github.com/Hooandee/decky-colores)

---

## Base system

| Source | URL | What we use |
|--------|-----|-------------|
| **Valve SteamOS ARM** (Frame / Deckard) | Valve | Official aarch64 userspace, Plasma, gamescope session, Steam Gamepad UI. Reconstructed at build time; **not** stored in git. |
| **Valve Steam (ARM64)** | Steam client | Game Mode client. Downloaded at image-build time; **not** stored in git. |
| **KDE Plasma / Frameworks** | https://kde.org | Official SteamOS desktop; kscreen and plasma-nm rebuilt to match SteamOS Qt/Plasma. |
| **Arch Linux ARM (Gear extras)** | https://archlinuxarm.org | Selected Plasma extras (Ark, Kate, …) built against SteamOS libraries. |

---

## Kernel and firmware

Inherited from **SteamOS-Ubuntu**. See
[`external-and-mods/kernel/CREDITS.md`](external-and-mods/kernel/CREDITS.md).

| Source | URL | What we use |
|--------|-----|-------------|
| **SteamOS-Ubuntu** | https://github.com/MaSieS4Fun/SteamOS-Ubuntu | Kernel tree, ABL `KERNEL` packaging, firmware staging |
| **MaSi-OS Kernel Updater** | https://github.com/MaSieS4Fun/MaSi-OS-Kernel-Updater | Same SM8550 kernel project |
| **Linux kernel** | https://www.kernel.org | GPL-2.0 vanilla tree |
| **Armbian** | https://github.com/armbian/build | SM8550 patch set and firmware |
| **ROCKNIX** | https://github.com/ROCKNIX/distribution · https://github.com/ROCKNIX/abl | ABL boot model, UFS `ROCKNIX`+`STORAGE`(+`HOME`), suspend patches |
| **Batocera / community DT** | Batocera, LineageOS AYN, Teguh Sobirin, Philippe Simons, thorch-os | Device trees, Thor touch, gyro firmware notes |

---

## Decky Loader and bundled plugins

| Source | URL | What we use |
|--------|-----|-------------|
| **SteamOS-Ubuntu** | https://github.com/MaSieS4Fun/SteamOS-Ubuntu | SM8550-Power and SM8550-LED as shipped here |
| **SteamDeckHomebrew / decky-loader** | https://github.com/SteamDeckHomebrew/decky-loader | PluginLoader (x86_64 via Box64) |
| **Hooandee / panel-de-control** | https://github.com/Hooandee/panel-de-control | Power-plugin design reference |
| **Hooandee / decky-colores** | https://github.com/Hooandee/decky-colores | LED-plugin design reference |
| **xXJSONDeruloXx / decky-lsfg-vk** | https://github.com/xXJSONDeruloXx/decky-lsfg-vk | LSFG Decky UI (when bundled) |

---

## Input, session, and graphics extras

| Source | URL | What we use |
|--------|-----|-------------|
| **ShadowBlip / InputPlumber** | https://github.com/ShadowBlip/InputPlumber | `deck-uhid` + keyboard target (OSK haptics) |
| **gamescope (Valve)** | https://github.com/ValveSoftware/gamescope | Gaming Mode compositor; PB-OS builds [its fork](https://github.com/project-barry/gamescope/tree/dual-screen) (MaSi's MSM port plus the dual-screen work) at `external-and-mods/gamescope/REF` |
| **Mesa / Freedreno Turnip** | https://gitlab.freedesktop.org/mesa/mesa | Adreno 740 Vulkan (host-provided `.so` at image apply) |
| **MangoHud** | https://github.com/flightlessmango/MangoHud | Performance overlay |
| **lsfg-vk** | https://github.com/PancakeTAS/lsfg-vk | Vulkan frame generation |
| **ptitSeb / box64** | https://github.com/ptitSeb/box64 | x86_64 for Decky PluginLoader |
| **thorch-os/thorch** | https://github.com/thorch-os/thorch | AYN Thor dual-screen / touch extras |
| **Tabler Icons** (MIT) | https://github.com/tabler/tabler-icons | Outline Firefox, Discord and Signal logos on Barry Launcher's home screen (`shell/Icon.qml`) |
| **Chrome Dinosaur Game (Google)**: Sebastien Gabriel, Alan Bettes, Edward Jung | https://source.chromium.org/chromium/chromium/src/+/main:components/neterror/resources/dino_game/ | The original of Barry Launcher's Dino app (`apps/dino/main.qml`), a new QML version drawn after it; no Chromium code or sprites |

---

## Applications bundled by this overlay

| Source | URL | What we use |
|--------|-----|-------------|
| **MESA Easy Manager** | https://github.com/MaSieS4Fun/MESA-Easy-Manager | Turnip / Mesa helper |
| **Proton ARM Easy Manager** | In-tree / inspired by ProtonPlus | ARM Proton helper |
| **Steam ROM Manager** | https://github.com/SteamGridDB/steam-rom-manager | Desktop launcher |
| **Easy UFS Install** | `external-and-mods/ufs-install/` (MaSi-OS UFS lineage) | Internal UFS install: ROCKNIX + STORAGE + HOME |

---

## Design inspiration

| Project | Relationship |
|---------|--------------|
| **SteamOS / Jupiter (Valve)** | Game Mode, Gamepad UI, official Plasma desktop |
| **SteamOS-Ubuntu** | Kernel, Decky SM8550 plugins, SM8550 handheld bring-up |
| **ROCKNIX** | ABL, UFS partition names, kernel patches |
| **Hooandee** | Decky power and LED plugin design |

---

## Acknowledgements

Thanks to **Valve**, the **SteamOS-Ubuntu** testers, **Hooandee**, and
maintainers of **kernel.org**, **Armbian**, **ROCKNIX**, **Batocera**,
**SteamDeckHomebrew**, **ShadowBlip**, **PancakeTAS**, **Flightless Mango**,
**ptitSeb**, **thorch-os**, and everyone who documented ABL / DTB slots
on SM8550 handhelds.
