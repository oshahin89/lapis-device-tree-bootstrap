# Lapis iOS port: experiment readiness

**Current decision: ready for offline source/build experiments; not ready for a phone boot experiment.** The phone and its active boot set have been captured read-only. No XNU for lapis has been built, and the bootloader has no confirmed RAM-boot path.

## What is now staged

- Device identity and live state: Redmi 25080RABDG / `lapis`, MT6878, Android 16 / SDK 36, OS3.0.309.0.WPPMIXM, active slot B, unlocked bootloader, KernelSU root, SELinux enforcing. The phone remained in Android during this capture.
- Live CPU evidence: all eight CPUs are online in two clusters. `/proc/cpuinfo` reports Arm implementer `0x41`, part `0xD05` for CPUs 0–3 and `0xD41` for CPUs 4–7; Linux topology sysfs groups them as clusters 0 and 1, four cores each. These correspond to Cortex-A55 and Cortex-A78 MIDR part IDs in Linux's CPU table. The Linux boot CPU is CPU0. Arm's A55 and A78 technical references document 16K translation-granule support for those core models, but this kernel does not export the live `ID_AA64MMFR0_EL1` value. The initial XNU profile selects 16K pages and exposes only CPU0 until a lapis boot confirms the effective register state and handoff. [Linux ARM64 CPU MIDR table](https://github.com/torvalds/linux/blob/master/arch/arm64/include/asm/cputype.h), [Arm A55 TRM](https://documentation-service.arm.com/static/5e7e1405b471823cb9de57ae), [Arm A78 TRM](https://documentation-service.arm.com/static/64bea12b38511951cb7a0ee8).
- Runtime FDT (not the 1 GiB candidate DTB) identifies CPU0 as MPIDR `0x0`, CPUs 1–3 as `0x100`/`0x200`/`0x300`, and CPUs 4–7 as `0x400`/`0x500`/`0x600`/`0x700`; every CPU node uses PSCI. The ARM GICv3 distributor is `0x0c000000`/`0x40000`, redistributors `0x0c040000`/`0x200000`. The architected timer frequency is 13 MHz and its PPI list is present in the DTB. This narrows the first CPU plan to CPU0 on the A55 cluster but does not prove LK's XNU entry contract or PSCI conduit behavior.
- Complete raw copies of the active B boot chain are in [slot_b_readback](slot_b_readback/README.md): `boot_b`, `init_boot_b`, `vendor_boot_b`, `dtbo_b`, all three vbmeta partitions, and `lk_b`. The four boot payload partitions match the supplied factory images byte-for-byte. Existing slot A readbacks are also present, but slot A is OS3.0.308 and is not a proven recovery slot.
- Runtime FDT, iomem, display/panel data, NT38771 module/firmware and SPI3 host analysis are already recorded in the linked hardware contracts. The NT38771 parser/state pipeline now accepts a caller-provided event callback and passes a host C syntax compile; it remains a prototype and is not linked into XNU or an SPI driver.
- The Ruby reference project is present at `reference/ruby-ios-bringup`. Its exact upstream project revision is also staged at [xnu-topaz-baseline](../reference/xnu-topaz-baseline) (`5095ac19dbdcf37b64b81fe443f4027ad51b84bf`). That tree contains the port patches and recipes, not the full XNU source. Its workflow fetches the XNU build harness and builds xnu-8796 on macOS 14 with Xcode 15.
- The exact XNU 8796.141.3 source tag is staged at [apple-xnu-8796.141.3](../reference/apple-xnu-8796.141.3), with an isolated lapis worktree at [xnu-lapis](xnu-lapis/). Patch `0001-lapis-board-config.patch` adds only the compile-time machine profile; `git apply --check` succeeds against the pinned source. The LAPIS profile caps XNU at CPU0. A GitHub Actions build workflow is staged at [xnu-lapis.yml](../.github/workflows/xnu-lapis.yml). A local Linux build was attempted from a scratch copy with the HakiOS Linux toolchain, but no kernel artifact was produced; the Windows-only limitation is obsolete, while the Linux harness still needs adaptation.

## Why the phone is not ready for an XNU boot

1. The approved RAM-boot probe was rejected by LK with `unknown command`. A/B raw backups help with recovery but do not prove a restore or boot path.
2. Slot A is older OS3.0.308 and has not been booted as a recovery system; slot B is the currently working OS3.0.309 installation. No partition write has been authorized or attempted in this pass.
3. The Ruby packing recipe assumes a different device and boot image layout. Lapis uses boot header v4, a 64 MiB `boot` partition, separate `init_boot` and `vendor_boot`, and an LZ4-compressed stock Linux kernel. We do not yet know the LK kernel-loading path for a custom image.
4. A compile-time LAPIS CPU/GIC/timer profile and the NT38771 report parser exist, but LAPIS still has no GIC/PSCI handoff, UFS, display, SPI host or XNU input integration. The touch parser is a host-side prototype, not a device driver.
5. The staged workflow still targets macOS/Xcode. The build and patch changes are uncommitted, so it has not been dispatched. A Linux build in a disposable copy reached XNU compilation: setup tools and `init_sysent.o` built, then the build stopped on Apple-specific type checks, aligned `uio_stackbuf_t` arrays, and SHA assembly syntax. Do not weaken XNU's type or layout checks to force a Linux build. The Windows absolute Git worktree pointers have been repaired, and Git now reports only the five intended LAPIS source changes despite the original CRLF files. The three-patch queue now passes the reproducible clean-source preflight in [`tools/lapis_xnu_patch_preflight.py`](../tools/lapis_xnu_patch_preflight.py). No kernel artifact was produced.

## Experiment sequence

### Phase A — offline XNU/source work

1. Make the Linux build repeatable in an isolated script: normalize source line endings in a disposable build copy, supply host-tool shims, and make syscall generation portable across `mawk`/`gawk`.
2. Continue the lapis-specific patch series on top of the pinned XNU source. The Linux compile exposed missing LAPIS GIC definitions/driver integration and a duplicate `reserved10` field in generic ARM `pmap.h` under this feature combination. Resolve these only after confirming the intended structure and interrupt-controller design. Do not apply Ruby-only UFS, CPU topology, memory, framebuffer, watchdog, or Goodix patches unchanged.
3. Implement a mock/host layer for FDT parsing and the NT38771 report pipeline, then wire XNU-only interfaces and compile in the Linux harness or staged macOS/Xcode CI workflow.
4. Use [the read-only boot v4 inspector](../tools/lapis_boot_v4_inspect.py) to parse any future candidate and compare it with the captured factory B images. It already confirms that the factory OS3.0.309.0 boot set matches the active B readbacks byte-for-byte; it does not validate AVB signatures.
5. Keep hardware access disabled while controller clock, GPIO ownership, IRQ setup, shared panel/touch reset, and LK handoff semantics remain uncertain.

### Phase B — device trial gate

Before the first device boot, separately establish and document all of the following:

- a verified way to enter recovery/fastboot and restore the exact active B boot set;
- a tested boot strategy compatible with this LK (the rejected `fastboot boot` path does not qualify);
- an image that passes offline header, size, hash and AVB/layout checks;
- a known-good power/thermal state and a recovery observation plan;
- explicit user authorization for the exact partition or bootloader operation.

Until those gates pass, do not change slot selection, boot-critical partitions, LK, AVB state, reset GPIO90, or SPI hardware state. The safe work available now is offline source adaptation, image parsing and build preparation.

## Stage 1 probe image (built offline 2026-11-11, never flashed)

A first device experiment can measure the open framebuffer questions without
running XNU. Tools (all host-only; none talks to the phone):

- `tools/lapis_payload/probe_fb.S` — 200-byte, position-independent AArch64
  payload. It writes only the LK framebuffer reservation, then parks in `wfe`.
  Each derived buffer gets: a top band of word `0xFFFF0000` (red = ARGB8888,
  blue = ABGR), *(index+1)* green bands out of 8 (which buffer LK scans out),
  and a white stripe in columns 0–63 (vertical = stride 5120 is right).
- `tools/lapis_payload/qemu_check.py` — runs a payload in QEMU `virt` and
  checks all three buffers. Result: all three patterns correct, CPU parked.
- `tools/lapis_lz4.py` — LZ4 legacy codec; decodes the stock kernel to the
  documented 35,432,960-byte Image and round-trips block 0 exactly.
- `tools/lapis_boot_pack.py` — copies the stock 64 MiB boot partition,
  replaces Image code0 with `b 0x1000`, puts the payload at Image+0x1000,
  recompresses LZ4 block 0 only, keeps blocks 1–4, the boot-signature blob and
  vbmeta byte-identical, and rewrites the AVB footer offsets. It re-parses its
  output and fails unless the decompressed Image differs only in code0 and the
  payload.

Built from `slot_b_readback/boot_b.img` (sha256 `851a5435…ae4f5`, matches the
recorded readback): output sha256
`a66e4e30ae0f2222f239725a6a2e6be5381cba1b795f293f3bc065c2be41bcbb`, kernel
16,544,915 → 17,125,864 bytes, AVB `original_image_size` `0xfcd000` →
`0x105b000`. The decompressed packed Image, run in QEMU from byte 0, branches
to the payload and draws all three patterns. The image is not committed.

Still unverified before flashing it anywhere: LK accepting a changed
`kernel_size` and a stale AVB hash in orange state, and LK's behaviour while
the probe is parked (watchdog reset → boot loop on the same image).

### Restore sources (checked 2026-11-11, read-only)

The full OS3.0.309.0 factory package is now present in
`lapis_global_images_OS3.0.309.0.WPPMIXM_16.0/images/` (12 GiB, not in git).
Against the readbacks: `boot`, `init_boot`, `vendor_boot`, `dtbo` are
byte-identical to slot B; `vbmeta`, `vbmeta_system`, `vbmeta_vendor` and `lk`
are identical to slot B up to the partition's zero padding. Every slot A image
differs (A is OS3.0.308.0). So slot B has two independent restore sources: the
raw readbacks and the factory images.

`flash_all_except_data_storage.sh` is the last-resort fastboot restore: it
checks `anti` (package anti-rollback 1) and product `lapis`, erases
`misc`/`boot`, flashes every firmware partition to **both** slots
(`*_ab`, including `preloader_ab`, `lk`, `vbmeta*`, `super`, `boot`,
`vendor_boot`, `init_boot`), keeps `userdata`, then `set_active a` and
reboots. It needs a working LK fastboot. `flash_all.sh` additionally erases
`metadata` and flashes `userdata` (wipes data). The scatter files and
`download_agent/` are for MediaTek BROM flashing, which on Xiaomi normally
needs an authorised account; do not count on it.

### Revised device order (each step needs explicit approval)

1. **No writes:** `adb reboot bootloader`, confirm fastboot sees the device,
   read `getvar current-slot`, `getvar anti`, `getvar product`,
   `getvar partition-size:boot_b`, then `fastboot reboot` and confirm Android.
   Also confirm entering fastboot from power-off with Vol-down + Power.
   **Step 1 result (2026-11-11, no writes):** `adb reboot bootloader` reached
   LK fastboot in ~10 s. Read-only `getvar`: product `lapis`, current-slot
   `b`, slot-count 2, anti `1` (equals the package's `anti_version.txt`, so
   the factory flash script would pass its rollback check), unlocked `yes`,
   secure `no`, is-userspace `no` (LK fastboot, not fastbootd),
   max-download-size `0x8000000` (128 MiB, holds a 64 MiB boot image),
   `partition-size:boot_a`/`boot_b` `0x4000000`, `partition-type:boot_b`
   `raw`, version-bootloader `lapis_3f2e32c7d_202609200804`,
   bypass_check_imgkey `no`. Slot state: **b successful=yes, unbootable=no,
   retry-count 1; a successful=no, unbootable=no, retry-count 0** — slot A is
   not a working fallback, so a broken `boot_b` would not fall back to A;
   recovery is through LK fastboot. `fastboot reboot` returned to Android
   (`_b`, OS3.0.309.0, orange) with boot_completed after ~21 s. Still to do by
   hand: enter fastboot from power-off with Vol-down + Power.
2. **Content no-op write:** `fastboot flash boot_b` with the exact stock image
   (sha256 `851a5435…ae4f5`, identical to what is already there), reboot,
   confirm Android. This proves the write path without changing any byte or
   switching to the older slot A.
3. **Probe:** flash the probe image to `boot_b`, reboot, photograph the
   screen; then return to fastboot (Vol-down + Power) and flash the stock
   `boot_b` back, reboot, confirm Android.

## Immediate next actions

1. Run `python3 tools/lapis_xnu_patch_preflight.py` after patch changes; then run the pinned macOS/Xcode workflow from a commit containing those patches and inspect the artifact and checksums. The Linux build is a diagnostic path only and has known host-compiler blockers.
2. Implement LAPIS-specific CPU/GIC/timer/FDT handoff logic from measured evidence. Generic Apple GIC code assumes Apple device-tree paths and interrupt setup, so compile-time constants do not make it safe to call. Ruby SoC drivers are not drop-in compatible.
3. Prove a lapis-specific image load/decompression/handoff contract from LK source or an independent recovery environment before creating any phone-bound image.

**Build update (2026-10-29):** The HakiOS open-kernel QEMU build completed from source in a separate temporary tree and reached its hello-world user process. Separately, the LAPIS XNU source was copied to `/tmp`; after adapting line endings and host tools in that scratch copy, an offline Linux build reached XNU compilation but failed as described above. The read-only boot v4 inspector now confirms the factory OS3.0.309.0 `boot`, `init_boot`, and `vendor_boot` match the active B readbacks exactly. No lapis kernel or phone image was produced. No phone operation was performed during this Linux build work; no reboot, flash, SPI transfer, GPIO write, or device-file write occurred.

**Readiness update (2026-10-29):** Fixed the Windows-absolute Git worktree pointers for the local LAPIS XNU tree and configured `core.autocrlf=input` only for the XNU worktrees. Patch 0001 and 0002 apply cleanly; regenerated the malformed hunk in 0003. Validated all three patches in order against a clean export of the pinned XNU commit, with whitespace checks and reverse checks. The first build checkpoint is now source-reproducible; the next one is a successful macOS/Xcode kernel artifact. The phone remains gated: LK rejected `fastboot boot`, no recovery/restore trial is proven, and no LAPIS image exists. No phone state was changed.
