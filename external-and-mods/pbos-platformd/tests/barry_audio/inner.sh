#!/bin/bash
# One scenario with one implementation, inside the rootfs (see run.sh).
set -u
scen=$1 impl=$2
export XDG_RUNTIME_DIR=/tmp/rt XDG_CONFIG_HOME=/tmp/cfg HOME=/tmp/home XDG_STATE_HOME=/tmp/home/.local/state
export PULSE_SERVER=unix:/tmp/rt/pulse/native
STATE=$XDG_STATE_HOME/barry_launcher_audio.json
QUICK=$XDG_CONFIG_HOME/barry_launcher/quick.json
CONFD=$XDG_CONFIG_HOME/pipewire/pipewire.conf.d
mkdir -p -m 0700 /tmp/rt /tmp/home "$CONFD" "$XDG_STATE_HOME" "${QUICK%/*}"
# The real outputs: null sinks that say they are hardware (device.api);
# "virt" is a virtual one.
sink() {  # name priority [device.api]
  echo "  { factory = adapter args = { factory.name = support.null-audio-sink node.name = \"$1\" ${3:+device.api = \"$3\"}
           media.class = \"Audio/Sink\" audio.position = [ FL FR ] priority.session = $2 } }"
}
{ echo "context.objects = ["; sink spk 1000 test; sink hdmi 900 test; sink dp 800 test; sink virt 100; echo "]"; } \
  >"$CONFD/50-test-sinks.conf"
cp /tmp/t/60-pbos-top-screen.conf "$CONFD/"
[[ $scen == notop ]] && rm -f "$CONFD/60-pbos-top-screen.conf"
dbus-daemon --session --address=unix:path=/tmp/rt/bus --fork >/dev/null 2>&1
export DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/rt/bus
pipewire >/tmp/pw.log 2>&1 &
sleep 0.5
wireplumber >/tmp/wp.log 2>&1 &
pipewire-pulse >/tmp/pwp.log 2>&1 &
for _ in $(seq 50); do [[ -S /tmp/rt/pulse/native ]] && break; sleep 0.1; done
sleep 1.5

step=pre
snap() { sleep "${1:-1.2}"; python3 /tmp/t/snapshot.py "$STATE" "$step" >>"/tmp/out/$scen.$impl.snap"; }
# The fake Barry Launcher gamescope: runs what is written to its fifo as its
# children (its cmdline names barry_launcher_session, its comm is gamescope-wl).
mkfifo /tmp/gs
cp /usr/bin/bash /tmp/gamescope-wl
/tmp/gamescope-wl -c 'while read -r cmd; do eval "$cmd" & done </tmp/gs' /usr/lib/barry_launcher/barry_launcher_session &
exec 4>/tmp/gs
PACAT="pacat --format=s16le --rate=48000 --channels=2"
play() {  # label [pacat args]: a stream that is no bottom app's
  local l=$1; shift
  $PACAT --property=test.label="$l" "$@" </dev/zero >/dev/null 2>&1 &
}
bottom() {  # label [pacat args]: a bottom app's stream
  echo "exec $PACAT --property=test.label=$1 ${*:2} </dev/zero >/dev/null 2>&1" >&4
}
bottom_ns() {  # label: a bottom app's stream from inside a pid namespace (as a Flatpak's)
  echo "exec unshare --pid --fork $PACAT --property=test.label=$1 </dev/zero >/dev/null 2>&1" >&4
}
stop_stream() { pkill -f "test.label=$1\b"; }
index_of() {  # the stream labeled $1
  pactl -f json list sink-inputs | python3 -c 'import json,sys
print(next(i["index"] for i in json.load(sys.stdin) if i["properties"].get("test.label")==sys.argv[1]))' "$1"
}
start() {
  if [[ $impl == python ]]; then cmd=(python3 /tmp/t/barry_launcher_audio)
  else cmd=(/tmp/t/pbos-platformd --user --only barry-audio); fi
  "${cmd[@]}" >"/tmp/out/$scen.$impl.log" 2>&1 &
  dpid=$!
  sleep 2
}
vol() { pactl set-sink-volume "$1" "$2"; }

