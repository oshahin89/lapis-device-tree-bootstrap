# lapis GPU (Mali, Valhall) — plan

Started 2026-11-23. Nothing runs on the GPU yet; iOS draws with the CPU
(topaz's software CoreAnimation path).

## Our GPU

- Runtime FDT: `mali@13000000`, `"mediatek,mali", "arm,mali-valhall"`, 0x480000
  of registers, IRQs JOB / MMU / GPU / **EVENT** / PWR (an EVENT line and a
  firmware idle hysteresis property point to a **CSF** GPU: the job manager is
  firmware running on the GPU's own MCU), OPPs up to 1.3 GHz (opp00
  0x4d7c6d00 Hz), `ged-supply`, protected-memory allocator, memory group
  manager. Android's driver: `mali_kbase_mt6878_r44` (Arm DDK r44).
- To confirm from Android: GPU product (gpuinfo), core count, the CSF
  firmware file (`mali_csffw.bin`), MFG power domains / clocks.

## What exists

- **hakios-mt6768** (MT6768, Mali-G52, Bifrost **job manager**, not CSF): a
  kernel Mali bring-up (`redwood_mali.c` / `RedwoodMali.cpp`) plus Mesa
  panfrost and their Metal driver; CoreAnimation draws through Metal, ~95% of
  frames in 18 ms. Their user space for that phone is not public.
- **hakios-xnu-kernel** (`components/metal`, MIT for `plugin/`): the Metal
  driver itself, GPU-agnostic at the top: Apple's shader IR (AIR, from the
  system's .metallib) -> NIR (`air-frontend.patch`) -> a Mesa gallium driver
  (today freedreno / ir3 for Adreno), plus `darwin-drm` (a DRM-like interface
  between Mesa and an xnu GPU service). Their docs: `docs/air.md`.

## What lapis needs (in order)

1. **Power and clocks**: MFG power domains (scpsys), GPU PLL, `ged-supply`
   voltage — read Android's sequence (the regmap / clk trace trick used for
   audio works here too).
2. **Kernel CSF driver** in xnu (the big piece, like Linux `panthor`): GPU
   reset and identification (GPU_ID), MMU / address spaces, load and boot the
   CSF firmware on the MCU, the global interface, command stream groups and
   queues, IRQs. First milestone: read GPU_ID from the shim; second: firmware
   boots and answers a GLB request.
3. **darwin-drm for panthor**: the ioctls Mesa's panfrost uses on a CSF GPU
   (panthor UAPI: VM_BIND, GROUP_CREATE, GROUP_SUBMIT, syncobjs), backed by (2).
4. **Mesa panfrost (Valhall) built for Darwin** + hakios's AIR frontend and
   Metal plugin retargeted from freedreno to panfrost.
5. CoreAnimation through Metal; then speed.

Each step is weeks; (2) and (3) are the bulk. The CPU side is still worth
fixing on the way: the big A78 cluster runs at ~25 MHz after LK (see
lapis_next_session.md), so iOS uses cpus=4 for now.
