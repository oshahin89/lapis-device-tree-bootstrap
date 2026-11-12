# NT38771 host-download contract (2026-11-12, offline)

Source studied: Novatek's GPL NT387xx driver in
[Project-Hikari-MewType/android_kernel_xiaomi_dash_device_modules-EX](https://github.com/Project-Hikari-MewType/android_kernel_xiaomi_dash_device_modules-EX/tree/main/drivers/input/touchscreen/NT38771)
(`nt387xx.c`, `nt387xx_fw_update.c`, `nt387xx_mem_map.h`; "Copyright (C)
2024 Novatek, Inc.", GPL-2.0+). Its comments carry "P16 code" tags, the same
P16 project as lapis' panel `p16_36_02_0b`. The stock `nt38771.ko` on the
phone exports the same function names. Not copied into this repository; the
facts below are what an XNU driver needs. Nothing has been sent to the chip.

## SPI framing (mode 0, 8-bit, one CS assertion per message)

| Operation | Bytes on MOSI | Notes |
|---|---|---|
| set page | `FF`, `(addr >> 15) & 0xFF`, `(addr >> 7) & 0xFF` | write; selects the 128-byte window |
| write | `(addr & 0x7F) \| 0x80`, data… | after set page |
| read N | `addr & 0x7F`, then N−1 + 1 dummy bytes | MISO payload starts at the 3rd received byte (`rbuf + 2`) |

The stock binary's helpers match this (bit 7 write/read, two leading receive
bytes, one message per call).

## NT38771 addresses (`NT38771_memory_map`, `hw_reg_addr_info_old_spe2`)

| Name | Address | Use |
|---|---|---|
| `chip_ver_trim_addr` | `0x3F004` | 6 ID bytes; bytes 4–6 = `72 87 03` or `71 87 03` → NT38771 |
| `swrst_sif_addr` | `0x3F0FE` | `0x69` = bootloader reset (MCU), `0xAA` = SW reset to idle |
| `BOOT_RDY_ADDR` | `0x3F102` | write 1 after download |
| `EVENT_BUF_ADDR` | `0x31400` | event page: `+0x50` host command (`0xAE` = enable FW CRC), `+0x60` reset-complete status (`0xA0` = `RESET_STATE_INIT`), `+0x78` FW info |
| `RESX_DECOUPLE_ADDR` | `0x3F8D4` | write `AA 55`, read back, up to 3 tries, after the bootloader reset |
| ILM CRC bank | DES `0x3F7B4`, LEN `0x3F7B8` (3 B), golden `0x3F7BC` (4 B), result `0x3FBBC` | |
| DLM CRC bank | DES `0x3FB84`, LEN `0x3FB88` (3 B), golden `0x3FB8C` (4 B), result `0x3FBC0` | |
| DMA CRC | EN `0x3F10F`, flag `0x3F129`, `BLD_ILM_DLM_CRC` `0x3F127` | used by the skip-ILM path |
| hw info | `HWCRC_LEN_3Bytes`, `CHECK_SPI_DMA_TX_INFO`, `BLDMHEADER_PH2FLASH_ONE`, `use_gcm = 1` | |

## Chip-ID check (`nvt_ts_check_chip_ver_trim`)

Bootloader reset (`0x69` → `0x3F0FE`, 5 ms), set page `0x3F004`, the driver
writes 6 zero bytes there, then reads 6 bytes and matches bytes 4–6. A first
lapis probe can skip the reset and the zero write and only read.

## Download (`nvt_download_firmware_hw_crc`, up to 3 tries)

1. Bootloader reset with RESX decouple (`0x69` → `0x3F0FE`, 500 µs, `AA 55` →
   `0x3F8D4` and read back, 5 ms).
2. Program the ILM and DLM CRC banks: destination SRAM address (3 bytes),
   length (3 bytes), golden CRC (4 bytes) from the image header.
3. Write every non-empty partition (`size + 1` bytes) to its SRAM address in
   ≤ 15 KiB writes (set page, then one write message per chunk).
4. FW CRC enable: page `0x31400`, `+0x60` ← 0, `+0x50` ← `0xAE`.
5. Boot ready: `0x3F102` ← 1, 5 ms.
6. Poll the reset state at `EVENT_BUF_ADDR + 0x60` for `0xA0`.

## The lapis image (`tools/nt38771_fw_map.py device/novatek_ts_fw_tm.bin`)

Parses cleanly with the driver's algorithm: header end `0x100`, 13 info
sections, no overlay, no cascade. ILM → SRAM `0x000000` (`0x1696f` B, CRC
`0xbe81adde`, from file `0x2b0`); DLM → `0x020000` (`0x5d47` B, CRC
`0xbbb53bd5`, from `0x19000`); plus Info-3/4/5 and the Header (`0x025d48`,
`0xff` B). The SRAM ranges are contiguous where expected (ILM ends at
`0x1696f`, Info-4 starts at `0x16970`; DLM ends at `0x25d47`, Header starts at
`0x25d48`). 133,252 bytes to write in total, about 4–5 s over a 250 kHz
bit-banged SPI. FW version byte `0x29` (`0xd6` complement) at `0x2f000`.

## Still unknown

- Timing between steps on this board beyond the driver's delays; whether
  LK leaves the chip powered and out of panel reset at xnu entry (the chip-ID
  probe answers this).
- The post-boot normal report flow is already captured from the stock binary
  ([touch_module_analysis.md](touch_module_analysis.md)).
