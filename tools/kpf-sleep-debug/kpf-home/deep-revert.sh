#!/bin/sh
# Undo the 2026-10-06 deep-sleep test setup on the Pocket FIT.
rm -f /etc/systemd/system/konkr-sleep.service.d/91-test-round.conf
rm -f /etc/systemd/sleep.conf.d/90-test-deep.conf /etc/systemd/system/konkr-sleep.service.d/90-test-deck-pad.conf
rmdir /etc/systemd/sleep.conf.d /etc/systemd/system/konkr-sleep.service.d 2>/dev/null
pbosctl sleep standby
echo s2idle > /sys/power/mem_sleep
systemctl daemon-reload
inputplumber device 0 targets set deck keyboard
[ -e /sys/devices/platform/soc@0/a600000.usb/driver ] || echo a600000.usb > /sys/bus/platform/drivers/dwc3-qcom/bind
echo 1 > /sys/bus/pci/rescan
systemctl start inputplumber.service
rmmod pcid3dbg 2>/dev/null
