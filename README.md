# iOS on the Redmi lapis (MediaTek MT6878)

Apple's XNU kernel and the iPhone X's iOS 16.7 userland running on a Redmi
`lapis` (25080RABDG, MT6878: 4× Cortex-A55 + 4× Cortex-A78, Mali-G615 MC2,
UFS, 6.83" panel). The kernel is built from the ios-sweet / ios-topaz patch
series plus this repository's LAPIS patches; the phone dual-boots Android
(default) and iOS (Volume up) from one `boot_b` image.

Updated 2026-11-24.

## What works on the phone

| Area | State | Details |
|---|---|---|
| Boot | iOS to the lock screen and home screen; Android stays the default | [dual boot](device/lapis_dual_boot.md) |
| CPUs | 8 cores come up; iOS runs on the 4 A55s (`cpus=4`): the A78 cluster sits at ~25 MHz after LK until its DVFS is done | [next session notes](device/lapis_next_session.md) |
| Storage | iOS root on a UFS partition (HFS+, read-write); XNU's own UFS host driver | [UFS bring-up](device/lapis_ufs_bringup.md) |
| Display | full screen, 1.138× scaled from the iPhone X layout | [touch + display](device/lapis_touch_bringup.md) |
| Touch | NT38771 over SPI3 (flashless firmware staged by the kernel) | [touch](device/lapis_touch_bringup.md) |
| Keys | power and volume | [keys](device/lapis_keys_bringup.md) |
| Battery / charging | bq28z610 gauge, MT6375 charger | [battery](device/lapis_battery_bringup.md) |
| Vibration | AW86927 haptics (key clicks) | [next session notes](device/lapis_next_session.md) |
| Audio | speaker tone from the kernel (AFE + TFA9865 amps); no iOS audio service yet | [audio](device/lapis_audio_bringup.md) |
| Brightness | panel backlight set over DSI from the kernel; not yet wired to iOS's slider | [next session notes](device/lapis_next_session.md) |
| **GPU** | **Mali-G615 driven from XNU and from iOS programs; Mesa's panfrost draws on it** (see below) | [GPU plan and log](device/lapis_gpu_plan.md) |
| Metal | `MTLCreateSystemDefaultDevice()` gives our device; buffers, blits, compute kernels and rendering (Apple's compiled shaders, textures and samplers, vertex descriptors, blending) run on the GPU; IOSurfaces and CoreAnimation next | [Metal plan](device/lapis_metal_plan.md) |
| Wi-Fi, Bluetooth, modem, camera, sensors | not started | |

## The GPU stack

Each layer was proven on the phone before the next one was built
(device/lapis_gpu_plan.md has every step, with its log in `device/captures/`):

1. **Kernel driver** (`device/xnu-lapis/driver/sweet_lapis_gpu.c`, patch 0091):
   power through MediaTek's GPUEB, the Mali CSF firmware loaded and booted,
   page tables, command-stream groups, an event-servicing thread (no IRQ line
   in this port), GPU faults isolated to their group.
2. **Kernel interface** (`device/xnu-lapis/driver/LapisGPU.cpp`, patch 0092):
   an IOKit service speaking Linux panthor's requests (VMs, buffers, groups,
   submissions, tiler heaps) plus a few of our own (wait, cache sync, log).
3. **Programs**: `tools/lapis_gputest` drives it directly (3000/3000 command
   streams, ~46 µs a round trip).
4. **Mesa 26.3.1 gallium panfrost** for iOS (`tools/mesa`): a libdrm stand-in
   over LapisGPU (`tools/mesa/lapis-drm`), Darwin patches, and a small
   gallium target. On the phone: the screen is created ("Mali-G615 MC2
   (Panfrost)"), GPU clears read back exactly (423 µs each), shaders compile
   and a triangle draws correctly (481 µs a frame).
5. **Metal** (`tools/lapis_metal`): a plug-in in the shape of hakios's, over
   Apple's `AGXMetalA11.bundle`, announced by the kernel's `IOAcceleratorES`
   service (boot-arg `lapismetal=N`). Next: CoreAnimation drawing on the GPU.

What this port learned about the hardware and XNU on the way (write-combined
mappings panic this kernel, the GPU is not coherent, submissions must be
64-byte blocks, and more) is recorded in the GPU plan.

## Building

Everything is built on GitHub Actions (macOS runners); nothing of Apple's,
MediaTek's or Arm's is committed.

| Workflow | Builds |
|---|---|
| `xnu-lapis.yml` | the XNU kernel: ios-sweet 0001–0079 (fetched at a pinned commit) + LAPIS patches 0080–0092 (`device/xnu-lapis/patches`) |
| `lapis-gputest.yml` | `tools/lapis_gputest` (GPU test program) |
| `lapis-mesa26.yml` | Mesa 26.3.1 panfrost for iOS + `tools/lapis_galliumtest` (two stages: the Mac's CL tools, then the iOS cross build) |
| `lapis-metal.yml` | the LapisMetal plug-in + `tools/lapis_metal/lapis_metaltest.m` |
| `lapis-metalprobe.yml` | `tools/lapis_metalprobe` (iOS 16 Metal's private classes) |

Then, on a Linux machine:

```sh
# the kernel payload (boot-args included)
python3 tools/ios-sweet/mkpayload.py kernel.development.sweet build/phone_ufs_debug.adt build/payload.bin \
    --cmdline "debug=0xc serial=3 cpus=4 keepsyms=1 -noprogress sweethw=0 wdt=-1 rd=ufsroot0 ufsroot=iosroot ufslun=2 ufsrw=1 rootrw=1 launchd_unsecure_cache=1 unentitled_ios_sim_launch=1 ignition_level=1 lapisnt=1 sweetfb_noconsole=1 lapisbacklight=0"
# one boot_b image: Android by default, iOS on Volume up (includes the GPU firmware when build/gpu/mali_csffw.bin exists)
tools/lapis_dual_image.sh build/payload.bin build/lapis_dual.img
```

`build/gpu/mali_csffw.bin` is pulled from the user's own phone
(`/vendor/firmware/mali_csffw.bin`) and stays out of git.

## On the phone (rooted Android, no PC needed)

```sh
# flash the dual image (from Download)
su -c 'dd if=/sdcard/Download/lapis_dual.img of=/dev/block/by-name/boot_b && sync'
# reboot, hold Volume up for iOS; the kernel log is kept on UFS:
su -c 'dd if=/dev/block/sdc92 bs=4096 skip=4193279 count=1025 of=/sdcard/Download/sweetlog.bin'
```

Programs and libraries go into the iOS root without mounting it:
`tools/lapis_rootslot.sh` writes them over the blocks of iOS files lapis does
not use (`IOMFB_FDR_Loader`, an Intel modem firmware, two files of Apple's A11
GPU bundle), checking the originals by hash and keeping them for `restore`.
`tools/hfsplus_find.py` finds a file's blocks in the HFS+ image without sudo.
The log is decoded with `reference/xnu-topaz-baseline/tools/sweet-log.py`.

**Recovery**: Power + Volume down → fastboot →
`fastboot flash boot_b device/hyperos4_readback/boot.img`
(sha256 `a01d0f11…`). See [RESTORE_ANDROID.md](device/RESTORE_ANDROID.md).

## Documents

- [Status table](device/lapis_status.md) · [Handoff guide](HANDOFF.md) · [Next session notes](device/lapis_next_session.md)
- Boot: [dual boot](device/lapis_dual_boot.md), [boot contract](device/lapis_boot_contract.md), [UFS cold bring-up log](device/lapis_ufs_cold_bringup_plan.md)
- Hardware: [UFS](device/lapis_ufs_bringup.md), [touch](device/lapis_touch_bringup.md), [keys](device/lapis_keys_bringup.md), [battery](device/lapis_battery_bringup.md), [audio](device/lapis_audio_bringup.md), [SPI3 contract](device/lapis_spi3_xnu_contract.md)
- GPU: [GPU plan and results](device/lapis_gpu_plan.md), [Metal plan](device/lapis_metal_plan.md)
- iOS root: [preparing the iPhone X userland](device/lapis_ios_prep.md)
- XNU workspace: [device/xnu-lapis/README.md](device/xnu-lapis/README.md)

## Credits and licensing

- ios-sweet / ios-topaz (MT6877 ports, dipa-studio) for the XNU patch series this kernel builds on; their sources are fetched at build time, not committed.
- [hakios](https://github.com/oosexgang/hakios-xnu-kernel) (MIT parts) for the dual-boot idea, the Metal plug-in shape and the libdrm stand-in design this port adapts.
- [Mesa](https://mesa3d.org) (panfrost) and Linux panthor's UAPI header (MIT).
- Linux and MediaTek GPL sources were read for register addresses and sequences only, never copied.
