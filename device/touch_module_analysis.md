# Lapis touch controller and firmware inventory

Collected read-only from the live rooted Android session and the active stock firmware. Device-side files were streamed to the workstation; no file was created or changed on the phone.

## Active hardware path

- Runtime FDT node: `/soc/spi3@11013000/novatek@0`.
- SPI compatible and live modalias: `novatek,NVT-ts-spi` / `spi:NVT-ts-spi`.
- Live SPI driver: `NVT-ts`; module: `nt38771` (description: Novatek Touchscreen Driver).
- The module is bound to `spi3.0` and creates input device `NVTCapacitiveTouchScreen` at `/dev/input/event5`.
- SPI maximum clock: 9,600,000 Hz; chip select 0. Disassembly of `nvt_ts_probe` shows the driver writes `bits_per_word = 8` and `spi_device.mode = 0` immediately before calling `spi_setup`; this is direct evidence the active touch client requests 8-bit SPI mode 0 (CPOL=0, CPHA=0). Its synchronous helpers zero-initialize transfer descriptors and do not set per-transfer `speed_hz`; the matching Linux SPI core defaults a zero transfer speed to the device's `max_speed_hz`, which the FDT sets to 9.6 MHz. Thus 9.6 MHz is the Linux requested ceiling/default target; the actual SCK after controller divider rounding has not been measured.
- Reset GPIO property: phandle `0x8f`, pin 90, flags 0. IRQ GPIO property: phandle `0x8f`, pin 3, flags `0x2001` (raw DT cells; flag interpretation needs the matching pinctrl binding).
- Input reports up to 10 multitouch slots (slot indices 0–9), X 0–20,479 and Y 0–44,351. Those coordinate ranges are exactly 16 units per native 1280×2772 display pixel; touch-major is 0–255.
- The touch reset is GPIO 90, also declared by the active panel node. They share the same pin controller phandle; reset sequencing must coordinate both devices.
- The runtime FDT's SPI3 pinctrl state `spimode-default` (phandle 732) configures pinmux encodings `0x4402`, `0x4502`, `0x4602`, and `0x4702`: pins 68–71, function 2. The MT6878 pin-function table identifies these as GPIO68=SPI3_CLK, GPIO69=SPI3_CSB, GPIO70=SPI3_MO, and GPIO71=SPI3_MI ([upstream MT6878 pin-function definitions](https://codebrowser.dev/linux/linux/arch/arm64/boot/dts/mediatek/mt6878-pinfunc.h.html#490)). Thus the live pinmux order is CLK, CSB, MOSI, MISO. The `default` state (phandle 727) is empty. The SPI3 controller node also carries states named `gt9896s_int_active`, `gt9896s_reset_active`, `gt9896s_int_suspend`, and `gt9896s_reset_suspend`; those states configure GPIO7 as input/low-in-suspend and GPIO6 as output-high/low-in-suspend. These controller state names do not align cleanly with the child touchscreen node, which separately declares reset GPIO90 and IRQ GPIO3 (raw flags `0x2001`); preserve both observations and resolve the vendor driver's state handling before mapping touch IRQ/reset in XNU. The stale `gt9896s` labels also differ from the live NT38771 controller. The published upstream pin table confirms the SoC mux capabilities, while the live FDT is the evidence for which function lapis selects.
- Runtime chosen properties include `lcm_panel_id = 0x10` and raw `tp_lockdown_info = 46 36 32 30 50 16 31 00` (the DT string is the hex text `4636323050163100`). The lockdown field's individual meanings remain unknown; however, static analysis of the live driver shows firmware selection is keyed by `lcm_panel_id`.
- Read-only proc status from the live controller reports `fw_ver=41, x_num=18, y_num=39`; `/proc/tp_info` reports `[Vendor]:TianMa [TP-IC]:NT38771 [FW]:0x29`, and `/proc/tp_lockdown_info` matches the chosen-node value above. The detected touch matrix is 18×39 channels.
- A fresh read-only `/proc/interrupts` snapshot on the rooted phone shows `mtk-eint 3 Edge NVT-ts` and `GICv3 494 Level 11013000.spi3`, confirming that the active touch interrupt is EINT/GPIO 3 and the SPI3 host interrupt is GIC IRQ 494. `/sys/kernel/debug` is not mounted in the current Android session, so active pinctrl debug state could not be inspected; this leaves the parent SPI node's GPIO6/7 state entries unexplained.

