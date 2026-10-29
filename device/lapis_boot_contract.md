# Lapis early-boot contract (measured baseline)

This file records facts to use when designing a **host-only** first-stage diagnostic shim. It is not a boot recipe or an image to flash. Values marked runtime came from the rooted phone's active OS3.0.309 boot; values marked stock image came from the matching factory package.

## Stock kernel image

- Android boot header: v4, 4,096-byte pages; `boot.img` partition image is 64 MiB.
- Kernel field: 16,544,915-byte LZ4 legacy stream (`02 21 4c 18`), not Ruby's gzip stream.
- Host decompression yields five LZ4 blocks and a 35,432,960-byte Linux ARM64 Image; its header has `ARM\x64` at offset `0x38`, `text_offset = 0`, `image_size = 0x02270000` (36,110,336 B), and flags `0xa`.
- Embedded kernel build: `6.1.138-android14-11-g51f8c580613d-ab13911623`, clang 17.0.2. This is the stock Android kernel, not XNU.
- Current LK build: `3f2e32c7d_202609200804`. `fastboot boot` with the exact factory image returns `unknown command`; this route has no RAM boot.

## Read-only boot v4 inspector

[`tools/lapis_boot_v4_inspect.py`](../tools/lapis_boot_v4_inspect.py) parses the component boundaries and SHA-256 values of `boot`, `init_boot`, and `vendor_boot` v4 images. It also optionally compares whole files with a factory or slot readback directory. The parser follows the [AOSP boot image header](https://android.googlesource.com/platform/system/tools/mkbootimg/+/refs/heads/android17-release/include/bootimg/bootimg.h); it does not validate AVB signatures, decompress the Linux kernel, or establish that an image is bootable.

The OS3.0.309.0 factory `boot.img`, `init_boot.img`, and `vendor_boot.img` were parsed and compared with the active slot B raw readbacks. All three matched byte-for-byte. The command is:

```sh
python3 tools/lapis_boot_v4_inspect.py \
  --boot lapis_global_images_OS3.0.309.0.WPPMIXM_16.0/images/boot.img \
  --init-boot lapis_global_images_OS3.0.309.0.WPPMIXM_16.0/images/init_boot.img \
  --vendor-boot lapis_global_images_OS3.0.309.0.WPPMIXM_16.0/images/vendor_boot.img \
  --compare-dir device/slot_b_readback --require-identical
```

## Active slot and recovery evidence (read-only, 2026-10-28)

- The attached phone reports Android 16 / SDK 36, build `OS3.0.309.0.WPPMIXM`, active slot `_b`, unlocked bootloader (`ro.boot.flash.locked=0`, `vbmeta.device_state=unlocked`), verified-boot state `orange`, and normal Android boot mode. Root shell is available (`uid=0`, KernelSU context); SELinux is enforcing.
- The current B-slot `boot`, `init_boot`, `vendor_boot`, and `dtbo` readbacks match the supplied OS3.0.309.0 factory images byte-for-byte. Their full partition-sized raw copies and SHA-256 values are recorded in [slot_b_readback/README.md](slot_b_readback/README.md). B vbmeta readbacks are also preserved there.
- This improves rollback evidence but does not make an image experiment safe yet. The LK rejection of `fastboot boot` is still the only tested temporary-boot result. Slot A is an older OS3.0.308.0 installation and has not been boot-tested as a recovery path. No replacement bootloader or boot-partition experiment has been tested.

## Runtime handoff data

- Runtime FDT: [runtime_fdt.dtb](runtime_fdt.dtb); full resource map: [runtime_iomem.txt](runtime_iomem.txt).
- A read-only hash/diff against the connected phone's `/sys/firmware/fdt` (2026-10-29) confirms the hardware declarations used here still match. The live tree differs in boot-profiler timings, the boot-argument prior power-off reason, and battery gauge readings; full hashes and sizes are recorded in [spi3_runtime_snapshot.md](spi3_runtime_snapshot.md).
- The read-only [`tools/lapis_fdt_inspect.py`](../tools/lapis_fdt_inspect.py) summarizes selected CPU, GIC, timer, panel, SPI3/touch, and watchdog nodes using `dtc`. It confirms the eight CPU MPIDRs, GICv3 ranges, 13 MHz architected timer, SPI3 at `0x11013000`, Novatek at chip-select 0 with 9.6 MHz max clock, reset GPIO tuple `<0x8f 0x5a 0x00>`, and IRQ tuple `<0x8f 0x03 0x2001>`. It also warns when SPI3 sibling nodes repeat a chip-select; the runtime tree currently triggers that warning for Novatek and Goodix at CS0. GPIO tuples are reported raw: interpreting flags requires the GPIO-controller binding. The DTB contains three candidate DSI panel nodes; `/chosen/lcm_panel_id = 0x10` and runtime driver evidence identify `p16_36_02_0b_dsc_vdo` as active, so the existence of other panel nodes is not evidence they are connected. This script reports declared properties and does not probe hardware or resolve phandles.
- Runtime FDT RAM declaration: base `0x40000000`, length `0x300000000` (12 GiB). Do not treat the extracted pre-boot candidate DTB's 1 GiB value as live.
- `/proc/iomem` splits System RAM into 32 discontiguous regions totaling 12,262,551,552 B. Secure, modem, APU, firmware, log, and other reservations must be retained until their owner/boot protocol is understood.
- The runtime kernel's `/proc/iomem` range starts at `0x40010000`; this is a Linux runtime resource range, not a proven LK load address for a replacement image.
- Live LK framebuffer: base `0xfd72f000`, size `0x028d0000` (42,795,008 B), same as `/reserved-memory/mblock-15-framebuffer`.
- LK framebuffer metadata gap: runtime `/chosen` has a 64-byte `atag,videolfb` blob, the separate base-low/base-high properties, and `atag,videolfb-vramSize = 0x028d0000`; it does not expose a parsed stride or pixel depth in the DTB. Android also has no `/proc/fb` or `/sys/class/graphics`. The active `lk_b` image contains both 16-bit and 32-bit ARGB8888 fill helpers and a bits-per-pixel dispatch warning, but static strings alone do not establish which format LK selected. Do not write diagnostic pixels until stride, depth, and byte order are recovered.
- Runtime CPU/interrupt data: eight CPUs use PSCI. MPIDRs are `0x0`, `0x100`, `0x200`, `0x300`, `0x400`, `0x500`, `0x600`, `0x700`; live Linux reports two clusters of four, with CPU0–3 MIDR part `0xD05` and CPU4–7 part `0xD41`. GICv3 distributor is `0x0c000000`/`0x40000`, redistributor range `0x0c040000`/`0x200000`; architected timer frequency is 13 MHz. This identifies the current Linux boot CPU and runtime interrupt map, not LK's replacement-kernel handoff behavior or proven XNU SMP support.
- Display: panel `p16_36_02_0b_dsc_vdo`, 1280×2772, 120/90/60/30 Hz. Panel reset GPIO 90, VDDI GPIO 149, ESD IRQ GPIO 12; regulators `mt6369_vtp` and `mt6363_vcn13`.
- Active Novatek SPI3 touch node also uses reset GPIO 90 (same `pio` phandle), IRQ GPIO 3, and max SPI clock 9.6 MHz. Treat GPIO 90 as a shared panel/touch reset line. The runtime FDT also contains a sibling Goodix child on CS0; connected-phone sysfs enumerates only `spi3.0` bound to Novatek. Both DT children specify `reg = <0>` and Novatek is listed first, so a duplicate chip-select rejection by SPI core is the leading explanation for Goodix's absence (not directly confirmed in this vendor kernel's logs). The absent `status` property means the node is ordinarily considered available. The SPI3 parent advertises `gt9896s_*` pinctrl states for GPIO6/7 despite the live Novatek child declaring GPIO90/3; state selection/ownership is unresolved, so those labels alone do not show that GPIO6/7 serve the live touchscreen. See the dated read-only follow-up in [spi3_runtime_snapshot.md](spi3_runtime_snapshot.md).
- Live touch driver is Novatek NT38771 (`NVT-ts`) on `spi3.0`, input `/dev/input/event5`; input axes are 0–20,479 and 0–44,351 (16× native 1280×2772). Runtime `lcm_panel_id = 0x10` selects TianMa firmware; read-only `/proc` status reports FW `0x29`, X/Y matrix 18×39. The raw `tp_lockdown_info` and selected firmware hashes are in [touch_module_analysis.md](touch_module_analysis.md).
- Runtime watchdog register resource is `0x1c00a000–0x1c00a0ff` (`mediatek,mt6878-wdt`), confirmed in both FDT and `/proc/iomem`. Ruby's `0x10007000` watchdog sequence is a different SoC address and must not be copied.
- The active panel's Linux module was extracted read-only from factory `vendor_boot.img` to `panel-p16-36-02-0b-dsc-vdo.ko` (SHA-256 `fa17d9db592defc31943fdac21944cc96fb86614cf456fa40c3257dad0b25015`). ELF metadata exposes `lcm_panel_init`, `lcm_get_modes`, `panel_init_power`, and four mode objects. This is an input for reimplementing the DSI sequence, not code that XNU can load directly.
- The active `nt38771.ko` and ten touch firmware/MP-test variants were streamed read-only from `/vendor_dlkm` and `/vendor/firmware`; module and payload hashes are in [touch_module_analysis.md](touch_module_analysis.md). Do not load Linux modules in XNU or run the factory MP self-test during passive analysis.

## Porting constraints

- The Ruby shim's gzip packer, fixed framebuffer, RAM size, stack/load assumptions, and watchdog constants are device-specific. Only its ARM64 entry/FDT parsing ideas are candidates for adaptation after review.
- The live DTB and memory map are a measurement of Android's current handoff. A custom bootloader may hand off differently; any replacement must establish its own safe RAM map and preserve reserved ranges.
- No MMIO register has been written by this audit. No custom kernel or shim has been built or booted. The `fastboot boot` probe was RAM transfer only and failed before execution.
- Slot A is OS3.0.308.0, not a verified backup of the current OS3.0.309.0 slot. Raw readbacks and hashes are in `slot_a_readback/` and `slot_b_readback/`.
