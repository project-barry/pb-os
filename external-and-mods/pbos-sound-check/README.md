# PB-OS Sound Check (temporary)

A Steam app (a non-Steam shortcut with Barry's head on it) for finding the
AYN Thor's sound dropout (reports from October 2026). It ships only until
that bug is found; then remove it from the image and Steam drops the
shortcut on its own (`TEMPORARY` in `pbos-default-shortcuts`).

What it does, with the controller (A, B, D-pad) or touch:

1. Asks whether the sound is gone right now, or works (after a restart).
2. Saves `pbos-report audio` (see `steamos-overlay/usr/bin/pbos-report`).
   The root part (`pbos-report-audio-root.service`) runs first and starts
   with the stall check (below), which reads the stream as found, before
   anything plays or is changed.
3. Offers two test tones and asks after each whether it was heard: one
   through the default output ("Top screen" on the Thor), one straight on the
   real output under it. Heard on the second but not the first means
   PipeWire's routing; neither means the driver, DSP or amplifier.
4. When the sound is gone and the first beep wasn't heard, tries the fixes
   for the stall class, not optional, PipeWire's restart always last (see
   "Stall classes and fixes"). After each: the playback pointer check, a
   fresh read of the audio path, and, when the pointer moves, the beep again.
   PipeWire's restart also restarts PB-OS's sound helpers; Steam then needs a
   restart for its own sound, which the app offers. Lux's Thor reports
   (10-23) showed the ADSP's playback stream stopping ~15 s after it starts,
   with everything after it powered. Lux's .28 report (10-24): the speaker
   stream reopened by the first fix froze again within 0.1 s; only PipeWire's
   restart brought the sound back. So each read of the audio path also carries what
   the audio event recorder (`pbos-report-audio-trace.service`, from boot)
   saw since the last read: LPASS clocks and DSP votes, runtime PM of the
   codec macros and SoundWire, DAPM, each DSP graph and DAI step with its
   result, and the data flow to and from the DSP, so the moment the stream
   froze shows what changed with it.
5. Saves the report with the answers in `~/Desktop/PB-OS Reports`. Its
   first lines: the stall class, which fix brought the sound back (or none),
   then each answer with the pointer check's result.
6. Where the image has a report address (`/usr/share/pb-os/report-url`),
   offers to send it to the PB-OS team (see below). Then, after a "sound is
   gone" report, asks for a restart and a second, "sound works" run.

## Stall classes and fixes

Seen live on a Thor (10-27): the speaker PCM stays RUNNING with its hw_ptr
frozen while the q6apm DSP still answers; PipeWire simply stopped writing.
The speakers run IRQ-driven (`api.alsa.disable-tsched`,
`54-konkr-quantum.conf`): PipeWire 1.6.8's ALSA sink turns its poll off
when it asks the graph for a cycle (`playback_ready()` in
`spa/plugins/alsa/alsa-pcm.c`) and only turns it on again when it writes;
a refused cycle leaves it off for good. PB-OS's patched plugin
(`../pipewire-alsa`) now plays silence and polls again, and logs
`graph did not start a cycle`; the stall check shows whether it is the one
loaded and how often it warned.

The stall check (`audio_stall` in `pbos-report`, root) reads every RUNNING
playback PCM twice, 1 s apart, and the PipeWire process holding it
(`owner_pid`): each thread's state, wchan, syscall, kernel stack and context
switches in that second; the epoll sets holding the PCM's fd, with every fd
in them resolved (timerfd and eventfd state); the recorder's writes to the
DSP and packets back in that second. Then PipeWire's nodes and their clients
(`pw-dump`, as the session user), `pw-top -b`, the mapped `libspa-alsa.so`'s
sha256 and whether it has the patched string, the count and last lines of
that warning, and the journal (`-o short-monotonic`) of PipeWire,
WirePlumber, Barry, the bottom screen and units starting and stopping in the
2 minutes before the freeze (the frozen status's `tstamp`, CLOCK_MONOTONIC;
`/proc/uptime` would count sleep).

| Class | Evidence | Fixes, in turn |
|---|---|---|
| A: PipeWire stopped writing | hw_ptr frozen, the PCM's fd in the data loop's epoll set without POLLOUT (`events: 18`), no timerfd armed (the data loop's switches are shown, not required: other fds on the same loop may still wake it) | 1. the speaker node paused and started, 2. PipeWire restarted |
| B: DSP or kernel | hw_ptr frozen while PipeWire still waits for the PCM (POLLOUT or a timer armed) or keeps writing to the DSP | 1. the speaker stream reopened, 2. PipeWire restarted |
| C: anything else (the sound working too) | everything above, recorded | as B |

The fixes, from PipeWire 1.6.8's source:

- **Speaker node paused and started** (class A): `pw-cli send-command
  <node> Pause '{}'`, then `Start '{}'`. The server hands every node command
  but Suspend straight to the node (`node_send_command()` in
  `impl-node.c`), so PipeWire's graph, the other nodes and the streams are
  untouched. In the adapter, Pause stops the converter and the ALSA sink
  (`spa_alsa_pause()`: `started` off, the poll sources taken out of the data
  loop, `snd_pcm_drop`); Start (`spa_alsa_start()`) prepares and starts the
  PCM and adds the poll sources anew with their full events, POLLOUT
  included. The PCM stays open. Start alone would do nothing:
  `spa_alsa_start()` returns at once while `started` is set.
- **Speaker stream reopened** (B and C): `pactl suspend-sink <real> 1`.
  pipewire-pulse turns it into the Suspend command, which the server runs
  through the node's state machine (`suspend_node()`): the node leaves the
  graph, the sink pauses and its format is cleared, so the PCM closes and
  the DSP's graph with it; the next stream opens it again. The resume
  (`... 0`) is a no-op in pipewire-pulse. Heavier than A's fix and not
  needed for it.
- **PipeWire restarted**: as before; the only proven fix so far.

Each fix is checked first by the playback pointer: 3 s of silence play on the
real output and its hw_ptr must move over 1 s (`/proc/asound/card*/pcm*p/sub0/status`,
the card and device from the output's `alsa.card`/`alsa.device`). Frozen:
the next fix follows without a beep. Moving: the beep, and the user's answer
decides. The full-restart step comes only after the pointer stayed frozen or
the beep wasn't heard.

Not shipped: logging PipeWire's refusals themselves (`ready non-active node`
in `pw.node`, `ready stopped node` in `spa.audioadapter`, both info level)
with a `PIPEWIRE_DEBUG` drop-in for `pipewire.service`. The adapter's line
has no rate limit and comes once per period while the graph refuses: with
the patched plugin polling again, that is 50 lines a second (960 frames at
48 kHz) for as long as it lasts, and `pw.node` at info also logs every node
state change and xrun. It could not be measured off the device; if wanted,
try `PIPEWIRE_DEBUG=W,pw.node:I,spa.audioadapter:I` in a drop-in on one
device first and count the journal's lines per hour of play.

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
saved report, `--pointer [OUTPUT]` runs the playback pointer check (the real
output by default), `--stall-class FILE` prints the class a saved root read
(`/run/pbos-report/audio-root.txt`) gives. `pbos-report` and the pointer
check read `PBOS_REPORT_ASOUND` in place of `/proc/asound` (tests only).

`pbos-report-audio-root.service` has `ProtectHome=read-only`, not `yes`:
the silence it plays and `pw-dump`/`pw-top` reach the session's PipeWire
through its socket in `/run/user`, which `yes` hides.
