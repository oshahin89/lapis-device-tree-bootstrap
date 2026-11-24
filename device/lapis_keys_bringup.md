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

## PMIF read path and the PMIC key bits (tpmic, 2026-11-22)

Stock `spmi-mtk-pmif.ko`: `mediatek,mt6878-spmi` → `mt6xxx_pmif_arb`
(`soc_chan` 2); `mt6xxx_regs` SWINF channels at `0x800 + 0x40·c`: ACC `+0x00`,
WDATA `+0x04`, RDATA `+0x14`, VLD_CLR `+0x24`, STA `+0x28`. `pmif_spmi_read_cmd`:
wait STA FSM (bits 1..3) = 0, ACC ← `type<<30 | sid<<24 | (len-1)<<16 | addr`
(EXT_REG_LONG = 3), wait FSM = 6, read RDATA, VLD_CLR ← 1.

`tools/lapis_shim/tpmic.c` (`build/tpmic_probe.img`, `2220d628…`) on the phone:
channel STA `0x8441` (idle), PMIC `0x8`/`0x9` = `40`/`63` (MT6363), 0 errors;
TOPSTATUS `0x1e` = `1E` at rest, bits 1–3 changed while Power and Volume up
were pressed → **Power = bit 1, Volume up = bit 3**, clear while down (as
stock `mt6363_regs`).

## Driver (v15)

`device/xnu-lapis/driver/sweet_lapis_keys.c` + patch/stage 0087: a thread
every 20 ms reads PMIC `0x1e` (PMIF channel 2), GPIO 54 and keypad `KP_MEM1`
bit 0, debounces (two equal reads) and calls `sweet_hid_key()` (0 power,
1 vol+, 2 vol−). Sources reading "down" at start are ignored. On by default,
`lapiskeys=0` disables; logs the first 30 changes (`LAPIS KEYS:`).

**v15 on the phone** (`build/lapis_keys_v15.img`, `e50eed70…`, CI 45023785265):
the user reports **all buttons work on iOS** (Power, Volume up, Volume down).
