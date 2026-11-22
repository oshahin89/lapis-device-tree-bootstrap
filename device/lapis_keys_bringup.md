# Lapis buttons bring-up (started 2026-11-22)

## Wiring (runtime FDT + stock modules)

| Button | Where | Read |
|---|---|---|
| Volume down | `gpio-keys`: GPIO 54, active low, pull-up, func 0 | GPIO DI `0x10005210` bit 22 |
| Power | MT6363 PMIC (`mt6363-keys`, SPMI slave 4) | PMIC TOPSTATUS `0x1e`, bit 0 (stock `mtk-pmic-keys.ko` `mt6363_regs`: `1e 1 90f 0 / 1e 3 90f 1 …`) |
| Volume up | MT6363 "home" key (code `0x73` = KEY_VOLUMEUP) | PMIC `0x1e` (second entry of `mt6363_regs`) |

`tools/lapis_shim/tkeys.c` (`build/tkeys_probe.img`, `23f3c17c…`) showed the
SoC keypad `kp@1c00e000` registers and GPIO 54 live: **only Volume down
changed anything** — Power and Volume up are not routed to the keypad, so
they need PMIC reads over SPMI.

## SPMI / PMIF (next)

`spmi@1cc04000` (`mediatek,mt6878-spmi`): PMIF `0x1cc04000` (size `0x8ff`),
SPMI master `0x1cc01000`. Stock `spmi-mtk-pmif.ko` (vendor_boot
`lib/modules`) has `mt6xxx_regs` / `mt6873_regs` / `mt6853_regs` tables and
`mt6xxx_pmif_arb` (most of the match table's `.data` points at `.data+0x410`
= `mt6xxx_pmif_arb`); `pmif_spmi_read_cmd` at `.text+0xa4`. Still to do:
confirm which entry `mediatek,mt6878-spmi` uses, take the SWINF channel
offsets (STA / SEND / RDATA / VLDCLR) from `mt6xxx_regs`, then a shim probe
that reads PMIC `0x1e` live (read-only), then the xnu key driver feeding
`sweet_hid_key()` (SweetHIDButtons already registers).
