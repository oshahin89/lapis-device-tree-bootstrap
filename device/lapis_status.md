# Lapis iOS-port status (updated 2026-11-19)

**Latest user-reported milestone:** the iOS boot reached the lock screen
(Run 6; see [the run log](lapis_ufs_cold_bringup_plan.md#49-run-6-reached-the-ios-lock-screen)).
The previous recorded run showed the Apple logo and data-migration progress
(Run 5). No image hash or detailed log for Run 6 is recorded yet.

## Working on the phone (each run flashed to `boot_b`, restored afterwards)

| Milestone | Image | Evidence |
|---|---|---|
| Framebuffer measured: ARGB8888, buffer 0 `0xfd72f000`, stride 5120 | probe `a66e4e30…` | screen pattern |
| TOPRGU watchdog disable; Power + Vol-down still recovers | probe `e4c7d108…` | no resets for 2 min |
| LK → `tools/lapis_shim` → xnu 8796, up to `Waiting on <IOMedia Apple_HFS>` | `5bb385fd…` | screen (wrapped console) |
| Userspace PID 1 from a RAM disk (`rd=md0`, mockfs) | `393c3cd3…` | `init running as PID 1`, heartbeat |
| SMP: 4× A55 | `1d6cbc1f…` | `4/4 CPUs initialized` |
| SMP: 4× A55 + 4× A78 | `adc0d35d…` | `8/8 CPUs initialized` |
| HFS+ RAM-disk root (`SweetRoot`) | `468dc8fd…` | `hfs: mounted SweetRoot`, PID 1 |
| UFS host state read at LK's jump | probe `407f270d…` | IP_VER `0x10420200` = MT6878, refclk on, HCE = 0 |
| Return to fastboot without keys (reboot mode 3 + PSCI `SYSTEM_RESET`) | probe `2b06b551…` | back in fastboot after ~32 s |
| xnu panic → automatic return to fastboot (patch 0083, `lapispanicreboot=N`) | `04f5aae0…` | back in fastboot after ~59 s |
| NT38771 touch controller answers over bit-banged SPI3 | probe `b864fef0…` | chip ID `71 87 03` |
| iOS XNU kernel boots on HyperOS 4 (Android 17), RAM-disk root, PID 1 + heartbeat | `7a9b66be…` | `init running as PID 1`, `still alive` repeating (2026-11-16) |

Images live in `build/` or the session scratchpad (gitignored); each can be
rebuilt from the CI kernel with the tools below. Recovery after every run:
LK fastboot → `fastboot flash boot_b device/slot_b_readback/boot_b.img` →
root `sha256sum /dev/block/by-name/boot_b` = `851a5435…ae4f5`.

## Open

| Area | State | Notes |
|---|---|---|
| UFS root (needed for a real iOS root) | **not implemented** | LK leaves the host disabled; register dump and Linux sequence in [lapis_ufs_bringup.md](lapis_ufs_bringup.md) |
| Touch + display | **works on iOS** (v13 touch, v14 full-screen 1.138× scaled display, 2026-11-22) | opt-in `lapisnt=1`; SPI3 controller driver; stages the flashless firmware and polls reports through SweetHID. GitHub Actions run 44506319609 built XNU with all 85 patches; details and limits in [lapis_touch_bringup.md](lapis_touch_bringup.md) |
| Battery + charging | **work on iOS** (v18, 2026-11-22) | bq28z610 gauge on I2C0, MT6375 charger on I2C5 (power-good, 1500 mA limits); [lapis_battery_bringup.md](lapis_battery_bringup.md) |
| Keys (power, volume) | **work on iOS** (v15, 2026-11-22) | Power/Vol+ from MT6363 TOPSTATUS 0x1e over PMIF ch 2, Vol− from GPIO 54; [lapis_keys_bringup.md](lapis_keys_bringup.md) |
| pstore log channel | partial | survives warm resets, lost on PSCI cold reset and on Power + Vol-down |
| USB, audio, Wi-Fi/BT, modem, camera, sensors, GPU | not started | none of ios-sweet/topaz/ruby has GPU, Wi-Fi, modem or camera either |
| AMP scheduling (A78 as P cluster) | not started | all eight cores run as one SMP cluster |
| iOS root (iPhone X 16.7.16) | **built** | 16 GiB HFS+ from the IPSW: launchd, dyld, SpringBoard, the 3.2 GiB dyld shared cache. `build/iosroot.img`, gitignored. Needs the UFS partition to deploy. |
| iOS userland | reaches the lock screen (user-reported Run 6) | touch input and remaining drivers are separate work |

The NT38771 firmware-download and touch-input path now has an offline prototype, pending XNU compilation and device verification. UFS host bring-up and key input remain separate unfinished work.

## Tools

| Tool | Purpose |
|---|---|
| `tools/lapis_xnu_patch_preflight.py` | strict patch-queue check (85 patches) |
| `.github/workflows/xnu-lapis.yml` | CI kernel build (macos-15, Xcode 16.0) |
| `tools/lapis_mkadt.py` | ADT (`--no-ufs`, `--with-a78`, `--debug`, `--qemu`, `--qemu-lapis-fb`, `--qemu-smp`) |
| `tools/ios-sweet/` | vendored MIT `mkpayload`, `mkinit`, `sweet-init.S`, `mkiosroot.sh`, `sweet-root-tweaks.py` |
| `tools/lapis_mkroot_hfs.sh` | HFS+ root image (sudo, hfsprogs) |
| `tools/lapis_shim/` | LK boot shim + probes (`build.sh OUT shim\|regdump\|pstest\|tchip`), `qemu_shim.py` |
| `tools/lapis_boot_pack.py` | pack into a boot_b copy (`--probe` / `--image --append`) |
| `tools/lapis_lz4.py` | LZ4 legacy codec for the stock kernel format |
| `tools/nt38771_fw_map.py` | parse the NT38771 firmware partition table |
| `tools/lapis_ipsw_extract.py` | pull DeviceTree + 3 dmgs from an iPhone X IPSW |
| `tools/lapis_gpt_plan.py` | plan an `iosroot` partition from a GPT dump (dry run) |
| `tools/lapis_qemu_regress.sh` | boot the kernel to PID 1 in QEMU and check milestones |
| `tools/lapis_payload/` | first-stage framebuffer probe and its QEMU check |

See [HANDOFF.md](../HANDOFF.md) for how to continue.
