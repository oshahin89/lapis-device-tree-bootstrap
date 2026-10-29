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

An offline reinspection of the exact runtime FDT found an additional `/soc/spi3@11013000/goodix@0` sibling at the same CS0. It has no `status` property, while the Novatek node at CS0 explicitly says `status = "ok"`. The captured live SPI enumeration has only `spi3.0` bound to `NVT-ts`; it does not include a full device-tree-to-SPI registration trace, so the Goodix node's omission or rejection mechanism is not established. The same FDT's SPI host advertises legacy `gt9896s_*` pinctrl states for GPIO6/7. These are now tracked as source-DTB inconsistencies, not as runtime Goodix or touch wiring evidence.

## Follow-up capture (2026-10-29, read-only)

The phone was connected over ADB and the refreshed [`tools/lapis_spi_runtime_audit.sh`](../tools/lapis_spi_runtime_audit.sh) queried sysfs, the live firmware DT, `dmesg`, and `/proc/interrupts`. It confirmed:

- Live identity remains `lapis`, OS3.0.309.0.WPPMIXM, active slot `_b`. The phone's readable `/sys/firmware/fdt` is 593,991 bytes with SHA-256 `e7d4f74c154f3517c98c3678c4338bd5a90fd684e2ec869d2916eb520bfed120`. The checked-in capture is 593,995 bytes with SHA-256 `4f49378e11ac3306012e63d631c9c80f1fe7ec3fa5940b63c65f4fc7cfa0cc37`. Decompiling and diffing both trees showed only nine changed property lines: boot-profiler timings, prior power-off reason (`Cold_reset` in saved capture versus `AP_WDT` in live bootargs), and fuel-gauge values. CPU, GIC, timer, panel, SPI3, touch and pinctrl declarations matched. The power-off reason is recorded as a raw boot argument; its cause and timing are not inferred.

- `/sys/bus/spi/devices/spi3.0` is bound to `/sys/bus/spi/drivers/NVT-ts`; its `OF_FULLNAME` is `/soc/spi3@11013000/novatek@0` and modalias is `spi:NVT-ts-spi`.
- There is no `spi3.1` device in the enumerated SPI clients. The live firmware DT still contains `/soc/spi3@11013000/goodix@0`, with compatible `goodix,gt9896s` / `goodix,nottingham`, `reg = <0>`, and no `status` property.
- The captured `/proc/interrupts` values were 65,081 for `mtk-eint 3 Edge NVT-ts` and 65,496 for `GICv3 494 Level 11013000.spi3`; these show both paths are active, but are cumulative counters, not per-touch measurements.
- The pinctrl provider sysfs lists `consumer:platform:11013000.spi3`, establishing that the SPI host is linked to the MT6878 pinctrl provider. `/sys/class/pinctrl`, `/sys/kernel/debug/pinctrl`, and the SPI device's `pinctrl` directory are absent, so the currently selected mux state cannot be read through exposed sysfs/debugfs.
- The bounded `dmesg` search returned no SPI/touch probe lines in the current ring buffer. A broader search returned unrelated audio `tfa98xx` lines containing the word “goodix”; those are not touch-driver evidence.

The most likely explanation is a duplicate-CS collision in SPI core: the runtime DT lists Novatek first and Goodix second, and both have `reg = <0>`; generic Android/Linux SPI core iterates available DT children, while its device-registration path rejects a second client using an already-claimed chip-select. The absent `status` property does not disable Goodix: OF treats an absent status as available. This matches the live result (Novatek exists, Goodix does not), but the exact Xiaomi kernel source/logs for the failed registration were not available in this capture, and the current `dmesg` ring contains no matching error; treat the collision as a strong inference rather than a directly observed error. See [Android common SPI enumeration and duplicate-CS handling](https://android.googlesource.com/kernel/common/+/09eed5a1ed3c752892663976837eb4244c2f1984/drivers/spi/spi.c#1897) and [OF availability semantics](https://android.googlesource.com/kernel/common/+/0644f5393e915f13733bcc65f13195ff39aeb63e/drivers/of/base.c#274).

No reset, reboot, write, SPI transfer, or other device configuration was performed. Re-run the script for a fresh snapshot; it reports enumerated SPI clients, host binding, Goodix DT status, probe-log matches, and interrupt counters.

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
