# Lapis SPI host module: stock binary analysis

**Status: read-only static analysis.** The file beside this report was extracted from the supplied stock `vendor_boot.img`, not copied from another device. Do not redistribute it or attempt to load it into XNU.

## Identity and match

- Extracted file: [`spi-mt65xx.ko`](spi-mt65xx.ko), 58,144 bytes, SHA-256 `4c131ccaa16b738b73cca89b35d41eb93b652d2eda7426ea677b270c12f612c5`.
- ELF: AArch64 `ET_REL`; module name `spi_mt65xx`; description `MTK SPI Controller driver`; GPL v2; vermagic `6.1.138-android14-11-ga77ed0ba4c27`.
- Its `.modinfo` explicitly includes `alias=of:N*T*Cmediatek,mt6989-spi` (and wildcard form). The live SPI3 DT node uses `mediatek,mt6989-spi`, and live sysfs reports platform driver `mtk-spi`; `/proc/modules` lists `spi_mt65xx`. This is direct evidence the exact stock SPI host module recognizes the device's compatible.
- `reference/ramdisk/modules.load` loads `spi-mt65xx.ko`; the binary came from the normal/platform vendor-ramdisk fragment. Its compile-string table embeds `../kernel_device_modules-6.1/drivers/spi/spi-mt65xx.c`, a useful lead for locating the source release.

## Compatible-data evidence

The module retains static ELF symbols for `mtk_spi_of_match`, `mt6989_compat`, probe, transfer, interrupt, chip-select, runtime suspend/resume, and SPI-memory routines. The `mt6989_compat` bytes are `01 00 00 01 01 01 01 01 01`. A public Motorola MTK `kernel_device_modules-6.1` source candidate defines nine booleans in the same order and assigns MT6989: `need_pad_sel=true`, `must_rx=false`, `must_tx=false`, `enhance_timing=true`, `dma_ext=true`, `ipm_design=true`, `support_quad=true`, `sw_cs=true`, `enhance_packet_len=true`. This matches the extracted binary's raw bytes exactly; the candidate source is not yet proven to be the exact Xiaomi build revision.

Strings and symbols show support paths for:

- pad selection and `mediatek,tickdly` / `clock-source-type` DT properties;
- TX/RX PIO and DMA transfers, host IRQ completion, and CS timing;
- runtime clock management and optional SPI memory operations;
- a `spi_log` sysfs control, which reports “Now spi log disabled.” on the live phone.

The extracted module's undefined-symbol table contains no pinctrl lookup/select API, and its strings contain no `gt9896s_*` state labels. That is evidence the SPI host driver itself does not explicitly select those optional named states. The pinctrl core may still apply the host's `default` state automatically; this binary check does not prove which mux state is currently active, and it does not rule out another driver selecting a state. The `gt9896s_*` entries therefore remain unexplained DT metadata, not evidence of the live Novatek IRQ/reset wiring.

The stock module's retained ELF symbols include `mtk_spi_probe`, `mtk_spi_set_cs`, `mtk_spi_prepare_message`, `mtk_spi_transfer_one`, `mtk_spi_can_dma`, `mtk_spi_setup`, `mtk_spi_set_hw_cs_timing`, `mtk_spi_interrupt`, `mtk_spi_prepare_transfer`, DMA-address setup, SPI-memory operations, and runtime PM callbacks. Its debug strings also name `mtk_spi_fifo_transfer` and `mtk_spi_dma_transfer`. These match the major entry points and transfer paths in the source candidate. The stock module has no separate local symbol entries for those two transfer helpers, so their detailed binary control flow still needs disassembly/relocation tracing rather than inference from the names alone.

The lapis FDT supplies `mediatek,pad-select = <0>`, `mediatek,tickdly = <2 0>`, and `clock-source-type = "univpll"`. These values line up with the module's observed property names. The source candidate's register map and MT6989 mode flags are now correlated with the binary, but its exact source revision and lapis runtime SPI mode still need confirmation before treating these details as a final XNU register specification.

## Candidate transfer behavior and implications

Reading the Motorola candidate's `mtk_spi_fifo_transfer`, `mtk_spi_dma_transfer`, `mtk_spi_prepare_transfer`, and interrupt paths gives a more concrete implementation lead:

- The PIO path moves data through the TX/RX data registers in chunks bounded by a 32-byte FIFO, then checks controller status and waits for transfer completion. It is a viable initial implementation target only after confirming the same behavior in the stock module.
- The DMA path programs source/destination addresses and uses the extended address registers when required. The candidate selects DMA through `mtk_spi_can_dma`; exact DMA eligibility, address constraints, and integration with XNU's DMA/IOMMU services still need to be recovered from the stock binary and platform.
- MT6989's matched compatibility data says `sw_cs=true`, `enhance_packet_len=true`, and `ipm_design=true`. The candidate therefore controls chip select through command state, extends packet length using CFG1/CFG5, and uses IPM timing fields including CFG2. XNU must preserve CS across any multi-packet transfer according to the client transaction semantics; an implementation that blindly toggles CS for every 32-byte FIFO chunk could break a touch command.
- The candidate's prepare-transfer path derives timing from the SPI clock and requested speed; its hardware-init path programs mode, bit order, interrupt, pad and tick-delay state. The live FDT's 9.6 MHz client limit and `univpll` source identify constraints, but do not establish the exact runtime clock rate or active SPI mode.

