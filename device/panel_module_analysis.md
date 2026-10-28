# Lapis active panel module analysis

This is a host-side, read-only analysis of `panel-p16-36-02-0b-dsc-vdo.ko`, extracted from the supplied factory `vendor_boot.img`. The module SHA-256 is `fa17d9db592defc31943fdac21944cc96fb86614cf456fa40c3257dad0b25015`. The phone was not rebooted and the module was not loaded or executed.

## Mode timings recovered from ELF data

The module is an AArch64 relocatable ELF (`ET_REL`). I decoded the four exported mode objects as little-endian DRM display-mode records and checked refresh as `clock_kHz × 1000 ÷ (htotal × vtotal)`.

| Mode object | Pixel clock | Active size | Horizontal display / sync start / sync end / total | Vertical display / sync start / sync end / total | Calculated refresh |
|---|---:|---:|---|---|---:|
| `performance_mode_2` | 481,198 kHz | 1280×2772 | 1280 / 1384 / 1388 / 1408 | 2772 / 2828 / 2830 / 2848 | 120.00 Hz |
| `performance_mode_1` | 479,001 kHz | 1280×2772 | 1280 / 1384 / 1388 / 1408 | 2772 / 3760 / 3762 / 3780 | 90.00 Hz |
| `default_mode` | 481,198 kHz | 1280×2772 | 1280 / 1384 / 1388 / 1408 | 2772 / 5676 / 5678 / 5696 | 60.00 Hz |
| `performance_mode_3` | 262,642 kHz | 1280×2772 | 1280 / 3050 / 3054 / 3074 | 2772 / 2828 / 2830 / 2848 | 30.00 Hz |

These four rates agree with the live display modes captured from Android. The unusual blanking values in the 60 Hz and 30 Hz records are kept exactly as encoded; do not “correct” them based on assumptions. Treat these as timing data recovered from the vendor's Linux module, not proof that an XNU display controller can use them unchanged.

## Initialization evidence

The `lcm_panel_init` function calls the Linux MIPI-DSI generic-write and DCS-write helpers with payloads in the module's `.rodata` section. Reconstructed bytes show:

- Vendor command-page unlocks beginning `F0 55 AA 52 08` and several page values (`00`, `01`, `03`, `04`, `08`).
- Vendor-specific register writes, including `CB`, `EA`, `C3`, `C7`, `BB`, `B5` and associated payloads. Preserve the exact order if translating these into a new driver.
- Standard DCS `0x11` (sleep out) followed by `0x29` (display on) as the final one-byte writes in the init path.
- DCS writes for brightness (`51 00 00` at the init defaults), tearing/scan settings and other vendor tuning values.

The payload extraction relied on `.rela.text` relocations to pair each DSI call with its `.rodata` address and the encoded write length. Since this is an unlinked ELF module, raw disassembly alone does not resolve its addresses; the relocations are part of the evidence. No panel command was sent to the device.

## Port implications

1. The XNU port can use these timings and ordered command payloads as a reference for a panel driver.
2. The panel still needs a lapis-specific MIPI-DSI host/controller path, correct D-PHY setup, and the verified panel power sequence. Linux helper calls are not an XNU implementation.
3. Runtime FDT evidence shows panel and Novatek touch share reset GPIO 90. Any future init sequence must coordinate that line and the panel's GPIO/regulator dependencies documented in [lapis_boot_contract.md](lapis_boot_contract.md).
4. Do not try to load this Linux module under XNU or copy vendor initialization commands into an experimental device boot without a known recovery plan.
