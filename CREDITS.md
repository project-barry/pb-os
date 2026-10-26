# Credits

**[SteamOS-ARM-Port](https://github.com/hashtagbasit/SteamOS-ARM-Port)**
(formerly SteamOS-ARM-Handhelds)
by hashtagbasit brings official SteamOS ARM to Snapdragon handhelds, starting
with the KONKR Pocket FIT (SM8650). pb-os is a fork of it. Some of that project's groundwork (the image builder
and a few scripts) came from MaSi's **SteamOS-ARM-SM8550**, so everything MaSi
credits below still applies.

## This project

| Source | URL | What we use |
|--------|-----|-------------|
| **MaSi / SteamOS-ARM-SM8550** | https://github.com/MaSieS4Fun/SteamOS-ARM-SM8550 | The whole base: image builder, SteamOS ARM overlay, Box64/Decky setup, scripts |
| **ROCKNIX SM8650** | https://github.com/ROCKNIX/distribution | Kernel recipe (20260901, written for Linux 7.2; rebased onto 7.3 in `kernel-sm8650/all/rocknix`), Pocket FIT panel/touch/MCU patches, device tree, firmware, audio UCM |
| **ROCKNIX SM8550** | https://github.com/ROCKNIX/distribution | Retroid Pocket 6 kernel (20260901 recipe, written for Linux 7.2): RP6 and Nova device trees and panels, RSInput pad and haptics drivers, AYN-signed firmware |
| **Retroid Pocket mainline kernel** | https://github.com/RetroidPocket/linux (branch `retroidpocket/v7.3`) | Their Linux 7.3 series (`external-and-mods/kernel-sm8650/retroid/`, by Teguh Sobirin and upstream authors): HTR3212 LED driver, RP6 panel, Retroid Pocket Duo (Eliza) clocks, GPU/video/CDSP nodes, WCN7750 Wi-Fi/BT, sound card, soundwire/lpass-tx-macro mic fixes, DPU command-mode fixes. GPL-2.0 |
| **linux-firmware** | https://gitlab.com/kernel-firmware/linux-firmware | Adreno 740 microcode and zap shader (SM8550) and Adreno 722 microcode (Retroid Pocket Duo), pinned by tag and SHA-256 |
| **Retroid (Moorechip)** | Retroid Pocket Nova and Retroid Pocket 6 stock Android (RPN_V1.0.0.436, RP6_V1.0.0.406) | Their own ADSP firmware and AW88166 speaker tuning (`sm8550/firmware/…/rpnova`, `…/rp6`), used instead of AYN Odin 2's |
| **Retroid Pocket alsa-ucm-conf** | https://github.com/RetroidPocket/alsa-ucm-conf (branch retroidpocket/v1.2.16, 81c2307) | Retroid Pocket Duo audio UCM (`steamos-overlay/usr/share/alsa/ucm2/RetroidPocket/ELIZA`), unchanged. BSD-3-Clause |
| **AYN** | AYN Odin 2 and Odin 2 Portal stock Android (1.0.0.387, 20250312) | Their ADSP firmware with charge separation (`sm8550/firmware/…/ayn/odin2`, `…/ayn/odin2portal`), used instead of the older ADSPs from ROCKNIX extra-firmware |
| **MaSi haptics trace fix** | `external-and-mods/kernel/patches/masi/1001-qcom-haptics-trace-7.0.patch` | Basis of the SM8550 port patch that builds the haptics driver with tracing on |
| **Armada** | https://github.com/armada-os/armada | SM8550 fan curves and fan loop (`sm8550-fand`, ported from `armada-powerd`); SM8550 GPU power-rail and s2idle kernel patches and the common AYN/Retroid DT fixes (`external-and-mods/kernel-sm8650/sm8550/`); the PCIe iommu-map fixes and the PCIe suspend-OPP fix the SM8650 kernel also carries (`external-and-mods/kernel-sm8650/patches/0006`, `0013`–`0014`; the first iommu-map fix, by Manivannan Sadhasivam, now comes with Retroid's series); gamescope bottom-screen lease patches by virtudude and JPyke3 (commits on the `dual-screen` branch of [PB-OS's gamescope fork](https://github.com/project-barry/gamescope/tree/dual-screen)); the AYN Thor's stick LED names, from `armada-rgb`'s device profiles (`sm8550-thor-controlsd`); the AYN Odin 2 sleep fixes (SN3112 LED controllers off and the panel IO rail kept on in sleep, `sm8550/patches/0550`, `sm8550/dts/qcs8550-ayn-odin2.append`) and the Odin 2 Portal's M1/M2 back buttons (`sm8550/dts/qcs8550-ayn-odin2portal.append`). GPL-2.0-or-later |
| **Armada game tweaks** | https://github.com/armada-os/armada | PB-OS Tools' **Games** tab and `pbos-game-launch` (`steamos-overlay/usr/bin/`): a port of Armada Control's per-game compatibility settings and `armada-game-launch` (FEX presets and switches through `FEX_APP_CONFIG`, CPU cores with `WINE_CPU_TOPOLOGY`, per-game environment variables, launch-option wrapping); the common-variable list (`steamos-overlay/usr/share/pb-os/game-launch/env-presets.json`, English text) and the Compatible FEX preset's settings. Copyright the Armada contributors. GPL-2.0-or-later (in PB-OS Tools under GPL-3.0-or-later, as "or later" allows) |
| **ProtonDB** | https://www.protondb.com (monthly reports dumps: https://github.com/bdefore/protondb-data; the dump each build uses is named in the data file) | Contains information from ProtonDB, which is made available here under the Open Database License (ODbL). The **ProtonDB Community Fix** section of the Performance tab's Per Game page offers the launch fix most of a game's working ProtonDB reports agree on. `steamos-overlay/usr/share/pb-os/game-launch/protondb-hints.json` (made by `tools/protondb-hints.py`) is a derived database of those reports and is published under the same license: [ODbL 1.0](https://opendatacommons.org/licenses/odbl/1-0/), individual contents [DbCL 1.0](https://opendatacommons.org/licenses/dbcl/1-0/) |
| **Motion sensors (gyro)** | https://github.com/linux-msm/hexagonrpc | `hexagonrpcd` and `sscregistrygen` (linux-msm, GPL-3.0-or-later) serve the ADSP sensors process (`external-and-mods/pbos-motion`); their patches (`pbos-motion/hexagonrpc/`) are suckbluefrog's from Batocera's Qualcomm builds, via MaSi's SteamOS-ARM-SM8550 (`external-and-mods/kernel/vendor/qcom-gyro`). The SM8550 FastRPC sensors-PD patch (`kernel-sm8650/sm8550/patches/0560`) is a 7.2.8 port (rebased for 7.3) of MaSi's patch 1025 (adapted there from suckbluefrog's), GPL-2.0; the stock Kailua sensor config (`sm8550-overlay/usr/share/sm8550-sensors/kailua`, identical in AYN's Thor and Retroid's Nova and RP6 firmware) and the Thor's `sns_tppe.so` (`…/sm8550-sensors/thor`) are from AYN's stock Thor firmware, as MaSi's tree carries them. `pbos-motiond` (own QMI/protobuf client of Qualcomm's sensor API) and InputPlumber patch 0006 are PB-OS's |
| **OpenGamingCollective gamescope** | https://github.com/OpenGamingCollective/gamescope | DRM leasing for dual-screen devices (Kyle Gospodnetich; leased-plane fix by pacoa-kdbg), the base of the AYN Thor bottom-screen support (the first commits on the fork's `dual-screen` branch) |
| **Luke Johnson (thorch)** | https://github.com/thorch-os/thorch | Root-cause work behind the SM8550 PCIe suspend-OPP and RSInput suspend fixes (via Armada) |
| **OpenABL** | https://github.com/OpenHandheldCollective/OpenABL (PB-OS fork: https://github.com/project-barry/OpenABL) | The bootloader staged on every image (`openabl/` on the card's BOOT partition, pinned in `abl/release.env` and `abl/releases.tsv`): Qualcomm's ABL with a boot menu, device model selection and Linux/Android boot. v1.2 from the PB-OS fork: upstream v1.1 plus a fix for an intermittent hang on the logo between ExitBootServices and the kernel (stack and stack canary cleaned by address before the MMU goes off; reported upstream as issue #29). GPL-3.0-or-later, Copyright (C) 2026-present Open Handheld Collective; its `LICENSE`, `LICENSES/` and `LICENSES.md` are copied next to it. Complete source: `openabl-v1.2-source.tar.xz` on the fork's [v1.2 release](https://github.com/project-barry/OpenABL/releases/tag/v1.2) |
| **ROCKNIX OpenABL scripts** | https://github.com/ROCKNIX/distribution (`projects/ROCKNIX/packages/tools/openabl`) | The backup/flash/restore scripts in `abl/` (PB-OS adds per-SoC paths and read-back checks). GPL-2.0 |
| **Armada ABL staging** | https://github.com/armada-os/armada | The per-SoC ABL folders on the card with root scripts, and the approved-hash list (`abl/releases.tsv`). The PB-OS tab's boot-menu update (`external-and-mods/pbos-qam/daemon/notices.cpp`) follows `armada-abl-update`'s design: only over a recognized older menu for the same chip, prefix-hash identification, each slot written, flushed and read back. GPL-2.0-or-later |
| **Armada Store** | https://github.com/armada-os/armada | The PB-OS tab's **App Store** (`external-and-mods/pbos-qam/daemon/store.cpp`, `sections/store.js`, `store/catalog.json`): a C++ port of the Armada Store Decky plugin's design (`decky/armada-store`): its catalog format (Flatpak and release-feed AppImage entries), newest-release lookup, Flatpak progress parsing, pending Steam shortcuts and the RetroArch aarch64 core-address fix. Copyright the Armada contributors. GPL-2.0-or-later |
| **App Store app logos** | each project, listed in `external-and-mods/pbos-qam/store/art-src/SOURCES.md` | The logos in the App Store's Steam artwork (`store/art-src/`, `store/art/`): each app's own icon from its repository or the package Flathub builds, under that project's license (Azahar's logo CC BY 4.0 by angyartanddraw and PabloMK7; Prism Launcher's CC BY-SA 4.0; ARMSX2's MIT, by fffathur). Names and logos belong to their projects and only identify each app |
| **lsfg-vk** | https://github.com/PancakeTAS/lsfg-vk | Frame generation layer: the last 1.x commit, built unmodified for aarch64 (MIT) |
| **decky-lsfg-vk** | https://github.com/xXJSONDeruloXx/decky-lsfg-vk | Frame generation Decky plugin (0.14.4 with a PB-OS patch for the lsfg-vk 1.x ARM layer) |

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
| **SteamDeckHomebrew / decky-loader** | https://github.com/SteamDeckHomebrew/decky-loader | PluginLoader, rebuilt unchanged as a native arm64 binary in [project-barry/decky-loader](https://github.com/project-barry/decky-loader) (GPL-2.0); the PB-OS tab's Quick Access tab hook and toaster follow its frontend (`tabs-hook.tsx`, `toaster.tsx`, GPL-2.0) |
| **SteamDeckHomebrew / decky-frontend-lib** | https://github.com/SteamDeckHomebrew/decky-frontend-lib | @decky/ui (LGPL-2.1), bundled in the PB-OS tab's loader (`external-and-mods/pbos-qam/frontend/dist/pbos-qam.js`) |
| **SteamGridDB / decky-steamgriddb** | https://github.com/SteamGridDB/decky-steamgriddb | Artwork plugin (GPL-3.0-or-later), 1.8.1 from the Decky store, unchanged, in the images and update packages; the Thor's bottom-screen hero art fetches from [SteamGridDB](https://www.steamgriddb.com)'s API with the user's own key |
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
| **ptitSeb / box64** | https://github.com/ptitSeb/box64 | x86_64 programs on ARM (Decky PluginLoader until it went native) |
| **Viveris Technologies / uMTP-Responder** (GPL-3.0) | https://github.com/viveris/uMTP-Responder | MTP over USB (`pbos-mtp.service`): built from the commit in `external-and-mods/uMTP-Responder/REF` plus one patch (refuses dot names while hidden files are off), with SuperSpeed descriptors and systemd notification turned on at build time |
| **Add Google Play downloads** (not shipped) | https://github.com/MindTheGapps, https://f-droid.org/packages/rkr.simplekeyboard.inputmethod/, https://adoptium.net, https://github.com/JesusFreke/smali | Fetched on the device only when the user launches Add Google Play (`google-play-fetch`, pinned SHA-256): Google's Play Store and Play services from MindTheGapps (Google's own terms), Simple Keyboard (Apache-2.0), and to rebuild Lepton's `services.jar` there, Eclipse Temurin 17 (GPL-2.0 with the Classpath Exception) and smali/baksmali (BSD-3-Clause), deleted after use |
| **thorch-os/thorch** | https://github.com/thorch-os/thorch | AYN Thor dual-screen / touch extras |
| **Tabler Icons** (MIT) | https://github.com/tabler/tabler-icons | Outline Firefox, Discord and Signal logos on Barry Launcher's home screen (`shell/Icon.qml`); the cloud outline in the Cloud theme's mark (`dashboard/skins/cloud/Cloud.qml`) |
| **Thumb-Key** (GNU AGPL 3.0) | https://github.com/dessalines/thumb-key | The letter and number layouts of Barry Launcher's optional Thumb-Key keyboard (`keyboards-extra/en-thumbkey/`, converted by `tools/thumbkey-to-barry.py`); the keys are drawn and read by Barry's own code, none of Thumb-Key's |
| **Figtree** (SIL OFL 1.1), The Figtree Project Authors | https://github.com/erikdkennedy/figtree | The SteamOS theme's keyboard font (`dashboard/skins/steamos/fonts/`, with its license) |
| **Chrome Dinosaur Game (Google)**: Sebastien Gabriel, Alan Bettes, Edward Jung | https://source.chromium.org/chromium/chromium/src/+/main:components/neterror/resources/dino_game/ | The original of Barry Launcher's Dino app (`apps/dino/main.qml`), a new QML version drawn after it; no Chromium code or sprites |

---

## Applications bundled by this overlay

| Source | URL | What we use |
|--------|-----|-------------|
| **MESA Easy Manager** | https://github.com/MaSieS4Fun/MESA-Easy-Manager | Turnip / Mesa helper |
| **Proton ARM Easy Manager** | In-tree / inspired by ProtonPlus | ARM Proton helper |
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
