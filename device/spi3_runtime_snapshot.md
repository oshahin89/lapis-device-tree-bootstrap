# Lapis SPI3 runtime snapshot

Read-only capture during the current Android session. No reset, reboot, write, SPI transfer, or device configuration change was performed for this capture.

## Build and boot identity

| Property | Value |
| --- | --- |
| Product / model | `lapis` / `25080RABDG` |
| Build fingerprint | `Redmi/lapis_global/lapis:16/BP2A.250605.031.A3/OS3.0.309.0.WPPMIXM:user/release-keys` |
| Android release / SDK | `16` / `36` |
| Vendor SDK property | `34` |
| Active slot | `_b` |
| Running kernel | `6.1.138-android14-11-g51f8c580613d-ab13911623`, AArch64 |

The Android platform and vendor SDK properties differ (`36` and `34` respectively); this is recorded as reported, without treating the difference alone as a compatibility failure.

## SPI3 host and client

- Host platform device: `/sys/devices/platform/soc/11013000.spi3`, driver `mtk-spi`, FDT compatible `mediatek,mt6989-spi`.
- SPI module: `spi_mt65xx` is present in `/proc/modules` during this session; the stock module extracted from the supplied `vendor_boot.img` has the matching OF alias.
- Child device: `spi3.0`, bound to `NVT-ts`; FDT child is `novatek,NVT-ts-spi`, chip select 0, max frequency 9.6 MHz.
- `/proc/interrupts` reports touch `mtk-eint 3 Edge NVT-ts` and host `GICv3 494 Level 11013000.spi3`.

## Read-only SPI core statistics

| Counter | Value |
| --- | ---: |
| bytes | 935,138 |
| bytes_tx | 935,138 |
| bytes_rx | 148,112 |
| messages | 2,338 |
| transfers | 2,338 |
| spi_sync | 2,338 |
| spi_sync_immediate | 2,338 |
| spi_async | 0 |
| errors | 0 |
| timedout | 0 |
| transfers_split_maxsize | 0 |

Non-zero transfer-size histogram buckets: 2–3 bytes: 565; 4–7: 315; 8–15: 1; 32–63: 20; 64–127: 1,328; 256–511: 18; 2,048–4,095: 34; 8,192–16,383: 57. These are cumulative kernel counters as observed; they are evidence of normal Android SPI use, not a protocol trace or an XNU test.

## Access limitations

The session has KernelSU root, but some sysfs files return `Permission denied` under the active SELinux domain. `/sys/kernel/debug` exists but exposes no entries, so pinctrl debug state is unavailable. The SPI driver's `spi_log` interface was readable earlier and reported logging disabled; subsequent reads were denied. No SELinux policy changes or mount operations were attempted.
