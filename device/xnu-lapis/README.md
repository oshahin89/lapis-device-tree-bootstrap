# Lapis XNU port workspace

This directory holds the lapis-specific XNU work. It is separate from the untouched reference repositories and the upstream source snapshot.

## Current source and patch

- XNU source tag: `xnu-8796.141.3`, the tag selected by Apple's macOS 13.5 release manifest; checked out at commit `1b191cb58250d0705d8a51287127505aa4bc0789`.
- The working source is in `xnu/` on branch `lapis-prototype`; the clean tag snapshot remains in `reference/apple-xnu-8796.141.3`.
- `patches/0001-lapis-board-config.patch` adds the XNU machine config `LAPIS`, plain arm64 target flags for the A55/A78 common baseline, an initial single-CPU cap, 16K pages, cache line assumptions and runtime-FDT GIC/timer constants.

## Scope of patch 0001

This is a compile-time board profile, **not a bootable lapis port**. It intentionally does not add a platform expert, CPU driver, custom GIC interrupt handling, PSCI startup, UFS, framebuffer, panel, touch or boot shim. The initial profile exposes only CPU0; do not enable the second cluster until PSCI, GIC affinity routing and resume behavior have an XNU-side implementation.

The CPU model IDs read from Linux are `0xD05` and `0xD41` (Cortex-A55 and Cortex-A78). Their Arm technical references document 16K-granule support, which is why the XNU profile selects 16K pages for the iOS-generation userland. The live ID register values are not exported by this Android kernel, so first-stage runtime verification remains an open item. The build target uses Armv8.2-A plus crypto based on both CPU models and the live Linux feature intersection; build output still needs disassembly review for instructions that require optional extensions.

The timer virtual interrupt is derived from the runtime FDT's third architected-timer PPI (PPI 11, GIC INTID 27), not copied from Ruby's PPI numbering. The values are compile-time notes only; no lapis GIC driver consumes them yet.

The first-stage display console is still unresolved: LK's framebuffer base and allocation size are known, but the runtime FDT does not provide stride or pixel depth. LK contains both 16-bit and 32-bit ARGB8888 fill paths; the selected path is not established from strings alone. Android exposes no fbdev node. Do not carry Ruby's framebuffer constants over or write pixels until those fields are decoded.

## Build limitation

The pinned build job is staged at `.github/workflows/xnu-lapis.yml`. It targets macOS 14/Xcode 15, fetches `blacktop/darwin-xnu-build` at `663906528d1cd5ea8fde7de6f16de73db9511549`, requests XNU 13.5 (`xnu-8796.141.3`), applies patch 0001, and uploads the resulting kernel artifact and hashes. This is an offline compile check of the lapis machine profile only; it does not include the Ruby platform-driver patches and cannot produce a phone-bootable image.

This Windows host has neither Xcode nor WSL, and the workspace root has no Git remote, so the workflow has not been dispatched. To run it, the workspace needs to be committed to a GitHub repository and the workflow file must be present at that repository's `.github/workflows/` path. The artifact must then be inspected before any device experiment.

## Safety status

No image was generated or installed on the phone. The device's LK rejected `fastboot boot`, and there is no verified recovery-boot path. Device boot experimentation remains gated on a recovery method that has been demonstrated to restore the exact active B boot set.
