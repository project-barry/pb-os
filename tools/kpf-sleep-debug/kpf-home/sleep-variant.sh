#!/bin/sh
# TEST ONLY (2026-10-06 power measurement): switch the KPF sleep variant.
#   standby       pb-os default screen-off standby (no kernel suspend)
#   s2idle-safe   kernel s2idle + all proven fixes, PCIe links left up
#   s2idle-pcie   as s2idle-safe + PCIe links forced down (pcid3dbg force_bridge_d3=1)
# Needs the test drop-ins in /etc/systemd (90-test-deck-pad, 91-test-round, sleep.conf.d).
set -e
v=${1:?usage: sleep-variant.sh standby|s2idle-safe|s2idle-pcie}
case $v in
standby)
	pbosctl sleep standby ;;
s2idle-safe|s2idle-pcie)
	pbosctl sleep s2idle
	printf '# TEST ONLY (2026-10-06) - remove with ~/.cache/deep-revert.sh\n[Sleep]\nMemorySleepMode=s2idle\n' \
		> /etc/systemd/sleep.conf.d/90-test-deep.conf
	grep -q "firmware image kept" /dev/kmsg 2>/dev/null || true
	lsmod | grep -q pcid3dbg && rmmod pcid3dbg
	if [ $v = s2idle-pcie ]; then
		insmod /home/steamos/pcid3dbg.ko force_bridge_d3=1 fix_ahb=1
	else
		insmod /home/steamos/pcid3dbg.ko fix_ahb=1
	fi
	for c in 100000.clock-controller 3d90000.clock-controller; do
		printf 1 > /sys/bus/platform/devices/$c/state_synced
	done ;;
*) echo "unknown variant $v" >&2; exit 2 ;;
esac
systemctl daemon-reload
echo "variant=$v sleep=$(pbosctl sleep status | awk '{print $3}') pcid3dbg=$(cat /sys/module/pcid3dbg/parameters/force_bridge_d3 2>/dev/null || echo -)"