vol spk 40%; pactl set-sink-mute spk 1; vol hdmi 30%; vol dp 100%; vol virt 77%
pactl list short sinks | grep -q pbos_top_screen && vol pbos_top_screen 60%
echo '{"appsVolume": 40}' >"$QUICK"
case $scen in
startup)  # streams already playing, no state file
  bottom app1; bottom_ns nsapp; play steam1 --device=spk; play lg1 --device=spk --property=node.link-group=testgroup
  play game1; snap 1.5
  start; step=started; snap
  ;;
switch)  # the default output moves to real ones; their top volumes are remembered
  bottom app1; play game1; sleep 1
  start; step=started; snap
  step=to-hdmi; pactl set-default-sink hdmi; snap
  step=top-55; vol pbos_top_screen 55%; snap
  step=to-dp; pactl set-default-sink dp; snap
  step=to-spk; pactl set-default-sink spk; snap
  step=to-hdmi-again; pactl set-default-sink hdmi; snap
  step=same-hdmi; vol pbos_top_screen 20%; pactl set-default-sink hdmi; snap
  step=to-virt; pactl set-default-sink virt; snap
  ;;
apps)  # bottom apps' streams come and go
  play game1; sleep 1
  start; step=started; snap
  step=app2-new; bottom app2; snap
  step=app2-moved-by-hand; pactl move-sink-input "$(index_of app2)" pbos_top_screen; snap
  step=app2-gone; stop_stream app2; snap
  step=nsapp-new; bottom_ns nsapp; snap
  step=steam-to-spk; play steam2 --device=spk; snap
  step=app3-vol-by-hand; bottom app3; sleep 1; pactl set-sink-input-volume "$(index_of app3)" 90%; snap
  step=spk-by-hand; vol spk 50%; pactl set-sink-mute spk 1; snap
  step=loopback-by-hand; pactl move-sink-input "$(index_of pbos_top_screen_out)" hdmi; snap
  ;;
usr1)  # the bottom volume moves (quick.json, then SIGUSR1)
  bottom app1; bottom_ns nsapp; play game1; sleep 1
  start; step=started; snap
  sig() { echo "$1" >"$QUICK"; kill -USR1 "$dpid"; snap; }
  step=apps-25; sig '{"appsVolume": 25}'
  step=apps-float; sig '{"appsVolume": 66.9}'
  step=apps-bad-json; sig '{"appsVolume": '
  step=apps-0; sig '{"appsVolume": 0}'
  step=apps-true; sig '{"appsVolume": true}'
  step=apps-over; sig '{"appsVolume": 101}'
  step=apps-string; sig '{"appsVolume": "30"}'
  step=apps-list; sig '[1]'
  step=apps-gone; rm -f "$QUICK"; kill -USR1 "$dpid"; snap
  step=no-signal; echo '{"appsVolume": 10}' >"$QUICK"; snap  # nothing told it
  step=usr2; kill -USR2 "$dpid"; snap  # not its signal
  ;;
linkgroup)  # filters' and loopbacks' own streams are left alone; a real output goes away
  play lg1 --device=hdmi --property=node.link-group=testgroup; bottom lgapp --property=node.link-group=g2
  play game1; sleep 1
  start; step=started; snap
  step=bt-new; pactl load-module module-null-sink sink_name=bt 'sink_properties=device.api=bluez5' >/tmp/bt.mod
  sleep 0.5; vol bt 70%; pactl set-default-sink bt; snap
  step=lg-to-bt; pactl move-sink-input "$(index_of lg1)" bt; snap
  step=bt-gone; pactl unload-module "$(cat /tmp/bt.mod)"; snap 2
  ;;
