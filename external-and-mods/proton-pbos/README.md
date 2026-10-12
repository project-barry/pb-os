# Proton PB-OS (ARM)

PB-OS twins of Valve's Proton tools for ARM64, each with one file rebuilt:
Wine's X11 driver (`files/lib/wine/aarch64-unix/winex11.so`), built from
Valve's own source (`ValveSoftware/Proton` at the tool's tag, its `wine`
submodule) in Valve's arm64 Proton SDK image, with the patch here.

| Steam tool | Valve tool (app) | Built from |
|---|---|---|
| Proton 11.0-2 PB-OS (ARM) | Proton 11.0 (ARM64) (4628740) | `proton-11.0-2e` |
| Proton Experimental PB-OS (ARM) | Proton Experimental (ARM64) (4427310) | `experimental-11.0-20261001` |

## What the patch fixes

In Game Mode, gamescope gives Steam's own window X focus whenever Quick Access
or the Steam menu opens. Wine took that as "the game lost focus" and made the
desktop the foreground window (in `focus_out()`, and again from
`_NET_ACTIVE_WINDOW` in `X11DRV_GetWindowStateUpdates()`). A game in exclusive
fullscreen then left fullscreen and minimized itself, as on Windows, and under
ARM64 Proton it came back shrunk into the top-left corner (Tomb Raider 2013,
FlatOut). The game's own window, swapchain and gamescope were all unchanged.

With the patch, when focus or the active window moves to one of Steam's windows
(WM_CLASS `steam`/`Steam`, not a game's `steam_app_*`), the game stays in the
foreground, like Steam's overlay on Windows, which never takes focus from the
game. Proton already blocks the game's input while the overlay shows. Every
other focus change behaves as before.

## On the device

The image ships only the patched drivers, `dist/<Valve version>/winex11.so`,
with the hash of Valve's own file they replace, in `/usr/share/pb-os/proton-pbos/`.
`/usr/lib/steamos/pbos-proton` (`pbos-proton.service` at login, and from
`pbos-proton.path` whenever Steam installs or updates something in
`steamapps`) builds each twin for the desktop user: a hard-linked copy of the
installed Valve tool next to it (almost no space) with the driver swapped, and
the Steam tool (manifests and a launcher) in
`~/.local/share/Steam/compatibilitytools.d/`. Only Valve's exact file gets
replaced. The launcher runs the patched copy only while Valve's installed tool
is still that version; after a Valve update it runs Valve's own, unpatched,
until PB-OS ships a patch for the new version.

PB-OS Utils follows Valve's Proton pick for each game: Valve's Proton 11.0
(ARM64) becomes Proton 11.0-2 PB-OS (ARM), Valve's Proton Experimental (ARM64)
becomes Proton Experimental PB-OS (ARM). Steam ranks the picks by number:
the global default 75, Valve's per-game pick 85, a pick in the game's
Compatibility settings 250, highest wins. So the twin is set per game, like a
user's pick. Linux ports (Valve picks the Steam Linux Runtime), games the user
set, and games whose Valve pick is just "the default" stay as they are; a game
the user changes after PB-OS switched it is left alone from then on
(`~/.config/pb-os/proton-games.json`). The global default becomes Proton
11.0-2 PB-OS (ARM) once, if it is still Valve's own (`proton-stable`) at the
first check; that check is recorded in the same file and the global default
is never changed again, so a default the user picks later stays. All of it
is event-driven: Steam start, Steam's app changes (installs, updates), Play.

## Build

    external-and-mods/proton-pbos/build.sh TAG        (TAG e.g. proton-11.0-2e)

on the arm64 build VM (Docker), with `VALVE_WINEX11=` Valve's own `winex11.so`
of that version for the hash; see its header. The unpatched build has the same
exports, imports and libraries as Valve's shipped `winex11.so`; the patch adds
one import (`XGetClassHint`, libX11).

A driver only fits the Valve version it was built from. Before every PB-OS
release, Valve's newest stable and Experimental Proton (ARM64) get checked:
when Valve has fixed this upstream, Proton PB-OS goes; until then, the patch is
rebuilt for each new Valve version.
