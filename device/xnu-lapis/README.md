# Lapis XNU port workspace

This directory holds the lapis-specific XNU work. It is separate from the untouched reference repositories and the upstream source snapshot.

## Current source and patch

- XNU source tag: `xnu-8796.141.3`, the tag selected by Apple's macOS 13.5 release manifest; checked out at commit `1b191cb58250d0705d8a51287127505aa4bc0789`.
- The working source is in `xnu/` on branch `lapis-prototype`; the clean tag snapshot remains in `reference/apple-xnu-8796.141.3`.
- `patches/0001-lapis-board-config.patch` adds the XNU machine config `LAPIS`, plain arm64 target flags for the A55/A78 common baseline, an initial single-CPU cap, 16K pages, cache line assumptions and runtime-FDT GIC/timer constants.
- `patches/0002-linux-mawk-syscall-generator.patch` makes XNU's syscall-table generator emit valid C with `mawk` as well as `gawk`.
- `patches/0003-lapis-arm-pmap-reserved-field.patch` renames a reserved field in an inactive preprocessor branch so the LAPIS feature combination compiles without a duplicate member; the field type, position and structure size stay the same.
- `patches/0004-lapis-open-arm64-pmap-config.patch` adds a LAPIS XNU master config that selects XNU's published ARM64 assembly and pmap sources (`nos_arm_asm`, `nos_arm_pmap`) instead of expecting those objects from an Apple KDK archive. This is an experimental build configuration, not a completed or validated LAPIS memory-management port.
- `patches/0005-lapis-no-apple-amcc-translation-units.patch` supplies empty translation units for two AMCC read-only-region source files that XNU's file list expects with `nos_arm_asm` but Apple does not publish. It provides no AMCC hardware routines; LAPIS still needs its own integrity and memory-protection design.

## Scope of patch 0001

This is a compile-time board profile, **not a bootable lapis port**. It intentionally does not add a platform expert, CPU driver, custom GIC interrupt handling, PSCI startup, UFS, framebuffer, panel, touch or boot shim. The initial profile exposes only CPU0; do not enable the second cluster until PSCI, GIC affinity routing and resume behavior have an XNU-side implementation.

The CPU model IDs read from Linux are `0xD05` and `0xD41` (Cortex-A55 and Cortex-A78). Their Arm technical references document 16K-granule support, which is why the XNU profile selects 16K pages for the iOS-generation userland. The live ID register values are not exported by this Android kernel, so first-stage runtime verification remains an open item. The build target uses Armv8.2-A plus crypto based on both CPU models and the live Linux feature intersection; build output still needs disassembly review for instructions that require optional extensions.

The timer virtual interrupt is derived from the runtime FDT's third architected-timer PPI (PPI 11, GIC INTID 27), not copied from Ruby's PPI numbering. The values are compile-time notes only; no lapis GIC driver consumes them yet.

The first-stage display console is still unresolved: LK's framebuffer base and allocation size are known, but the runtime FDT does not provide stride or pixel depth. LK contains both 16-bit and 32-bit ARGB8888 fill paths; the selected path is not established from strings alone. Android exposes no fbdev node. Do not carry Ruby's framebuffer constants over or write pixels until those fields are decoded.

## Build limitation

The pinned build job is at `.github/workflows/xnu-lapis.yml`. It targets macOS 14/Xcode 15, fetches `blacktop/darwin-xnu-build` at `663906528d1cd5ea8fde7de6f16de73db9511549`, requests XNU 13.5 (`xnu-8796.141.3`), and applies the patches in `patches/`. Its uploaded output is named as build output, with a status file indicating whether a kernel file was produced; a compile artifact is not a phone-bootable image.

### Latest CI result

GitHub Actions run `41288279292` (run 3, XNU 13.5, `DEVELOPMENT`, `ARM64`, `LAPIS`) applied the patches and compiled objects, but failed at the final link with 261 undefined ARM64 symbols. These include pmap and low-level entry, exception-return and cache routines. The XNU source configuration marks the relevant pmap and assembly sources `optional nos_arm_pmap` / `optional nos_arm_asm`; Apple's build harness normally supplies their implementations from a machine-specific KDK archive. There is no LAPIS archive, and borrowing another machine's private archive would not implement this device's platform support. Therefore this run did not produce a kernel. The warnings emitted while applying unrelated compatibility patches were not the fatal error.

Patch 0004 selects the published pmap and assembly implementations, following the approach used by the Ruby bring-up reference. Patch 0005 fills the source-list gap without inventing Apple AMCC behavior. The five-patch queue passes the clean-source apply/reverse preflight; this does not compile or link XNU. Do not treat a successful compile or a linker workaround as evidence that the kernel can boot; a separate platform bring-up and verified recovery path are still required before phone experiments.

GitHub Actions run `41290346426` applied patch 0004 and started the build, but XNU stopped before linking: `files.arm64` requested `osfmk/arm64/amcc_rorgn_ppl.c`, which is absent from Apple's published source. Patch 0005 adds no-op translation units for the AMCC-only files so we can continue compiling the generic ARM64 code; it does not emulate or implement Apple's AMCC protections. The five-patch queue passes the clean-source apply/reverse preflight. A new CI run is needed to find the next blocker.

The pinned five-patch queue is reproducible with `python3 tools/lapis_xnu_patch_preflight.py`: it exports the clean pinned XNU commit, applies every patch with whitespace errors rejected, and reverse-checks the result. This catches malformed hunks before CI.

The Windows-to-Linux move left absolute Windows paths in the LAPIS worktree's `.git` pointers. Those pointers are now relative and the XNU worktree-specific Git setting is `core.autocrlf=input`. Source CRLF remains; the Linux build normalizes only a disposable copy. An offline Linux attempt using HakiOS cross-build dependencies and host-tool shims also stopped on Apple-specific type checks, aligned `uio_stackbuf_t` arrays, and SHA assembly syntax. The LAPIS header supplies compile-time GICv3 constants only; no LAPIS GIC handoff/initialization path exists.

## Safety status

No image was generated or installed on the phone. The device's LK rejected `fastboot boot`, and there is no verified recovery-boot path. Device boot experimentation remains gated on a recovery method that has been demonstrated to restore the exact active B boot set.
