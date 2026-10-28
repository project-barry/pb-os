# pipewire-alsa

PB-OS replaces PipeWire's ALSA plugin (`/usr/lib/spa-0.2/alsa/libspa-alsa.so`)
with upstream [PipeWire](https://gitlab.freedesktop.org/pipewire/pipewire) at
the version in `REF`, plus the patches here. The rest of PipeWire stays stock.
`REF` must match the rootfs' PipeWire; the build stops when it does not.

**Why:** our speakers run the ALSA sink IRQ-driven
(`api.alsa.disable-tsched`, `54-konkr-quantum.conf`). In that mode the sink
turns its poll off when it asks the graph for a cycle, and only turns it on
again after the graph delivered a buffer. When the graph refuses the cycle
(the node is being reconfigured or stopped; logged only at info level), no
buffer comes and nothing wakes the sink again: the speakers stay silent
until PipeWire restarts. Seen live on a Thor: the PCM drained and stopped,
PipeWire's data loop slept with the PCM fd polled for errors only and both
timers off, every node had finished the last cycle and none started after.
It happened as Barry closed and a bottom-screen app opened during a game.

**Patch** `0001-alsa-irq-mode-keep-waking-when-the-graph-refuses-a-cycle.patch`:
when the ready callback fails in IRQ mode, the sink plays one quantum of
silence and turns its poll on again, so the next period interrupt retries
like a regular cycle. It logs a warning (rate limited) each time, so the
journal shows when it happens.

**Build:** `scripts/apply-overlays.sh` calls
`scripts/build-spa-alsa-in-rootfs.sh`, which builds only the plugin inside a
throwaway overlay of the Frame rootfs and reuses the result while `REF`, the
patches and the rootfs build stay the same.

**Changing it:** edit in a PipeWire checkout at `REF`, `git diff` into a
patch here, or move `REF` to the rootfs' new PipeWire and refresh the
patches.