The extracted Xiaomi stock binary has now been disassembled for these paths. Direct binary evidence confirms a PIO chunk limit of `0x20` bytes, TX FIFO writes at base `+0x10`, RX FIFO reads at `+0x14`, DMA address writes at `+0x08`/`+0x0c`, optional upper-address writes at `+0x2c`/`+0x30`, status reads at `+0x1c`, and command-register accesses at `+0x18`. The DMA eligibility helper requires at least `0x21` bytes and 4-byte alignment of each present TX/RX buffer. The interrupt handler reads status at `+0x1c`, tests bit 1, advances transfer pointers, and arranges further work/completion. These offsets align with the candidate register names and map above, so the offset correlation is now strong and based on the exact stock binary.

Probe disassembly and ELF relocations also confirm the binary maps the platform MMIO resource, gets the platform IRQ and requests a threaded IRQ, reads device-tree properties, obtains three clocks, sets a clock parent, reads a clock rate, configures a DMA mask, and enables runtime PM/clock preparation before controller registration. This supports the observed FDT contract (one host IRQ, three clock names, pad/timing properties), but does not by itself establish the provider's clock rates or the correct XNU clock-tree implementation.

The clock/timing path can now be narrowed further. Stock `mtk_spi_prepare_transfer` reads and writes `base + 0x28` when compatibility byte 3 (`enhance_timing`) is set, and derives equal SCK high/low fields from the requested rate and stored SPI clock rate. This directly confirms CFG2 is the active timing register for this compatible. The prepare-message path reads/writes `base + 0x18` and the pad path uses `base + 0x24`; the CS callback also manipulates command state at `+0x18`. These accesses match the candidate's CMD/PAD/CFG2 naming. Candidate source interprets CMD bits for phase/polarity, bit order, software CS and tick delay, but exact active lapis mode remains unknown because the child DT does not declare it.

The candidate's probe sequence names the three clocks `parent-clk`, `sel-clk`, and `spi-clk`, sets `sel-clk`'s parent to `parent-clk`, and gets the rate from `spi-clk`; it treats `clock-source-type="mainpll"` specially and otherwise leaves the default/univpll path. Lapis says `univpll`, so this source points to that default path. The actual provider rates and enabled-state behavior still need runtime or matching Xiaomi source evidence.

The stock binary confirms offsets and several control-flow properties, but the C source used to interpret each bitfield is still the Motorola candidate; exact bit semantics and probe/init sequencing have not all been independently reconstructed from the stripped/relocatable binary. The next static step is focused relocation and constant tracing around probe and prepare-message/init, then compare those masks with the candidate definitions. Do not implement unverified MMIO writes based only on this section.

## Source comparison

The Xiaomi `lapis-v-oss` branch's copy of `drivers/spi/spi-mt65xx.c` has no `mt6989-spi` match entry. The stock binary's embedded compile path points to the separate `kernel_device_modules-6.1` tree. A public [Motorola source candidate](https://github.com/MotorolaMobilityLLC/kernel-kernel_device_modules-6.1/blob/android-14-release-u3uc34.63-31-6/drivers/spi/spi-mt65xx.c) in that tree has the exact MT6989 compatible entry and flag bytes and is a strong register/source reference, but its Android 14 release branch is not proven to be the exact Android 16/Xiaomi source revision.

## Candidate register map from matching MT6989 source

These offsets are from the Motorola source candidate and the same driver family; they are suitable for source-to-binary correlation, not yet a final XNU programming guide.

| Offset | Candidate register | Function |
| ---: | --- | --- |
| `0x00` | `SPI_CFG0` | SCK high/low and chip-select setup/hold timing |
| `0x04` | `SPI_CFG1` | CS idle, packet loop/length and tick delay |
| `0x08` / `0x0c` | `SPI_TX_SRC` / `SPI_RX_DST` | 32-bit DMA addresses |
| `0x10` / `0x14` | `SPI_TX_DATA` / `SPI_RX_DATA` | FIFO data |
| `0x18` | `SPI_CMD` | activate, resume, reset, pause/deassert, sample select |
| `0x1c` / `0x20` | `SPI_STATUS0` / `SPI_STATUS1` | transfer and interrupt status |
| `0x24` | `SPI_PAD_SEL` | select SPI pad group |
| `0x28` | `SPI_CFG2` | IPM SCK high/low timing |
| `0x2c` / `0x30` | `SPI_TX_SRC_64` / `SPI_RX_DST_64` | upper DMA address words |
| `0x40` | `SPI_CFG3` | extended controller configuration |
| `0x68` | `SPI_CFG5` | packet-length extension on IPM designs |

The live host resource is `0x11013000–0x110130ff`, so these candidate offsets fit inside its mapped `0x100` window. Confirm the registers that the exact stock binary touches and the applicable MT6989/IPM bit definitions before implementing any MMIO writes.

## Consequence for XNU work

The exact stock controller binary and a strongly matching public source candidate are now in hand. Disassembly of the stock binary confirms the candidate's key register offsets and 32-byte PIO chunking, DMA address registers, status location and IRQ completion path; the compatible bytes, symbols and debug strings also correlate. Remaining source-to-binary work is focused on probe/init register masks, timing semantics and any Android 16/Xiaomi deltas. Then translate validated host-transfer, clock, IRQ and CS behavior into an XNU SPI host. Keep NT38771 protocol/frame parsing as its own client-driver stage.
