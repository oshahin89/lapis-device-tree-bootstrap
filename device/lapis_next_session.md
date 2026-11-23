# Lapis test session plan (prepared 2026-11-22 evening, for 2026-11-23)

Working on iOS now: touch, full-screen display, buttons, battery + charging
(v18 `build/lapis_chg_v18.img`, `d96b8c99…`; since 2026-11-22 shipped as the dual image `build/lapis_dual_v1.img`, `1073ab60…`, see [lapis_dual_boot.md](lapis_dual_boot.md)). The vibration probe now ships inside `build/lapis_dual_v2.img` (`9999675a…`): Volume down at the prompt runs `tvib`, Power afterwards reboots to Android — no PC needed. Each probe below is flashed
to `boot_b` on top of stock HyperOS 4 `boot.img` (`a01d0f11…`); recover
with Power + Vol-down, then `fastboot flash boot_b device/hyperos4_readback/boot.img`.

## 1. Vibration — `build/tvib_probe.img` (`504df334…`)

AW86927 haptic driver, I2C2 (0x11c22000) address 0x5a, reset GPIO 116
(details in the header of `tools/lapis_shim/tvib.c`; registers from
Awinic's GPL haptic_hv driver, CONT mode, DT drive levels 0x7f/0x36).
Shows GPIO 116 + I2C2 state, reads the chip ID (raises GPIO 116 if the chip
does not answer), then **three 300 ms buzzes, 1 s apart**.
Report: did it buzz 3 times; the `CHIP` / `CHIP2` id (expect `9270`),
`errors`, and the `BUZZ` state lines (`state_running` 6 = CONT).

Next if it works: a kernel helper `lapis_vibrate(ms)`. iOS itself has no
vibration service in the sweet/topaz stack (only HID usage tables), so
system haptics need a new IOKit service — separate research.

## 2. Brightness — `build/tdsi_probe.img` (`6d155394…`), read-only

The OLED takes DCS long write `0x51 <hi> <lo>` (14 bit; stock
`panel-p16-36-02-0b-dsc-vdo.ko` `lcm_setbacklight_cmdq`), sent on DSI0
(0x1401a000) during video mode. Stock `mediatek-drm.ko`
`mt6878_dsi_driver_data`: CMDQ0 0xd00 / CMDQ1 0xd04, VM_CMD_CON 0x200,
VM_CMD_DATA 0x208/0x218/0x228/0x238, 0xc00, start bit 0x100(?); the stock
path writes them through GCE (`mtk_dsi_vm_cmdq`). The probe dumps what LK
left in DSI0 (mode, VM_CMD, CMDQ) twice, 1 s apart.
Report: a photo of the screen (or the CTRL `+014`, VMCMD and CMDQ0 lines).

Next: write the VM_CMD send for `0x51` from those values (brightness), then
screen off on lock (brightness 0 / DCS 0x28) in SweetFramebuffer's power-off.

## Still not started

USB (device mode for SSH/logs), audio (tfa98xx amp on I2C6), Wi-Fi/BT
(MT6878 connsys), cameras, sensors, GPU — each a project of its own.

## Vibration result (2026-11-22, dual_v6 / tvib v5) — **buzzes**

- AW86927 at I2C2 0x5a answers at once (ID 0x9270); GPIO 116 needs no reset.
- **I2C2 DATA_PORT must be loaded with 8-bit stores.** A 16-bit store puts
  two bytes into the FIFO (value + 0x00): `fifo loaded` 0x84 for two 16-bit
  stores vs 0x42 for two 8-bit ones, and the chip got `reg, 0x00`, so every
  write landed as 0 (v1..v4). This is also the "extra leading byte" seen on
  reads. (I2C5 charger writes in XNU work with 16-bit stores.)
- With 8-bit stores: Android's idle config (captured from haptic_hv sysfs
  `reg`, `tools/lapis_shim/aw86927_android.h`) clones with 8 mismatches, all
  read-only (LRA F0 0x25.. etc.), then CONT play exactly as the driver does
  (PLAYCFG1 0x57 bypass, PLAYCFG3 0x02, CONTCFG6 0xff, CONTCFG7 0x36,
  CONTCFG9 0xff, GO) reaches GLBRD5 = 6 (CONT_GO) and the user felt it.
- Next: an XNU driver (`lapis_vibrate(ms)`), then an iOS-side haptics hook.

## Brightness: DSI0 state (tdsi on the phone, 2026-11-22)

START 0x00 = 1, INTSTA 0x0c = 0x800b0712, CON_CTRL 0x10 = 0x01000000,
MODE_CTRL 0x14 = 2 (video), TXRX 0x18 = 0x0011003c, PSCTRL 0x1c = 0x2c050500,
VACT 0x2c = 0xad4 (2772), VM_CMD_CON 0x200 = 0x21 (EN | TS_VFP_EN), data 0,
CMDQ0 0xd00 = 0x00033902 / 0xd04 = 0x00390651: LK's last command was DCS long
write 51 06 39 = brightness 0x639. Values did not move in 1 s.
`tools/lapis_shim/tbl.c` (dual_v8, Volume down) sends DCS 0x51 through
VM_CMD (VM_CMD_DATA0 0x208, START bit 16, INTSTA bit 5) at 5 levels.

**Result (dual_v8, 2026-11-22): brightness changes on the phone.** All five
levels visibly applied; VM_CMD_DONE came after 0xf4..0x1a1 polls of 10 us
(2.4..4.2 ms, within a frame); START and INTSTA unchanged afterwards
(0x1 / 0x800b0712). Recipe: VM_CMD_DATA0 0x208 = 0x51 | hi << 8 | lo << 16,
VM_CMD_CON 0x200 = 0x00033923, clear INTSTA bit 5, START bit 16 0 -> 1, wait
INTSTA bit 5, START bit 16 -> 0, VM_CMD_CON back to 0x21. Next: an XNU
brightness hook (and screen off with level 0 or DCS 0x28/0x10).

## Brightness from iOS (2026-11-23) — not working yet

- v19/v20 (SweetBacklight, `backlight-control` = true): backboardd matches it
  and reads AppleARMBacklight-style properties (default-whitepoint-type,
  blr-cct-min/max/default/warning, pre-strobe-dim-period, pab-scaler-index,
  aml-table-factor) but **never sets a property and never opens it**; iOS
  becomes slow (v21b with `lapisbacklight=0` is fast again).
- v22 (service off) and v23 (on): moving the Settings slider sends **nothing**
  to the framebuffer user client (only boot-time sel 17/50/68/84) and sets no
  property on AppleCLCD; v23 shows only ~60 property reads, so the slowness is
  not a registry polling loop (probably CoreBrightness spinning in userland).
- The DCS 0x51 path itself works from XNU (`lapis_brightness_set`).
- Next: CoreBrightness reverse engineering (which service/property it wants on
  an OLED iPhone X), or a kernel-side screen-off on lock via DCS level 0.
