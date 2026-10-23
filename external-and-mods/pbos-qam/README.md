# PB-OS Quick Access (pbos-qam)

PB-OS's own tab in Steam's Quick Access menu. Its icon is Barry's head. It
replaces the PB-OS Tools and Barry Launcher Decky plugins. The tab opens on
one list of pages, in the order the PB-OS team laid out (`PAGES` in
`frontend/src/loader.js`); each section adds its parts to a page
(`plugin({ page, order, content })`). Each device lists only the pages it
has something for:

- Display (AYN Thor): Dual Screen, with Bottom Screen Art; Emulators.
- Personalization: Lighting (the Thor's stick-light dimmer, or the device's
  lighting); Themes and Widgets (Thor).
- Apps (Thor): Apps, Companion Apps.
- Performance: a default mode and per-game settings.
- Hardware, Power: where the device has them.
- System: Software Update, Keyboard Languages (Thor), Move SteamOS, Report a Problem.
- App Store, an entry of its own after the pages: emulators and apps to
  install (see below).

Selecting an entry opens it. B or the back arrow returns to the list, and
LB/RB step through the entries.

## Why not Decky

Decky's PluginLoader used to be an x86-64 program running under box64 on
these ARM devices; it is native arm64 now (Project Barry's fork,
project-barry/decky-loader), but each Decky plugin's Python backend is still
a process of its own. pbos-qamd is one native C++ process, the backend for
every section. Decky Loader still ships, for decky-lsfg-vk and
the user's own plugins, but PB-OS's settings no longer depend on it.

## Parts

| Path | Installed as | What |
|---|---|---|
| `daemon/` | `/usr/lib/pbos-qam/pbos-qamd` | The daemon: puts the tab into Steam and is every section's backend. C++20, built in the rootfs by `build.sh`. Links libc, libstdc++ and libsystemd; libarchive is loaded only to unpack a theme or language pack. |
| `daemon/tools.cpp` | | The PB-OS Tools backend (was `Decky/pbos-utils/main.py`). |
| `daemon/barry.cpp` | | The Barry Launcher backend (was `Decky/sm8550/dual-screen/main.py`). |
| `daemon/lights.cpp` | | The stick light engine (RP6, Nova, Odin 2s; was `pbos_lights.py`): `pbos-qamd --lights`, its own unit `pbos-utils-lights`, which the tools section starts. Effect frames are its only timer; LED devices coming back after sleep are heard as uevents, the charging indicator with inotify. |
| `daemon/store.cpp`, `sections/store.js` | `/var/lib/pbos-qam/store.json` | The App Store: installs and uninstalls what `store/catalog.json` lists (`/usr/share/pbos-qam/store/catalog.json`) and adds it to Steam. |
| `daemon/notices.cpp`, `sections/notices.js` | `/home/.pbos-abl-backup/` (boot menu backups) | A message once Steam starts, no page in the tab: on a device with an older boot menu PB-OS knows (OpenABL 1.0/1.1, ROCKNIX ABL; both `abl` slots hashed as PB-OS Installer does) it offers to install the shipped OpenABL (`/usr/share/pb-os/openabl`) from Game Mode: both slots backed up to `/home/.pbos-abl-backup`, then written one at a time with read-back. At every boot and Steam restart until done; never on the current release or an unidentified boot loader. |
| `daemon/report.cpp`, `sections/report.js` | `~/Desktop/PB-OS Reports/` | System > Report a Problem: the user picks what went wrong (sound, sleep, controls, screen, Wi-Fi, something else) and saves a report there and then, from Game Mode. `/usr/bin/pbos-report AREA` (run as root, read-only) writes the version, this boot's sleep and wake times, warnings, and that part's state and logs; the file is written as the user. A line in the journal marks the moment. Nothing is sent anywhere. |
| `daemon/files.cpp` | | Folder listings for the tab's file picker (home and removable drives only). |
| `daemon/heroart.cpp` | `/run/pbos-qam/steam-game.json` | AYN Thor: the game selected in Steam and its hero and logo images (the user's own art, Steam's, or SteamGridDB's with the user's own API key), for the bottom screen while Barry Launcher is closed. |
| `daemon/barrylink.cpp` | `/run/pbos-qam/barry.sock` | AYN Thor: Barry Launcher's quick settings page calls a few methods here (brightness, charging, game profiles). Only the desktop user and root are answered. |
| `frontend/` | `/usr/share/pbos-qam/pbos-qam.js` | The loader: the tab, the list, toasts, the file picker and the check. Bundles @decky/ui. `frontend/build.sh` builds `dist/pbos-qam.js` on the build Mac, and the result is committed. |
| `sections/*.js` | `/usr/share/pbos-qam/sections/` | Each section's pages: the Decky plugins' tab components, unchanged. |
| `../../steamos-overlay/usr/lib/systemd/system/pbos-qamd.service` | | Runs it, as root, on every device. |

`scripts/install-pbos-qam.sh` installs everything into an image. Update
packages remove the old `pbos-utils` and `dual-screen` Decky plugins
(`build-update-package.py` REMOVE, `pbos-update.py` LEGACY_PLUGINS), and
pbos-qamd also removes them at start if they are there.

## App Store

