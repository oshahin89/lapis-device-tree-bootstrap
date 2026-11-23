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

- ~~Shim probe: power state + GPU_ID~~ done (tgpu v2).
- ~~CSF firmware boot~~ **done (tcsf v1, dual_v31, first try, 2026-11-23)**:
  GPUEB power-on (0.6 ms), MMU_FEATURES 0x2830 (48-bit VA, 40-bit PA),
  AS_PRESENT 0xff, CSF_ID 0x04100412; soft reset immediate, L2 on in 0.25 ms;
  7 firmware sections loaded into a DRAM pool at 0x58000000 (0xb9000 bytes
  incl. tables), shared section at PA 0x580ac000; AS0 4-level AArch64 4K
  tables, all non-cacheable (MEMATTR 0x4c..., PTW NC); MCU_CONTROL = AUTO ->
  JOB_INT_GLOBAL_IF after 60 us, MCU_STATUS 1 (enabled), no GPU or MMU fault.
  **Global interface v2.5.0**, features 0x10, input VA 0x04002000, output VA
  0x04006000, **8 command stream groups**, group stride 0xa0.
- ~~Milestone 3: first command stream~~ **done (tcsf v7, dual_v38,
  2026-11-23): the GPU ran our command stream.** MOVE48 / MOVE32 /
  STORE_MULTIPLE / WAIT stored 0xc0ffee42 in 70 us; extract advanced to 0x20.
  What it took (logs in device/captures/tcsf_v*_pstore_*):
  - GLB: core_en_mask = shader_present, progress / poweroff timers,
    ack_irq_mask, IDLE_EN | COUNTER_EN, toggle CFG_ALLOC_EN | POWEROFF |
    PROGRESS, ring doorbell 0; PING acked.
  - **The queue's user I/O pages and the group's suspend buffers must be MCU
    VAs in the shared region 0x04000000..0x08000000** (here 0x04100000 /
    0x04101000 / 0x04110000 / 0x04120000), as Android's kbase and panthor do;
    at 0x10000000 the firmware acked the group but never started the stream.
    Found by kprobing kbase_csf_firmware_{cs,csg,global}_input[_mask] on
    Android (device/captures/android_kbase_csf_writes_2026-11-23.txt).
  - CS req idle bits 0x1500 (kbase) + START; CSG START + ENDPOINT_CONFIG
    toggle; CSG doorbell_req bit per queue, GLB doorbell_req bit per group,
    GLB doorbell; then the queue's user doorbell (CSF_DOORBELL(1)) after
    writing insert. Group on AS1 sharing AS0's tables.
  - Probe logs no longer need photos: dual probes tee the console into the
    pstore console zone and chain into Android without a reset.
- Next: an xnu kernel driver doing all this at boot (power, firmware, GLB,
  one CSG), then the panthor-like UAPI for Mesa.
- Android trace of the GPU power-up (regmap / clk / scpsys events, GPUEB
  mailbox) with the boot-time cmdline trick, as for audio.

## tgpu v1 on the phone (dual_v29, 2026-11-23)

- mfg0 domain (PWR_CON 0x1c001eb4) is **on** after a reboot from Android.
- The Mali itself reads **0** (gpu_id 0, shader_present 0) without a stall:
  the GPU core domain is off or isolated. mfg_rpc 0x13f90000: +0 = 0x08010030,
  +4 = 0x012b0000, +8 = 0, +0xc = 0x0fc00100.
- Next: the core power-up goes through GPUEB (gpueb-support = 1): Android's
  mtk_gpufreq / gpueb IPI drivers send it a power request over the mailbox at
  0x13c00000. Pull those modules from vendor_dlkm and read the IPI protocol
  (IPI_ID_GPUFREQ), or find whether mfg_rpc can be driven directly.

## GPUEB power IPI (kprobes on Android, 2026-11-23)

- Modules pulled to build/gpu/mod (not committed): mtk_gpufreq_wrapper,
  mtk_gpufreq_mt6878, gpueb, mtk_tinysys_ipi, mtk_rpmsg_mbox, mali_kbase.
- `gpufreq_power_control` -> `gpufreq_ipi_to_gpueb(data)` ->
  `mtk_ipi_send_compl(ipidev, 1 = IPI_ID_GPUFREQ, 1, data, 8 words, 10000)`
  -> `mtk_mbox_write(mbox 0, slot offset 4, 32 bytes)` +
  `mtk_mbox_trigger_irq(mbox 0, 0x2)`.
- Message: w0 = magic 0x02812969 (the driver reads it from GPUEB GPR 13 =
  0x13c3fd1c + 13 * 4 at probe), w1 = command (6 = CMD_POWER_CONTROL,
  8 = DVFS commit with w2 = 1, w4 = OPP index), w4 = 1 on / 0 off.
  Debug file `/proc/gpufreqv2/mfgsys_power_control` takes power_on / power_off;
  kbase itself toggles power every ~8 ms while idle.
- Mailbox (FDT gpueb): slots at 0x13c3fd80 (0x280 B), set 0x13c62004, clr
  0x13c62074, send 0x13c62000, recv 0x13c62078, IRQ 0x111.
- **tgpu v2 (dual_v30) on the phone: the GPU powers on from our code.** GPUEB
  is running after a reboot from Android (GPR13 magic 0x02812969, same as
  Android's); the power-on IPI was answered in ~0.6 ms (recv = 0x2); then
  **GPU_ID = 0xb8a31030** (Mali-G615 r1p3), L2_FEATURES 0x07120306,
  SHADER_PRESENT 0x5 (as Android's core mask), L2_PRESENT 0x1, TILER_PRESENT 0,
  GPU_STATUS 0. Milestone 1 done.

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
