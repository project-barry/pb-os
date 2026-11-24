# pbos-platformd

PB-OS's platform daemons as modules of one event-driven C++ program
(they were a Python process each). Each unit runs its own module, under
its old unit name, so everything that starts, reloads or signals a unit by
name keeps working.

| Module | Unit | Was | Listens to |
|---|---|---|---|
| `fan` | `sm8550-fand.service` | `sm8550-fand` | a 3 s tick (thermal zones can't notify); SIGHUP reloads the profile |
| `power-button` | `sm8550-powerbuttond.service` | `sm8550-powerbuttond` | the key (evdev), input uevents, the charge-rest flag (inotify, pidfd) |
| `charge-control` | `sm8550-charge-control.service` | the daemon of `sm8550-charge-control` (the script stays as the settings CLI) | power_supply uevents, logind's wake signal, its settings file (inotify), SIGHUP |
| `volume-keeper` | `sm8550-volume-keeper.service` (user) | `sm8550-volume-keeper` | the sound server's events (libpulse) |
| `pbosd` | `pbosd.service` | `/usr/lib/konkr/pbosd` | input, power_supply and USB uevents, the /run flags (inotify, pidfd), process events (game boost), the webhelper (inotify); a 1 s fan tick; SIGHUP |
| `konkr-volume` | `konkr-volume.service` (user) | `konkr-volume` | the sound server's events, ALSA control events (card 0), the PipeWire node's params |
| `volume-keys` | `sm8550-volume-keys.service` (user) | `sm8550-volume-keys` | the keys (evdev), input uevents, `/dev/input` permissions (inotify) |
| `thor-backlight` | `sm8550-thor-backlightd.service` | `sm8550-thor-backlightd` | Steam's file, the /run/sm8550-thor flags, STATE/SCREENS, Barry's settings, charge-rest flag (inotify, pidfd); AYN key + touch (evdev); input/backlight/DRM uevents; process events (Desktop Mode); one-shot timers; 0.5 s re-check only while the bottom screen is dark |
| `thor-controls` | `sm8550-thor-controlsd.service` | `sm8550-thor-controlsd` | its socket (/run/sm8550-thor/controls.sock) |
| `thor-desktop-brightness` | `sm8550-thor-desktop-brightness.service` (user) | `sm8550-thor-desktop-brightness` | the session bus (Plasma's ScreenBrightness), the panel behind its hardware slider (inotify) |
| `audio-dp` | `sm8550-audio-pipewire.service` (user; the script applies the sink policy once, then execs this) | the 2 s loop of `sm8550-audio-pipewire --watch` | DRM uevents (HDMI/DP hotplug); runs the script again on a flip |
| `barry-audio` | `barry_launcher_audio.service` (user) | `barry_launcher_audio` | the sound server's events (libpulse); SIGUSR1 (apps volume changed) |
| `memguard` | `pbos-memguard.service` (every device) | new | two PSI triggers on `/proc/pressure/memory` (POLLPRI); one-shot timers only after it acted |

## memguard: what goes first when memory runs out

Like Apple's jetsam: background work goes before the game, gamescope, Steam
and the sound server. systemd-oomd stays off (it kills a whole cgroup, the
game's with it). Two layers:

**OOM order** (the kernel's last resort; drop-ins `NN-pbos-oom.conf` in
`sm8650-overlay/usr/lib/systemd/system/*.d`, every image):

| OOMScoreAdjust | Units |
|---|---|
| 1000 | `gpu-trace`, `pbos-flatpak-appstream`, `steamos-log-submitter`, `flatpak-system-helper`; Decky's loader process (set by memguard) |
| 999 | `plugin_loader` (Decky; its plugin backends inherit it) |
| 900 | the game (set by the Steam client) |
| 300 | `tailscaled` (remote access: after the game) |
| 200 | the user session (user@.service): Steam, steamwebhelper |
| 0 | other system units |
| -500 | `pbos-*`, `sm8550-*`, `pbosd`, `pocket-s2-controller`, `inputplumber`; gamescope (`gamescope-session`, Thor's `sm8550-bottom-session`), `pipewire`, `pipewire-pulse`, `wireplumber` (set by memguard) |

User units cannot go below the user manager's 100, so memguard (root) sets
the compositor and sound server processes, at start and on its early
trigger. The Steam client starts every game at 900 (Retroid Pocket Nova,
10-19), so the expendable units sit above it.
Decky does not restart a plugin backend that dies (and spins on it), so
memguard puts the loader itself above its backends: the kernel takes the
loader, and systemd restarts all of Decky (Restart=always).

**Acting early** (before zram thrashing makes the game stutter):

- early trigger `some 150000 2000000` (7.5% of 2 s stalled): the pins above;
  pushes a recovery back.
- act trigger `some 500000 2000000` (25%): in two windows in a row (about
  4 s of heavy stalls) while MemAvailable is under 10% of RAM (a process
  thrashing in its own memory limit stalls as hard with RAM free; Nova
  test) the next unit stops, one per streak, in order:
  `gpu-trace.service`, then `plugin_loader.service`. A unit that is not
  running is skipped. Each stop is logged with the PSI numbers and
  MemAvailable before and 2 s after.
- Barry Launcher's shelld closes its frozen apps at `some 300000 2000000`
  (15%) on every window, so it always acts first; memguard never touches
  Barry's or Flatpak's app scopes, and never stops the game, gamescope,
  Steam, steamwebhelper, PipeWire, WirePlumber, sddm or our own units.
- Once no trigger fired for 120 s, the units it stopped start again
  (Decky first); also when memguard itself stops (not at shutdown).

`journalctl -u pbos-memguard` shows what it did. `tests/memguard/run.sh`:
FIFOs in place of the triggers, a stub systemctl, fake unit cgroups, and
the real kernel trigger fired by a process held under `memory.high`.

The sleep path's one-shot programs run by name as
`pbos-platformd charge-rest|konkr-standby|led-fade ARGS`:
`sm8550-charge-rest` (now a two-line wrapper at its old path, so the
systemd-suspend drop-in, konkr-sleep and pbos-qamd call it as before),
`konkr-standby` (exec'd by konkr-suspend) and sm8550-sleep's LED fade.

`pbos-platformd --system|--user [--only NAME,...] [--list]`. Built in the
rootfs by `build.sh` with pbos-qamd's event loop (`../pbos-qam/daemon`),
installed by `scripts/apply-overlays.sh` as `/usr/lib/pbos-platformd/pbos-platformd`.

Measured on a Retroid Pocket Nova (Game Mode, idle, CPU per minute with
child processes): the five Python daemons ~1690 ms (volume-keeper's wpctl
polling alone 1662 ms), ~33 MB; these modules 17 ms, ~2 MB.

`tests/` holds the off-device equivalence harnesses (Python vs C++ in a
Linux VM with a fake sysfs, uinput devices and a real PipeWire). The
power-button test sends KEY_POWER and runs under a logind inhibitor, or the
VM's own logind would power it off.