saved)  # a state file from before; a real default output at start
  printf '{"top": {"hdmi": 20, "spk": 70}, "real": "spk", "other": [1, "\\u00e9"]}' >"$STATE"
  pactl set-default-sink hdmi; bottom app1; play game1; sleep 1
  start; step=started; snap
  step=to-spk; pactl set-default-sink spk; snap
  step=to-dp-first; pactl set-default-sink dp; snap
  ;;
notop)  # no "Top screen" output: nothing to do
  bottom app1; play game1; sleep 1
  start; step=started; snap
  step=change; vol spk 45%; pactl set-default-sink hdmi; snap
  ;;
linked)  # Barry's volumeLinked: the bottom apps at the top's volume, quick.json too (C++ only)
  SETTINGS=${QUICK%/*}/settings.json
  link() { echo "{\"volumeLinked\": $1}" >"$SETTINGS"; kill -USR1 "$dpid"; }
  lsnap() { sleep 1.2; { echo "== $step"; cat "$QUICK"; echo; pactl -f json list sink-inputs | python3 -c 'import json,sys
for i in sorted(json.load(sys.stdin), key=lambda i: str(i["properties"].get("test.label"))):
    l = i["properties"].get("test.label")
    if l: print(l, {c["value_percent"] for c in i["volume"].values()}, "muted" if i["mute"] else "")'
    pactl get-sink-volume pbos_top_screen | grep -o "[0-9]*%" | head -1; pactl get-sink-volume spk | grep -o "[0-9]*%" | head -1; } >>"/tmp/out/$scen.$impl.snap"; }
  bottom app1; play game1; sleep 1
  start; step=off; lsnap
  step=on; link true; lsnap
  step=top-30; vol pbos_top_screen 30%; lsnap
  step=new-app; bottom app2; lsnap
  step=top-muted; pactl set-sink-mute pbos_top_screen 1; lsnap
  step=top-unmuted; pactl set-sink-mute pbos_top_screen 0; lsnap
  step=top-over; vol pbos_top_screen 130%; lsnap
  step=top-45; vol pbos_top_screen 45%; lsnap
  step=off-again; link false; vol pbos_top_screen 80%; lsnap
  step=apps-20; echo '{"appsVolume": 20}' >"$QUICK"; kill -USR1 "$dpid"; lsnap
  ;;
restart)  # PipeWire restarts under it
  bottom app1; play game1; sleep 1
  start; step=started; snap
  pkill -x pipewire-pulse; pkill -x wireplumber; pkill -x pipewire; sleep 1
  pipewire >/tmp/pw.log 2>&1 & sleep 0.5
  wireplumber >/tmp/wp.log 2>&1 &
  pipewire-pulse >/tmp/pwp.log 2>&1 &
  for _ in $(seq 50); do [[ -S /tmp/rt/pulse/native ]] && break; sleep 0.1; done
  sleep 1; vol spk 40%; bottom app4; play game4
  step=restarted; snap 6
  ;;
idle)  # CPU and memory: a minute idle with streams playing, then 20 changes
  bottom app1; bottom_ns nsapp; play game1; play game2; sleep 1
  start
  cpu() { awk '{print $14+$15+$16+$17}' "/proc/$dpid/stat"; }
  sleep 3; a=$(cpu); sleep 60; b=$(cpu)
  for n in $(seq 20); do vol pbos_top_screen "$((30 + n))%"; sleep 0.5; done
  sleep 1; c=$(cpu)
  echo "$impl: idle 60 s $((b - a)) ticks, 20 changes $((c - b)) ticks (hz $(getconf CLK_TCK)), $(grep VmRSS /proc/$dpid/status | tr -s ' \t' ' ')" \
    >"/tmp/out/$scen.$impl.cpu"
  ;;
esac
sleep 0.3
kill "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null
exec 4>&-
pkill pacat
kill $(jobs -p) 2>/dev/null
wait 2>/dev/null
true
