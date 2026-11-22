# Lapis test session plan (prepared 2026-11-22 evening, for 2026-11-23)

Working on iOS now: touch, full-screen display, buttons, battery + charging
(v18 `build/lapis_chg_v18.img`, `d96b8c99…`). Each probe below is flashed
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