## Driver module

The active `nt38771.ko` was streamed from `/vendor_dlkm/lib/modules/nt38771.ko` and saved beside this note. SHA-256: `85d5cae16b05536671ab721f22e40b2ecefea17a9c7961fd5e6fb33911649720`; file size 658,640 bytes. ELF metadata says AArch64 `ET_REL`, GPL, module name `nt38771`, and dependencies on `hqsysfs`, `xiaomi_touch_common`, all three panel modules (`p16-36`, `p16-41`, `p16-42` variants), and `mediatek-drm`.

The module requests touch firmware from `/vendor/firmware`; the live bootargs set `firmware_class.path=/vendor/firmware`. It has firmware-update, SPI, suspend/resume, IRQ, gesture and input-reporting routines. The module's `vermagic` is `6.1.138-android14-11-ga77ed0ba4c27`, while the running kernel reports `6.1.138-android14-11-g51f8c580613d-ab13911623`. The panel module has the same `ga77...` vermagic. Both drivers are live, so this discrepancy is recorded for source/build investigation rather than treated as proof of incompatibility.

## SPI framing recovered from the stock client binary

Static disassembly of `CTP_SPI_READ` and `CTP_SPI_WRITE` establishes a minimal register-access convention:

- Both helpers serialize access with a mutex and submit synchronous SPI messages through `spi_sync`; each retries a failed message up to five times.
- Each helper builds one SPI message with one transfer-list insertion. Its transfer descriptors are zero-initialized, so `cs_change` remains false: Linux keeps CS asserted for the whole helper transaction and releases it at message completion. Read and write are separate helper calls/messages, so CS is released between them.
- Read clears bit 7 in the first command/address byte. It copies the returned bytes starting two bytes into the receive buffer back to the caller starting at `buf + 1`, for `len - 1` bytes. This shows two leading wire/receive positions before the returned register payload in this helper's framing.
- Write sets bit 7 in the first command/address byte before forming the synchronous message. This is consistent with bit 7 marking write versus read for this client's register protocol.
- The normal touch worker calls `CTP_SPI_READ` with a zero-initialized buffer and length `0x181` (385 bytes). The helper then exposes 384 returned bytes in buffer positions 1–384; the worker parses those bytes as the touch report. A separate code path uses a 109-byte read.
- Before parsing a normal report, the worker sums buffer offsets 1–64 modulo 256 and compares offset 65 against the two's-complement negative of that sum. A mismatch rejects the report. The regular contact loop advances in six-byte records: state/ID byte at record offset 0, X bytes at offsets 1–2, Y at offsets 3–4, and touch-major at offset 5. X and Y are assembled little-endian; the input limits already recovered from the running device match their guarded maxima (`0x4fff` and `0xad3f`). The ID comes from the upper five bits of the state/ID byte and is constrained to ten slots; the low three bits carry the contact state. Codes 1–4 all enter the active-contact path. A zero touch-major value is normalized to 1.

The worker reports `ABS_MT_SLOT` (0x2f), calls `input_mt_report_slot_state(..., active=1)`, then reports X (`ABS_MT_POSITION_X`, 0x35), Y (`ABS_MT_POSITION_Y`, 0x36), and touch-major (`ABS_MT_TOUCH_MAJOR`, 0x30). At frame end it walks the slots not marked present in this report, selects each slot, reports touch-major 0, calls `input_mt_report_slot_state(..., active=0)`, and clears the slot's active marker. It emits `BTN_TOUCH` (EV_KEY code 0x14a) according to whether any valid contacts were processed, then syncs the input frame. This means contact-up is represented by a slot missing from a later frame, not by status code 3 or 4.

