# Lapis touch bring-up (study, 2026-11-12)

The first XNU driver prototype is in `device/xnu-lapis/driver/sweet_nt38771.c`
and patch queue entries 0084–0085. The source stages the existing firmware,
loads it over GPIO bit-banged SPI3, polls reports and forwards contacts through
SweetHID. It is opt-in with `lapisnt=1`. GitHub Actions run [44506319609](https://github.com/oshahin89/lapis-device-tree-bootstrap/actions/runs/44506319609) built XNU successfully with all 85 patches applied and uploaded the `lapis-xnu-DEVELOPMENT` artifact. The firmware download and touch event have not been tried on the phone. Hardware facts are in
[lapis_spi3_xnu_contract.md](lapis_spi3_xnu_contract.md) and
[touch_module_analysis.md](touch_module_analysis.md).

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

## Integration status and remaining work

1. ~~The exact download sequence and register addresses.~~ Recovered from
   Novatek's GPL NT38771 source: [nt38771_host_download.md](nt38771_host_download.md);
   the lapis firmware image parses with the same partition logic
   (`tools/nt38771_fw_map.py`).
2. ~~Confirm the controller state at XNU entry.~~ The bit-banged SPI probe
   reads NT38771 ID `71 87 03` without touching GPIO90.
3. Compile the new driver and confirm the patch-built SweetHID uses the
   lapis 1125×2436 viewport. The integration opts in with `lapisnt=1` and
   maps the panel's centered vertical crop before dispatching HID events.

## Proposed order

1. Offline: XNU compilation succeeded in GitHub Actions; inspect warnings and
   preserve the artifact as a compile product only.
2. Device: only after a separately approved boot operation, enable
   `lapisnt=1`; check firmware boot-ready and version `0x29`.
3. Device: confirm report polling and try touch on the lock screen. Recovery
   and image restore remain separate device-operation requirements.

## Chip-ID probe (2026-11-12) — the NT38771 answers

`tools/lapis_shim/tchip.c` (`build.sh OUT tchip`): shows GPIO 68–71
mux/dir/do, switches only those pins to GPIO (CSB high, CLK low, MOSI out,
MISO in), sends `FF 07 E0` (page `0x3F004`) and a 6-byte read
(`04` + 7 bytes), twice, shows the eight received bytes and whether
`rx[5..7]` (driver `buf[4..6]`) are `72|71 87 03`, then returns to fastboot
after 30 s via PSCI. The page select is its only write to the chip; GPIO 90
is not touched. QEMU: runs to the countdown (pins read `FFFFFFFF` there).

**On the phone** (final image `b864fef0…`, which keeps the result on screen
instead of rebooting; earlier `78c4381d…`/`6133de23…` rebooted via PSCI after
30/120 s and did return to fastboot alone):

| | value | meaning |
|---|---|---|
| mode before / after (`+0x380`) | `22021111` → `00001111` | pins 68, 70, 71 were function 2 and 69 (CSB) already GPIO at xnu entry; only 68–71 changed |
| dir before / after (`+0x20`) | `04018020` → `04018070` | CLK/MOSI become outputs (CSB was already an output) |
| do / di before | `04000000` / `14065202` | |
| rx bytes 0–7 | `01 ff 00 00 00 71 87 03` | |
| ID (`rx[5..7]` = driver `buf[4..6]`) | **`71 87 03`** | **NT38771** (second trim-ID entry) |

So at xnu entry the controller is powered and out of reset (no GPIO 90
needed), and the bit-banged SPI3 framing from Novatek's source is right.
Android's touch works normally after restoring `boot_b` (`NVT-ts` interrupts
counting, `boot_b` `851a5435…`). The new driver prototype now attempts the
firmware download and report path; both still need compile and device evidence.

## Why the firmware never started under xnu (2026-11-22)

**Device log** of the last `lapisnt=1` boot (sweetlog): chip `71 87 03`,
15 partitions written, SRAM readback **and the chip's hardware CRC match the
image** (ILM `a2010048` / CRC `be81adde`, DLM `2b030327` / `bbb53bd5`),
`BOOT_RDY=01`, `HOST_CMD=ae` — yet the reset state stays `0x00`. So the write
path is right; the boot is not.

**Android's actual sequence**, captured with kprobes on the shipped
`nt38771.ko` (`CTP_SPI_WRITE/READ`, `nvt_set_page`, `nvt_write_addr`,
`nvt_*` boot functions) in a private tracefs instance (the main buffer is
consumed by another reader): `device/captures/nt38771_android_*_2026-11-22.txt`.

- Screen-on reload: `0x69 → 0x3F0FE`, 5 ms; DMA-CRC check of the resident ILM
  (BLD bank `0x3F108/0x3F10C/0x3F110`, `0x3F10F ← 1`, poll `0x3F129`) → ILM
  skipped; ILM/DLM CRC banks; DLM + Info-3/4/5 + Header written;
  `+0x60 ← 0`, `+0x50 ← 0xAE`; `0x3F102 ← 1`; state reached in ~70 ms.
- Full load (factory self-test, MP firmware incl. ILM): `0x69` only, banks,
  all partitions, `BOOT_RDY` → the firmware **boots to `0xA0`** (Android's
  dmesg: `buf[1]=0xA0`; the self-test then fails waiting for a later MP state —
  harmless, Android reloads its normal firmware; touch kept working).
- The shipped module has **no RESX-decouple function** (only
  `nvt_bootloader_reset`); Novatek's GPL revision has `AA 55 → 0x3F8D4`.
- Android waits for state `0xA1` and accepts a range; HyperOS 4 runs
  **FW 0x2B** (`/proc/tp_info`: `[Vendor]:TianMa [FW]:0x2b`), our image was 0x29.

Changes (driver + staging): plain bootloader reset (RESX decouple only with
`lapisnt_resx=1`), firmware = HyperOS 4 TianMa 0x2B
(`device/novatek_ts_fw_tm_hyperos4.bin`, sha256 df22cff3…), version check by
complement, ready = state `0xA0..0xAF`. Needs a CI kernel build and a device
boot with `lapisnt=1`.

### v8 result and the probe capture (2026-11-22)

v8 (kernel CI run 44999594414, plain reset, FW 0x2B): still `state=00`, with
the ILM/DLM CRC (`b2209eb7` / `5dff1136`) and the chip's CRC flags
(`CRC=0018`) identical to Android's own dmesg — the load is now byte-identical.
(The console text the user saw on touches was not from touch: the driver had
already stopped.)

Android's **cold** load, captured by unbinding/rebinding `spi3.0` from `NVT-ts`
with kprobes (`device/captures/nt38771_android_probe_cold_load_2026-11-22.txt`;
touch kept working): `nvt_gpio_config` (only `gpio_request(299)`, the IRQ
pin as input — no GPIO toggles), then `nvt_ts_check_chip_ver_trim_loop`:
`0x69 → 0x3F0FE`, 5 ms, **write six zero bytes at `0x3F004`**, read the ID;
~1 s later `Boot_Update_Firmware` does exactly our sequence (full ILM,
`skip_update_ilm = 0`) and the firmware is up in ~50 ms ("Update firmware
success! <186910 us>"). The chip check is the only remaining difference; the
driver now does it (reset, zero write, ID read) and waits 1 s before loading.
