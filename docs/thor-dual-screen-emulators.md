# Two screens for DS, 3DS and Wii U games on the AYN Thor

> [!WARNING]
> **Work in progress.** This needs a gamescope that PB-OS images don't ship
> yet. The bottom screen itself works on a Thor; the emulators haven't been
> tried there yet.

In Game Mode, PB-OS's gamescope can show an emulator's second window on the
Thor's bottom screen. The emulator draws one screen per window, the top window
stays the game, and the bottom window replaces the Barry Launcher until the
game closes.

PB-OS doesn't ship any emulators. Install them yourself, for example with
EmuDeck. Nothing here changes EmuDeck; each emulator needs one setting
changed, once, and remembers it.

## The quick way: the Barry Launcher plugin

The Thor does this for you: **Quick Access → PB-OS → Display → Two-screen
emulators** is on by default. It finds melonDS, Azahar, Lime3DS,
Citra and Cemu (AppImage or Flatpak) and makes the changes below in their
settings files, also for an emulator installed later or reset by EmuDeck,
within half a minute and while that emulator isn't running (they write their
settings back when they quit). An emulator needs to have been opened once,
so its settings file exists.

Turning the switch off puts back what the emulators had, and leaves them
alone from then on. While it's on, changing these settings inside an
emulator doesn't stick; turn the switch off first.

DS games still need the standalone melonDS (see below). When Steam starts DS
games in RetroArch, the plugin says how many, but leaves them as they are:
switching them changes their Steam entries and where their saves live.

## By hand

The easiest place to change these settings is Desktop Mode: open the emulator
from the application menu, change the setting, and close it (File → Exit or
the window's close button; an emulator killed by a switch to Game Mode loses
the change).

## Nintendo DS: melonDS

Use the standalone melonDS. RetroArch's melonDS DS core draws both screens in
one window, so it can't use the bottom screen.

1. Open melonDS.
2. **View → Open new window.** A second melonDS window opens. melonDS opens it
   again every time it starts.
3. In the **first** window: **View → Screen sizing → Top only**.
4. In the **second** window: **View → Screen sizing → Bottom only**.
5. Close melonDS.

If EmuDeck added your DS games to Steam, it most likely used RetroArch. In
Steam ROM Manager, turn off the **Nintendo DS - RetroArch melonDS DS** parser,
turn on **Nintendo DS - melonDS (Standalone)**, and add the games again. Copy
your saves first: RetroArch keeps them in its own folder, and melonDS won't
find them there.

## Nintendo 3DS: Azahar

Azahar replaces Citra and Lime3DS, which are no longer developed. Use Azahar
for 3DS games; Lime3DS and Citra work the same way if you still have them.

1. Open Azahar.
2. **View → Screen Layout → Separate Windows.**
3. Close Azahar.

Azahar's interface must be in English. See [Other languages](#other-languages).

## Wii U: Cemu

1. Open Cemu.
2. **Options → Separate GamePad view.** The GamePad's screen goes to the bottom
   screen; the TV picture stays on top.
3. Close Cemu.

Cemu's interface must be in English. See [Other languages](#other-languages).

Cemu only publishes x86_64 builds, so on the Thor it runs through x86
emulation (box64), or from an unofficial ARM build. It may be too slow for some
games.

## Keeping the settings

EmuDeck's updates keep these settings. **Reset configuration** in EmuDeck puts
its own settings back (it saves the old file with a `.bak` ending), so after a
reset, change them again (with the plugin's switch on, it does that itself).

## Other languages

gamescope finds the second window by its title, which Azahar and Cemu
translate. The default titles it looks for are:

| Emulator | Second window's title |
|----------|-----------------------|
| melonDS | starts with `[w2] ` |
| Azahar | ends with `\| Secondary Window` |
| Cemu | `GamePad View` |

For another language, set `GAMESCOPE_BOTTOM_SCREEN_TITLES` in Game Mode's
environment to a POSIX extended regular expression that matches the translated
titles. It replaces the defaults, so include every emulator you use.

## When the bottom screen isn't free

If another program holds the bottom screen (an emulator build that drives the
bottom panel itself), gamescope leaves the second window alone and it opens on
the top screen like any other window.

Don't use these settings on PB-OS devices with one screen. There the second
window can end up covering the game.

## How it works

The code lives in PB-OS's gamescope fork, branch `dual-screen`:
<https://github.com/project-barry/gamescope/blob/dual-screen/DUAL-SCREEN.md>.
