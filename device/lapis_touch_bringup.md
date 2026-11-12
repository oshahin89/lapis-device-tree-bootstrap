# Lapis touch bring-up (study, 2026-11-12)

Offline study; no touch code exists and nothing was sent to the controller.
Hardware facts are in [lapis_spi3_xnu_contract.md](lapis_spi3_xnu_contract.md)
and [touch_module_analysis.md](touch_module_analysis.md).

## The controller is flashless

The stock client `nt38771.ko` contains `nvt_download_firmware_hw_crc`,
`nvt_download_init`, `nvt_write_sram`, `nvt_boot_ready`, `nvt_fw_crc_enable`,
`Boot_Update_Firmware` and messages such as "write to sram failed", "sram
program failed", "Download Init failed" and "skip download firmware". So the
NT38771 runs its firmware from SRAM, loaded by the host over SPI after each
reset; the image is `novatek_ts_fw_tm.bin` (196,608 bytes, version byte `0x29`
at `0x2f000`, matching the controller's reported FW `0x29`). The controller
resets are also reachable over SPI (`nvt_bootloader_reset`,
`nvt_sw_reset_idle`, `nvt_check_fw_reset_state`).

Consequence: when xnu takes over, the controller has been through LK's panel
power-up (GPIO 90 is the shared panel/touch reset) and Linux never ran, so it
is most likely without firmware. A touch driver must download the firmware
before any report can be read — unlike ruby's Goodix GT9916R, which keeps its
firmware in flash ("No firmware/config upload" in ruby's 0085).

## Transport

| | ruby (MT6877, Goodix) | lapis (MT6878, NT38771) |
|---|---|---|
| bus | SPI3, GPIO bit-bang, mode 0, 8-bit, CS low, 250 kHz | SPI3 pins GPIO 68 CLK / 69 CSB / 70 MOSI / 71 MISO (function 2 today), mode 0, 8-bit, ≤ 9.6 MHz |
| power | MT6360 LDO3/LDO2 set by the driver | display/touch share supplies; panel already on from LK |
| reset | GPIO 15 | GPIO 90, **shared with the panel** — avoid; use the SPI resets |
| IRQ | GPIO | EINT/GPIO 3, edge |
| firmware | in flash | **download 192 KiB to SRAM** |

A bit-bang first version (like ruby) needs only the GPIO block (base
`0x10005000`; pins 68–71: mode `+0x380` bits 16–31, DIR `+0x20`, DO `+0x120`,
DI `+0x220` bits 4–7, derived from upstream MT6878 pinctrl tables) and avoids
the MTK SPI controller's unknown clock setup. At 250 kHz the firmware download
takes about 6–7 s, acceptable for a first test.

## Client protocol (from the stock binary)

- Register access: first byte = address, bit 7 = 1 write / 0 read; read
  returns payload after two leading bytes; one CS assertion per message.
- Normal report: read 385 bytes, report area 384 bytes, checksum over
  offsets 1–64 against offset 65, 10 × 6-byte contact records
  (`nt38771_report_parser.c` already implements this and the slot state).
- Coordinates 0–20479 × 0–44351 = 16 × native 1280 × 2772.

## What is missing before code

1. The exact download sequence (`nvt_download_init`, SRAM write chunking,
   HW-CRC enable/check, boot-ready poll) and register addresses. Novatek's
   public NT36xxx SPI host-download driver (GPL, carried in many Android
   kernel trees as `nt36xxx_fw_update.c`) implements the same function names;
   compare it with the stock `nt38771.ko` disassembly for MT6878 addresses
   (the chip-specific memory map table, `trim_id` table).
2. Confirm the controller state at xnu entry with a read-only probe: after
   switching pins 68–71 to GPIO, read the chip ID registers over bit-banged
   SPI and display them (no writes to the controller).
3. Decide the XNU input path: ios-sweet's `SweetHID` (`sweet_hid_touch()`,
   which ruby's driver feeds) via `nt38771_sweet_hid_glue.c`; its logical
   size is hard-coded for 1080×2400 and must take 1280×2772.

## Proposed order

1. Offline: recover the download sequence and NT38771 memory map; write a
   host-side simulator of the transfers for review.
2. Device, read-only: chip-ID probe over bit-banged SPI (pins 68–71 only).
3. Device: firmware download + boot-ready check, display the FW version read
   back (`0x29` expected).
4. Device: report reading → `SweetHID`, then a touch test under the RAM-disk
   init.
