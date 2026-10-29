# Lapis SPI3 / NT38771 XNU bring-up contract

**Status: hardware contract only; no XNU driver implementation or verified controller register map yet.** This file records facts a future XNU SPI host and touch client must honor. It is not a bootable driver specification.

## Confirmed platform contract

| Item | Value | Evidence / qualification |
| --- | --- | --- |
| SoC | MediaTek MT6878 | Runtime pinctrl compatible is `mediatek,mt6878-pinctrl`. |
| SPI host | SPI3 at `0x11013000`, size `0x100` | Runtime FDT `/soc/spi3@11013000`; node compatible is literally `mediatek,mt6989-spi` despite the MT6878 SoC. Preserve the observed string; do not infer controller revision from the name alone. |
| Stock host driver | `mtk-spi` / module `spi_mt65xx` | Live sysfs and `/proc/modules`; exact stock `spi-mt65xx.ko` is extracted from supplied vendor_boot at [`spi-mt65xx.ko`](spi-mt65xx.ko), and its module alias includes `mediatek,mt6989-spi`. Static analysis is in [spi_mt65xx_module_analysis.md](spi_mt65xx_module_analysis.md). |
| Source lead | MTK `kernel_device_modules-6.1` SPI implementation | Public Motorola source candidate matches the stock binary's MT6989 compatible and nine compatibility bytes; exact Xiaomi/Android 16 revision remains unconfirmed. Stock-binary disassembly now confirms 32-byte PIO chunking, FIFO offsets, DMA address offsets, status offset and IRQ status-bit handling. Candidate still supplies much of the bitfield interpretation. Details and caveats are in [spi_mt65xx_module_analysis.md](spi_mt65xx_module_analysis.md). |
| Host interrupt | GICv3 IRQ 494 | Runtime FDT raw interrupt cells `<0 462 4 0>`; live `/proc/interrupts` names `GICv3 494 Level 11013000.spi3`. |
| Clock references | Raw cells `<66 124 66 17 64 12>` | Runtime FDT names them `parent-clk`, `sel-clk`, `spi-clk`. Stock probe confirms three clock lookups, sets `sel-clk`'s parent to `parent-clk`, and reads `spi-clk`'s rate. Lapis selects `univpll` in `clock-source-type`; provider rates and runtime enable state remain unknown. Stock `mtk_spi_prepare_transfer` confirms CFG2 (`+0x28`) timing writes for this compatible. |
| Touch child | `novatek,NVT-ts-spi`, CS0 (`reg = <0>`) | Runtime FDT `/soc/spi3@11013000/novatek@0`, live bound as `spi3.0` / `NVT-ts`. |
| Touch SPI settings | CS0, 8-bit words, mode 0; Linux request defaults to 9.6 MHz | CS and `spi-max-frequency` come from runtime FDT. Active driver disassembly sets mode 0 / 8 bits and leaves transfer speed unset; the matching Linux SPI core defaults that to the device maximum of 9.6 MHz. Effective SCK after controller divider rounding and the source clock rate remain unknown. |
| SPI3 pins | GPIO68=CLK, GPIO69=CSB, GPIO70=MOSI, GPIO71=MISO, mux function 2 | Runtime pinctrl state has encodings `0x4402`, `0x4502`, `0x4602`, `0x4702`. Function names are independently identified in the [MT6878 pin-function table](https://codebrowser.dev/linux/linux/arch/arm64/boot/dts/mediatek/mt6878-pinfunc.h.html#490). |
| Touch IRQ | EINT/GPIO3, edge-triggered | Touch child declares `novatek,irq-gpio = <&pio 3 0x2001>`; live `/proc/interrupts` says `mtk-eint 3 Edge NVT-ts`. Preserve raw DT flags until the matching Xiaomi binding interprets them. |
| Touch reset | GPIO90, active level/sequence unresolved | Touch child declares `novatek,reset-gpio = <&pio 90 0>`; active panel also uses GPIO90, so reset ownership/ordering must be coordinated. |
| Active controller | Novatek NT38771, TianMa module | Read-only `/proc/tp_info` and `/proc/nvt_fw_version`; active FW is `0x29`, matrix 18×39. Active panel id `0x10` selects `novatek_ts_fw_tm.bin`. |
| Touch reports | 10 slots; X 0–20479, Y 0–44351 | Live input device; ranges are 16× the 1280×2772 native panel dimensions. |
| Android host health | 2,338 synchronous transfers; zero errors/timeouts | Read-only Linux SPI-core counters in [spi3_runtime_snapshot.md](spi3_runtime_snapshot.md). Confirms Android's host/client path has been working in this session; it is not evidence of XNU compatibility. |

## Required XNU layering

1. A platform SPI3 host owns the `0x11013000–0x110130ff` register window, host GIC IRQ 494, pinctrl and the three named clocks. It exposes queued full-duplex transfers without leaking MediaTek register details to clients.
2. An NT38771 SPI client targets CS0, negotiates at or below 9.6 MHz, uses GPIO3 for edge-driven service, and owns GPIO90 reset through a shared reset coordinator with the display.
3. The active client uses synchronous register SPI helpers: read clears command bit 7 and copies payload after two leading receive bytes; write sets bit 7; both retry failed messages up to five times. Each helper submits one message with `cs_change` false, so CS remains asserted for that message and releases between separate helper calls. The normal worker reads 385 bytes and parses a 384-byte report area. It checks the two's-complement checksum over report offsets 1–64 against offset 65, then decodes six-byte contact records with little-endian X/Y, touch-major, ten IDs and a three-bit state field. All valid codes 1–4 mark an active slot; absent slots are explicitly released at frame end and BTN_TOUCH is updated from the active-contact count. Optional statuses 3/4 also toggle EV_KEY `0x152` for the FOD path; its platform meaning is unresolved. Do not send firmware-update or MP-test payloads as part of initial bring-up.
4. Translate validated touch frames into XNU's multitouch/HID input path only after frame format, checksum, slot encoding, contact lifecycle and coordinate scaling are established.

## Parser implementation checkpoint

[`nt38771_report_parser.c`](nt38771_report_parser.c) and its header contain a host-independent parser draft for the recovered 384-byte normal report area. It validates the report checksum, skips invalid individual contacts like the stock client, and returns active records plus a present-slot mask. [`nt38771_touch_state.c`](nt38771_touch_state.c) converts raw 16x coordinates into native 1280×2772 pixels and emits down/update events plus releases for slots absent from the next frame through a callback. [`nt38771_pipeline.c`](nt38771_pipeline.c) connects frame validation to slot updates and accepts the callback from its caller; it has no dependency on a particular XNU HID implementation. [`nt38771_sweet_hid_glue.c`](nt38771_sweet_hid_glue.c) remains an optional adapter to the Ruby reference's `sweet_hid_touch(finger, down, x, y)` API. The pipeline consumes the 384-byte payload at `CTP_SPI_READ` buffer offset 1. It does not handle touch-major or the optional FOD key. These files have no SPI, GPIO, MMIO or allocation code; they passed a host C syntax compile but are not integrated or linked into XNU. The next integration step is an XNU-side callback for the chosen 8796 HID/input path.

## Open issues before coding the host driver

- No exact Xiaomi MT6878/`mt6989-spi` controller source or public register programming guide has been found in this workspace, although the exact stock driver binary is now recovered. I checked the official `lapis-v-oss` branch's `drivers/spi/spi-mt65xx.c`: it contains generic MediaTek SPI code, but its compatible table does not match `mediatek,mt6989-spi`; the stock binary's embedded compile path points instead to `kernel_device_modules-6.1/drivers/spi/spi-mt65xx.c`. Generic/upstream code is reference material, not proof that this Android controller uses identical timing/register behavior.
- The SPI controller node includes `gt9896s_*` pinctrl states for GPIO6/7, while its live child names GPIO3/90 for IRQ/reset. `/proc/interrupts` confirms GPIO3 is the active IRQ, but `/sys/kernel/debug` is not mounted, so the GPIO6/7 states' runtime role is unknown. Do not toggle GPIO6/7 for touch.
- Read-only FDT data does not state SPI mode or word width. Avoid assuming mode 0 or issuing trial transfers; first recover the exact runtime configuration from source or the bound driver's state.
- The NT38771 interrupt protocol, register map, frame format, reset delays and firmware-update sequence are not yet recovered. Keep this as a separate milestone from host-controller support.
- Any first device boot needs a safe boot path. This phone rejects `fastboot boot`; do not test a new kernel by writing a boot-critical partition until a separately verified recovery path exists.

## GPIO bit-bang fallback: what the public MT6878 pinctrl source establishes

The upstream MT6878 pinctrl implementation gives a useful *layout reference* for the GPIO block: GPIO base `0x10005000`; mode fields begin at offset `0x300`, DIR at `0x0`, DO at `0x100`, and DI at `0x200`, with the pin-field calculator handling register stride and bit position. Applying those formulas to pins 68–71 yields mode register `+0x380` at bit positions 16/20/24/28, DIR `+0x20` bits 4–7, DO `+0x120` bits 4–7, and DI `+0x220` bits 4–7. This is a derivation from upstream's MT6878 tables, not confirmation against the Xiaomi Android 16 pinctrl driver or a license to write these registers from XNU.

The pin-function table independently maps GPIO68–71 to SPI3 CLK/CSB/MOSI/MISO on function 2. A GPIO bit-bang transport could therefore be explored as a way around the still-incomplete SPI clock tree, but it still needs the exact lapis pinctrl ownership/drive semantics, a safe CS policy, and proof that the XNU display/touch code can coordinate those pins. GPIO90 remains shared with the active display and is out of scope. No bit-bang transport code is added until those semantics are corroborated against the stock platform and an XNU baseline is available.

## Safe development sequence

1. Obtain the matching vendor SPI host/NT38771 source and clock/pinctrl bindings, or document the gaps with exact register evidence from a trusted source.
2. Finish correlating probe/init masks and timing fields against the candidate source; transfer, PIO/DMA address, status and IRQ register offsets have been checked against stock-binary disassembly.
3. Build host-side parsers for runtime FDT and captured Linux input/interrupt evidence; use synthetic data to validate coordinate and slot translation.
4. Implement the XNU SPI abstraction with a mock backend first. Add hardware operations only after register semantics, reset ownership and IRQ polarity are source-confirmed.
5. Add NT38771 frame parsing from captured or documented protocol data; validate malformed frames and contact-up/down behavior offline.
6. Only after the XNU tree, boot chain and recovery procedure are buildable and reviewed, plan a separately authorized device boot experiment.

No device state was changed while preparing this contract. No XNU code was built and no hardware transfer was sent.
