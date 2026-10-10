# Retroid Pocket Nova firmware

Retroid's own firmware from the Nova's stock Android (build RPN_V1.0.0.436,
2026-07-22), used instead of AYN Odin 2's on the Nova:

- `adsp.mbn`, `adsp_dtb.mbn`: ADSP `ADSP.HT.5.8-01329-KAILUA-1`, OEM build
  `sz-d-l-011216b`, from `/vendor/firmware/adsp.mdt` + `adsp.bNN` and
  `adsp_dtb.mdt` + `.bNN`, joined into one file each (segments placed at their
  ELF offsets, like pil-squasher). Retroid's charge controls (USB properties
  0x0f/0x10) only work with this ADSP.
- `aw883xx_acf.bin`: Awinic AW88166 speaker tuning (project A1901), unchanged
  from `/vendor/firmware`.
- `*.jsn`: protection-domain maps from the modem partition (`image/`),
  identical to Odin 2's.

With this ADSP the amps need stock's channel numbering (amp 0x34 on channel 1,
0x35 on channel 0), set in `external-and-mods/kernel-sm8650/sm8550/dts/qcs8550-retroidpocket-rpnova.append`.
Tested on a Nova 2026-10-10: speakers (sides, no crackle), battery, charging,
sleep/wake.
