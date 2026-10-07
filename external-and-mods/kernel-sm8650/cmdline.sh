#!/usr/bin/env bash
# ABL cmdline for SM8650 SteamOS. ABL picks the DTB by model — never put
# devicetree=/dtb= here. No initramfs: root must be PARTUUID (or /dev/…),
# the kernel cannot resolve root=UUID= on its own.
#
# Quiet by default: no kernel text, penguin logo or blinking cursor on the
# panel before Steam (the journal still has everything). CMDLINE_QUIET=0 gives
# the verbose console back (scripts/sd-debug-boot.sh sets it).

build_cmdline() {
  local partuuid="$1"
  # No clk_ignore_unused / pd_ignore_unused: those are SM8550 (MaSi) flags;
  # ROCKNIX boots SM8650 without them and they can upset display bring-up.
  local -a parts=(video=efifb:off)
  # No irqaffinity=: interrupts keep the kernel default (all CPUs). Only the
  # GPU's must stay off the little cores (A740/A750 GMU hang, Armada
  # e2d9802), and the kernel always adds the boot CPU to this mask anyway, so
  # sm8550-irq-affinity.service moves just those.
  parts+=(
    # Pocket FIT pad is an XInput device on USB; 2 ms polling like ROCKNIX
    # (needs 0506-usbcore-add-interrupt-interval-override.patch).
    usbcore.interrupt_interval_override=045e:028e:2
    # (The HID mode, 4001:0428, already polls at 1 ms by its own descriptor.)
  )
  # SM8650 (Pocket FIT) stays on s2idle until its deep sleep is validated.
  if [[ "${SOC:-sm8650}" == sm8550 ]]; then
    # Deep sleep reaches full SoC sleep on SM8550 (kernel patches 0531-0536
    # + sm8550-sleep). console=tty0 keeps the kernel console off the debug
    # UART (stdout-path), whose GENI clock would otherwise keep the XO on.
    parts+=(mem_sleep_default=deep console=tty0)
  else
    parts+=(mem_sleep_default=s2idle)
  fi
  if [[ "${CMDLINE_QUIET:-1}" == 1 ]]; then
    parts+=(quiet loglevel=0 systemd.show_status=0 rd.udev.log_level=0
            logo.nologo vt.global_cursor_default=0)
  else
    parts+=(console=tty0 loglevel=4)
  fi
  parts+=(
    rw rootwait
    "root=PARTUUID=${partuuid}"
    rootfstype=ext4
    errors=remount-ro
  )
  if [[ -n "${KERNEL_CMDLINE_EXTRA:-}" ]]; then
    local -a extra
    read -ra extra <<<"${KERNEL_CMDLINE_EXTRA}"
    parts+=("${extra[@]}")
  fi
  printf '%s' "${parts[*]}"
}