When an optional FOD path is enabled, status 3 also emits EV_KEY code `0x152` with value 1, and status 4 emits value 0 if the FOD flag is active. The binary does not name this vendor key code; preserve it as an optional vendor event until the matching input ABI or source confirms its meaning. These codes are not generic touch down/up states.

This is a client-binary finding, not a generic NT38771 specification. The paired helpers and report worker establish the normal-path framing, per-message CS lifetime, checksum description and slot lifecycle, but full FOD integration still needs source/platform correlation. Keep firmware-update and MP-test commands out of an initial XNU touch driver; implement only the normal report-read path and parser after that correlation.

## Firmware files captured from `/vendor/firmware`

All ten matching files were streamed read-only from the phone. Each is 196,608 bytes. Disassembly of `tp_compare_ic` gives the selection map:

- `lcm_panel_id = 0x10` selects TianMa firmware `novatek_ts_fw_tm.bin` and test data `novatek_ts_mp_tm.bin`.
- `lcm_panel_id = 0x30` selects Visionox. Its lockdown byte at offset 5 selects `ggv2` when it is `0x32`; otherwise it selects `weijing`.
- `lcm_panel_id = 0x40` selects CSOT. Its lockdown byte at offset 2 selects `ggv2` for `0x33` or `weijing` for `0x37`; other values take the driver's logged default `weijing` path.

The live FDT reports `lcm_panel_id = 0x10`, so the driver's selected/requested pair for this device is `novatek_ts_fw_tm.bin` and `novatek_ts_mp_tm.bin`. The selected firmware image has `0x29` followed by its `0xd6` complement at offset `0x2f000`, matching the controller's reported FW version `0x29` (decimal 41). This cross-check ties the live module, chosen FDT, on-controller version report, and selected firmware file together.

| File | SHA-256 |
|---|---|
| `novatek_ts_fw_csot_ggv2.bin` | `adae7090412a962fc44ed2e16ce39f34d00204b4cdb7428b169623440d3eb27e` |
| `novatek_ts_fw_csot_weijing.bin` | `b453715738f8b0987210bbb07d067029e0476b12bd0864a2c14f9f0c79c33151` |
| `novatek_ts_fw_tm.bin` | `7722bb9c4f0df6abba2297309ac09036e499956dcc0ee66133a976a2def180fd` |
| `novatek_ts_fw_vox_ggv2.bin` | `1cd48e91b886e0690604479be493cef40fe4a0b7a4babb0f6c1c291e48c176f4` |
| `novatek_ts_fw_vox_weijing.bin` | `d06f1fcd299d7aa4fdf07eac7e345c46f705db11b9b5389230e2cef56acc2a26` |
| `novatek_ts_mp_csot_ggv2.bin` | `ab446428dac4c82ad8d154aa002582bd2ec33f1d75657246ee25b20307c08129` |
| `novatek_ts_mp_csot_weijing.bin` | `521dae07d74ff30e0faee08b0a4ea7dd81a95717dd3fce8cb5e5cf80ee82f4e8` |
| `novatek_ts_mp_tm.bin` | `4b8ca15a8f79519e29d95a7eebe5bf6b6ceea287cb1322cf708b9959db164dce` |
| `novatek_ts_mp_vox_ggv2.bin` | `fd812dbadc28d2a96596bb6fdb1bee186b8f39fe4a0f3e9281424d0a9d88edd0` |
| `novatek_ts_mp_vox_weijing.bin` | `0ff6a52fe2f659fb80d57517dfebbdaf5a95e551945f0f42caf7d04255430295` |

These are firmware/calibration payloads copied from the user's own device for local port research; do not publish or redistribute them. Their inclusion does not make the Linux driver directly usable from XNU.

## Still unresolved

1. Locate the matching NT38771 source and compatible build configuration for this exact Android 16 image, or finish mapping the optional FOD key and its platform consumer. Helper framing, per-message CS lifetime, report length, checksum coverage, contact record layout and normal slot-down/up lifecycle are now known.
2. Implement XNU GPIO/SPI/IRQ and touch-report translation for MT6878. The Linux module and MP-test firmware are reference evidence only.
