# Lapis iOS port: experiment readiness

**Current decision: ready for offline source/build experiments; not ready for a phone boot experiment.** The phone and its active boot set have been captured read-only. No XNU for lapis has been built, and the bootloader has no confirmed RAM-boot path.

## What is now staged

- Device identity and live state: Redmi 25080RABDG / `lapis`, MT6878, Android 16 / SDK 36, OS3.0.309.0.WPPMIXM, active slot B, unlocked bootloader, KernelSU root, SELinux enforcing. The phone remained in Android during this capture.
- Live CPU evidence: all eight CPUs are online in two clusters. `/proc/cpuinfo` reports Arm implementer `0x41`, part `0xD05` for CPUs 0–3 and `0xD41` for CPUs 4–7; Linux topology sysfs groups them as clusters 0 and 1, four cores each. These correspond to Cortex-A55 and Cortex-A78 MIDR part IDs in Linux's CPU table. The Linux boot CPU is CPU0. Arm's A55 and A78 technical references document 16K translation-granule support for those core models, but this kernel does not export the live `ID_AA64MMFR0_EL1` value. The initial XNU profile selects 16K pages and exposes only CPU0 until a lapis boot confirms the effective register state and handoff. [Linux ARM64 CPU MIDR table](https://github.com/torvalds/linux/blob/master/arch/arm64/include/asm/cputype.h), [Arm A55 TRM](https://documentation-service.arm.com/static/5e7e1405b471823cb9de57ae), [Arm A78 TRM](https://documentation-service.arm.com/static/64bea12b38511951cb7a0ee8).
- Runtime FDT (not the 1 GiB candidate DTB) identifies CPU0 as MPIDR `0x0`, CPUs 1–3 as `0x100`/`0x200`/`0x300`, and CPUs 4–7 as `0x400`/`0x500`/`0x600`/`0x700`; every CPU node uses PSCI. The ARM GICv3 distributor is `0x0c000000`/`0x40000`, redistributors `0x0c040000`/`0x200000`. The architected timer frequency is 13 MHz and its PPI list is present in the DTB. This narrows the first CPU plan to CPU0 on the A55 cluster but does not prove LK's XNU entry contract or PSCI conduit behavior.
- Complete raw copies of the active B boot chain are in [slot_b_readback](slot_b_readback/README.md): `boot_b`, `init_boot_b`, `vendor_boot_b`, `dtbo_b`, all three vbmeta partitions, and `lk_b`. The four boot payload partitions match the supplied factory images byte-for-byte. Existing slot A readbacks are also present, but slot A is OS3.0.308 and is not a proven recovery slot.
- Runtime FDT, iomem, display/panel data, NT38771 module/firmware and SPI3 host analysis are already recorded in the linked hardware contracts. The parser/state/HID files are prototypes only; none has been compiled or linked into XNU.
- The Ruby reference project is present at `reference/ruby-ios-bringup`. Its exact upstream project revision is also staged at [xnu-topaz-baseline](../reference/xnu-topaz-baseline) (`5095ac19dbdcf37b64b81fe443f4027ad51b84bf`). That tree contains the port patches and recipes, not the full XNU source. Its workflow fetches the XNU build harness and builds xnu-8796 on macOS 14 with Xcode 15.
- The exact XNU 8796.141.3 source tag is staged at [apple-xnu-8796.141.3](../reference/apple-xnu-8796.141.3), with an isolated lapis worktree at [xnu-lapis](xnu-lapis/). Patch `0001-lapis-board-config.patch` adds only the compile-time machine profile; `git apply --check` succeeds against the clean pinned source. The LAPIS profile caps XNU at CPU0. A GitHub Actions build workflow is staged at [xnu-lapis.yml](../.github/workflows/xnu-lapis.yml), but no kernel build has run because the workspace root has no Git remote and this Windows host has no Xcode/WSL.

## Why the phone is not ready for an XNU boot

1. The approved RAM-boot probe was rejected by LK with `unknown command`. A/B raw backups help with recovery but do not prove a restore or boot path.
2. Slot A is older OS3.0.308 and has not been booted as a recovery system; slot B is the currently working OS3.0.309 installation. No partition write has been authorized or attempted in this pass.
3. The Ruby packing recipe assumes a different device and boot image layout. Lapis uses boot header v4, a 64 MiB `boot` partition, separate `init_boot` and `vendor_boot`, and an LZ4-compressed stock Linux kernel. We do not yet know the LK kernel-loading path for a custom image.
4. No lapis CPU/GIC/timer/UFS/display handoff patch set exists yet. The touch parsing pipeline is separate from the missing SPI host, panel integration, and XNU input stack.
5. The current Windows environment has no WSL installation. The proven reference kernel build uses a macOS/Xcode CI environment. The source tree is not a git checkout, so there is no configured CI remote to build from here.

## Experiment sequence

### Phase A — offline XNU/source work

1. Commit/publish the workspace in a user-owned GitHub repository and dispatch `.github/workflows/xnu-lapis.yml` to compile-check the machine profile. This does not create a phone image.
2. Continue the lapis-specific patch series on top of the pinned xnu-8796 build recipe. Do not apply the Ruby-only UFS, CPU topology, memory, framebuffer, watchdog, or Goodix patches unchanged.
3. Implement a mock/host layer for FDT parsing and the NT38771 report pipeline, then wire XNU-only interfaces and compile in the matching macOS/Xcode build environment.
4. Build a non-flashing image-layout verifier for lapis boot v4 and compare it to the captured factory B images. Require byte-for-byte preservation of every non-kernel boot component and record input/output hashes.
5. Keep hardware access disabled while controller clock, GPIO ownership, IRQ setup, shared panel/touch reset, and LK handoff semantics remain uncertain.

### Phase B — device trial gate

Before the first device boot, separately establish and document all of the following:

- a verified way to enter recovery/fastboot and restore the exact active B boot set;
- a tested boot strategy compatible with this LK (the rejected `fastboot boot` path does not qualify);
- an image that passes offline header, size, hash and AVB/layout checks;
- a known-good power/thermal state and a recovery observation plan;
- explicit user authorization for the exact partition or bootloader operation.

Until those gates pass, do not change slot selection, boot-critical partitions, LK, AVB state, reset GPIO90, or SPI hardware state. The safe work available now is offline source adaptation, image parsing and build preparation.

## Immediate next actions

1. Run the staged pinned xnu-8796 compile workflow from an owned CI repository or provide a macOS/Xcode build host; this Windows host cannot run that build as configured.
2. After the profile compiles, port the smallest CPU/GIC/timer/FDT bring-up slice using the runtime DTB and lapis-specific evidence. The Ruby SoC drivers are not drop-in compatible.
3. Design a lapis-specific first-stage boot path and prove its load/decompression/handoff contract from LK/source or an independent recovery environment before creating any phone-bound image.

**No tests or XNU builds were run in this readiness pass.** The only phone operations were property reads and raw reads of existing boot partitions. No reboot, flash, SPI transfer, GPIO write, or device-file write occurred.
