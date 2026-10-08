#!/bin/sh
# TEST ONLY (2026-10-06): release audio before s2idle. $1 = suspend|resume
U=steamos; R=/run/user/$(id -u $U)
pa() { runuser -u $U -- env XDG_RUNTIME_DIR=$R pactl "$@"; }
log=/var/tmp/audio-release.log
if [ "$1" = resume ]; then
  for n in $(pa list short sinks | cut -f2); do pa suspend-sink "$n" 0; done
  for n in $(pa list short sources | cut -f2); do pa suspend-source "$n" 0; done
  exit 0
fi
echo "== $(date +%T) suspend" >> $log
i=0
while [ $i -lt 30 ]; do
  for n in $(pa list short sinks | cut -f2); do pa suspend-sink "$n" 1; done
  for n in $(pa list short sources | cut -f2); do pa suspend-source "$n" 1; done
  open=$(grep -lv closed /proc/asound/card*/pcm*/sub*/status 2>/dev/null | wc -l)
  [ "$open" = 0 ] && break
  sleep 1; i=$((i+1))
done
sleep 5   # let the LPASS macros runtime-suspend (3 s autosuspend)
echo "waited ${i}s open_pcms=$open" >> $log
cd /sys/kernel/debug/clk && for c in LPASS_HW_MACRO LPASS_HW_DCODEC; do echo "$c $(cat $c/clk_prepare_count)"; done >> $log
for c in /sys/kernel/debug/clk/LPASS_CLK_ID_*; do p=$(cat $c/clk_prepare_count); [ "$p" != 0 ] && echo "held ${c##*/} $p"; done >> $log
exit 0