PB-OS ships no emulators. The App Store lists some (and a few apps) in
`store/catalog.json`, and downloads the one the user picks:

- `flatpak`: from Flathub, system-wide, then given the user's home and
  removable drives (`flatpak override --system`).
- `appimage`: the newest release asset matching `asset` (a regular
  expression) in a GitHub or GitLab releases feed (`releases`), into
  `~/Applications` as `filename`, with a desktop entry for Desktop Mode. A
  prerelease only when the project publishes nothing else.

After an install, the frontend adds the app to Steam's library
(`SteamClient.Apps.AddShortcut`); apps marked `desktopOnly` get no shortcut.
`postInstall: "retroarch-cores-url"` gives Flathub's RetroArch the aarch64
core download address its build lacks, while the user's own setting is empty.

Every file in the user's home (downloads, desktop entries, RetroArch's
config) is written by a program run as the user (`setpriv`), never by the
root daemon itself. An app already installed some other way (EmuDeck's
Flatpaks in the user's installation, an AppImage of the same name the App
Store didn't put there) shows as installed and is never updated or removed
here. Updates are looked up when the page opens, at most hourly.

Only entries with an aarch64 build belong in the catalog. The DS and 3DS
emulators are Flathub's stock melonDS and Azahar: on the AYN Thor, gamescope
puts their second window on the bottom screen
([docs/thor-dual-screen-emulators.md](../../docs/thor-dual-screen-emulators.md)).

The App Store follows Armada's Armada Store (armada-os/armada,
`decky/armada-store`, GPL-2.0-or-later): its catalog format, release
lookup, Flatpak progress and shortcut handling.

```
sudo pbos-qamd --call store get_status
sudo pbos-qamd --call store install_now '["ppsspp"]'
sudo pbos-qamd --call store uninstall_now '["ppsspp"]'
```

## How it talks to Steam

- Steam runs with `-cef-enable-debugging`, so its browser has a DevTools
  port at 127.0.0.1:8080 (Decky uses the same port). pbos-qamd connects to
  the Game Mode page and evaluates the loader in it, followed by the
  sections' scripts.
- The frontend calls the backend through a DevTools binding:
  `window.__pbosQamCall(json)` reaches pbos-qamd as `Runtime.bindingCalled`,
  and the answer goes back with `Runtime.evaluate`. pbos-qamd opens no port
  of its own. On the AYN Thor it has a local socket for Barry Launcher
  (`barrylink.cpp`), not a network port.
- Calls are positional, as Decky's `callable()` was. Each call runs on a
  thread of its own, so a slow helper (an update check, sizing the home
  folder) never holds up the others.
- Backend events (update available, profile changed, move done) go to the
  frontend's listeners and show as toasts. With Decky installed they use
  Decky's toaster (see STEAM-INTERNALS.md).

## Event-driven

Nothing in pbos-qamd polls, except where noted below.

| What | Heard from |
|---|---|
| Steam starting | inotify on Steam's web helper log and `~/.steam/steam.pid`, watched only while not connected. Then a short backoff until the page is there. |
| Steam going away | the DevTools socket closing |
| A drive put in or taken out | kernel uevents (block) |
| An update file in Downloads | inotify on `~/Downloads` |
| A move ending (SD ↔ internal) | systemd over D-Bus (the unit's PropertiesChanged) |
| The Performance button (pbosd) | inotify on pbosd's state file |
| Emulator configs (two-screen) | inotify on their folders, and pidfd when an emulator still running has to quit first |
| The game selected in Steam (Thor hero art) | `focusin` in Steam's windows and games starting; Barry Launcher reads the result with inotify |

The Decky plugins polled the drives every 3 s, the move every 2 s, pbosd's
state every 0.25 s and the emulators every 30 s. The update check still
runs every 6 hours, by design (it asks GitHub). The section pages
themselves poll while they are open, as the plugins did: for example,
Barry's keyboard availability every 2 s and the running games every 1 s.

## Testing without Steam

```
sudo pbos-qamd --call tools get_state
sudo pbos-qamd --call barry get_languages
sudo pbos-qamd --call files list '["/home/steamos/Downloads"]'
sudo pbos-qamd --call report make '["audio"]'
```

`--call SECTION METHOD [ARGS]` runs one backend method and prints its
answer. ARGS is the JSON list the frontend would send.

In Steam's page (CEF DevTools, port 8080), the loaded tab also has
`window.__pbosQam.plugins()` (the list's keys), `.open("page/system")` (shows a page),
`.toast({title, body})` and `.status()` (patched, drawn, sections, errors),
for tests.

`sudo pbos-qamd --check` checks Steam's internals in the running Steam.
Run it before every release, see [STEAM-INTERNALS.md](STEAM-INTERNALS.md).

## Credits and licenses

The daemon and the loader are GPL-2.0, the license of this repository's own
software. The loader's tab hook and toaster follow Decky Loader's frontend
(SteamDeckHomebrew/decky-loader, GPL-2.0). The App Store follows Armada's Armada Store
(armada-os/armada, GPL-2.0-or-later). @decky/ui is
SteamDeckHomebrew/decky-frontend-lib (LGPL-2.1), bundled in
`frontend/dist/pbos-qam.js`. The section scripts carry the Decky plugins'
components, which were GPL-3.0-or-later.
