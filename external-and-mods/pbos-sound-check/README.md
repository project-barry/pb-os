# PB-OS Sound Check (temporary)

A Steam app (a non-Steam shortcut with Barry's head on it) for finding the
AYN Thor's sound dropout (reports from October 2026). It ships only until
that bug is found; then remove it from the image and Steam drops the
shortcut on its own (`TEMPORARY` in `pbos-default-shortcuts`).

What it does, with the controller (A, B, D-pad) or touch:

1. Asks whether the sound is gone right now, or works (after a restart).
2. Saves `pbos-report audio` (see `steamos-overlay/usr/bin/pbos-report`).
3. Offers two test tones and asks after each whether it was heard: one
   through the default output ("Top screen" on the Thor), one straight on the
   real output under it. Heard on the second but not the first means
   PipeWire's routing; neither means the driver, DSP or amplifier.
4. When the sound is gone and the first beep wasn't heard, tries two fixes,
   not optional, each followed by a fresh read of the audio path and the beep
   again: first the speaker stream (the output suspended and resumed, so
   PipeWire opens the DSP's playback stream again), then PipeWire itself (and
   PB-OS's sound helpers; Steam then needs a restart for its own sound, which
   the app offers). Lux's Thor reports (10-23) showed the ADSP's playback
   stream stopping ~15 s after it starts, with everything after it powered.
   Lux's .28 report (10-24): the speaker stream restarted by fix 1 froze
   again within 0.1 s; only fix 2 (the stream closed and opened again)
   brought the sound back. So each read of the audio path also carries what
   the audio event recorder (`pbos-report-audio-trace.service`, from boot)
   saw since the last read: LPASS clocks and DSP votes, runtime PM of the
   codec macros and SoundWire, DAPM, each DSP graph and DAI step with its
   result, and the data flow to and from the DSP, so the moment the stream
   froze shows what changed with it.
5. Saves the report with the answers in `~/Desktop/PB-OS Reports`.
6. Where the image has a report address (`/usr/share/pb-os/report-url`),
   offers to send it to the PB-OS team (see below). Then, after a "sound is
   gone" report, asks for a restart and a second, "sound works" run.

## The tones

Ear safety: each tone is one steady 440 Hz beep, 1 s, at -26 dBFS, faded in
and out over 0.15 s. Never a sweep or a click. The first plays at the user's
volume. The real output under "Top screen" is held at 100 %, so the second
plays with the Top screen's volume as its own and is never louder than the
first, on speakers or headphones. Nothing plays until the user picks "Play".

## Sending

Only when the user taps Send. The device sends a copy (saved beside the
report as `-sent.txt`) without Wi-Fi and network lines, with MAC addresses,
IP addresses and the host name hidden, at most 400 KB. It goes to the
project's report relay (`pb-os-installer-apk/relay`, a Cloudflare Worker:
`/sound-report`), which masks addresses again and posts the file to a
private Discord channel. The Discord webhook is only in the Worker's
secrets. The relay's address is kept out of this repo: `apply-overlays.sh`
writes it into the image from the build host's `/work/pbos-report-url`
(`PBOS_REPORT_URL_FILE`). Without that file the app only saves.

## Parts

| Path | Installed as |
|---|---|
| `main.cpp`, `build.sh` | `/usr/lib/pbos-sound-check/pbos-sound-check` (SDL2, SDL2_ttf, Noto Sans; built in the rootfs by `apply-overlays.sh`) |
| `art/*.png`, `make-art.py` | `/usr/share/pb-os/sound-check/`: Steam's library art and icon, copied into each account's grid folder |

`pbos-sound-check --scrub FILE` prints the copy that would be sent for a
saved report.
