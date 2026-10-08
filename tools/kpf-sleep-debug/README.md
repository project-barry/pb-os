# KONKR Pocket FIT deep-sleep debug kit (test only)

Tools used on 2026-10-06/07 to get the Pocket FIT from "CPUs asleep" to full
SoC sleep, and the measurements behind kernel patches 0009-0016 and the
`konkr-sleep` changes. Nothing here is installed by the image builder.

## Files

- `pcid3dbg.c`, `Makefile`: out-of-tree module, built against the SM8650
  kernel tree (`make -C <tree> M=$PWD modules`). Loaded with `insmod`,
  never installed. Parameters:
  - `force_bridge_d3=1`: allow D3 on the Qualcomm root ports (now patch 0012)
  - `only_domain=N`: force only PCI domain N (0 = Wi-Fi, 1 = Renesas xHCI)
  - `fix_ahb=1`: unprepare the DSI PHY AHB clock in noirq (now patch 0011)
  - `mask_irq=N`: mask one IRQ in the deep-suspend window (did not help)
  - `off_gpios=a,b,...`: drive these TLMM GPIOs low between syscore suspend
    and resume, restore them first thing on resume (`off_delay_ms`, 20)
  - always: logs PCI power states, the GIC SPIs pending at syscore suspend
    and resume (the wake source in deep sleep), and the TLMM GPIOs driven
    high at syscore suspend (skips the secure `gpio-reserved-ranges`;
    reading those faults)
- `kpf-etc/`: test drop-ins on the device (sleep mode, Deck pad switch,
  per-round hook with battery/IRQ/qcom_stats snapshots around each sleep)
- `kpf-home/`: `sleep-variant.sh` (standby / s2idle-safe / s2idle-pcie),
  `audio-release.sh` (wait for all PCMs closed), `deep-revert.sh` (undo all)
- `swapkernel-dtb-kpf.py`: repack `/boot/KERNEL` with a new Image and
  Pocket FIT DTB, keeping the header, cmdline, ramdisk and the other DTB

## Results (USB-C meter at 5 V with the battery full, or battery counter)

| State | Power |
|---|---|
| Awake, Steam home | 3.15-3.5 W (meter) |
| konkr-standby | 1.25-1.3 W (meter) |
| s2idle, PCIe links up | 1.0 W (meter), no CX collapse |
| deep, both links down + 0013/0014 floor | 0.49-0.54 W (meter), CX ~100 % |
| + battery notifications off (0015), 6 h | 47 mAh/h ≈ 0.36 W (battery) |
| + fan rails off in sleep (GPIO 124/125) | ~10-18 mAh/h ≈ 75-135 mW (battery, 38 min) |

## Findings still to turn into patches

- Fan: `fan_vdd` (GPIO 124) and `fan_pwr` (GPIO 125) stay on in deep sleep
  and cost about 0.27 W; pwm-fan's suspend should cut them.
- Still high at suspend: GPIO 28 (gamepad power, pinctrl output-high, no
  clear saving), 163/164 (panel power, pinctrl output-high; untested),
  77 (speaker SD_N), 107 (audio codec reset), 161 (touch reset).
- Wake press is sometimes handled as a sleep press (logind "Power key
  pressed short" then "The system will suspend now!" within a second).
- `aosd` (AOSS sleep) is still 0.
