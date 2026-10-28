# pipewire-alsa

PB-OS replaces PipeWire's ALSA plugin (`/usr/lib/spa-0.2/alsa/libspa-alsa.so`)
with upstream [PipeWire](https://gitlab.freedesktop.org/pipewire/pipewire) at
the version in `REF`, plus the patches here. The rest of PipeWire stays stock.
`REF` must match the rootfs' PipeWire; the build stops when it does not.

**Why:** our speakers run the ALSA sink IRQ-driven
(`api.alsa.disable-tsched`, `54-konkr-quantum.conf`). In that mode the sink
turns its poll off when it asks the graph for a cycle, and only turns it on
again after the graph delivered a buffer; there is no timer. When a cycle
never comes back (a client stalled or removed mid-cycle, the node being
stopped or reconfigured), nothing wakes the sink again: the speakers stay
silent until PipeWire restarts. Seen live on a Thor: the PCM drained and
stopped, PipeWire's data loop slept with the PCM fd polled for errors only
and every timer off, every node had finished the last cycle and none started
after. Timer mode does not have this hole, and PipeWire's graph already
counts a still-unfinished cycle as an xrun and starts the next one.

**Patch** `0001-alsa-irq-mode-give-every-cycle-a-deadline.patch`: a cycle
started by a period interrupt gets a deadline of two periods (the timerfd,
now created in both modes). The write that finishes the cycle disarms it. If
it fires first, the sink plays one period of silence, turns its poll on
again, and the next interrupt starts a regular cycle. The deadline is armed
only while the poll is off. The patch also restores the poll mask when a
follower becomes the driver again. A cycle the graph
refuses (the ready callback fails) is handled the same way at once. Warnings
(rate limited): `graph did not start a cycle` (refused) and
`cycle did not finish in time` (deadline), so the journal shows when it
happens. Capture (timer mode here) is unchanged.

**Build:** `scripts/apply-overlays.sh` calls
`scripts/build-spa-alsa-in-rootfs.sh`, which builds only the plugin inside a
throwaway overlay of the Frame rootfs and reuses the result while `REF`, the
patches and the rootfs build stay the same.

**Changing it:** edit in a PipeWire checkout at `REF`, `git diff` into a
patch here, or move `REF` to the rootfs' new PipeWire and refresh the
patches.
