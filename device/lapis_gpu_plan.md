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
- Confirmed on Android (2026-11-23): **Mali-G615, 2 cores, r1p3**
  (gpuinfo `Mali-G615 2 cores r1p3 0xB8A3`), shader core mask 0x5, power
  policy coarse_demand; CSF firmware `/vendor/firmware/mali_csffw.bin`
  (290816 B, magic c3f13a6e, sha256 3bbb5e95…, pulled to build/gpu/, not
  committed). gpufreq: 47 OPPs, idle at OPP 35 = 508 MHz / 0.59375 V, Vsram
  0.75 V; **GPUEB support On** (MediaTek's GPU embedded processor owns
  power / DVFS: power-up likely goes through its mailbox, not plain scpsys).
- **Mesa supports it**: docs.mesa3d.org/drivers/panfrost lists "G615, G715 |
  Valhall (v11)" with OpenGL ES 3.1 and Vulkan 1.4 (PanVK). On Linux that runs
  on the panthor kernel driver (CSF), so step 3 targets panthor's UAPI.

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

## First concrete steps

- Shim probe: MFG power state after a reboot from Android (the audio domain
  kept Android's state; the GPU may too), then GPU_ID at 0x13000000 + 0x0
  (expect 0xb8a3xxxx-ish product id for G615 r1p3).
- Android trace of the GPU power-up (regmap / clk / scpsys events, GPUEB
  mailbox) with the boot-time cmdline trick, as for audio.

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
