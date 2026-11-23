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

- ~~Milestone 4: the same inside XNU~~ **done (patch 0091,
  device/xnu-lapis/driver/sweet_lapis_gpu.c, dual_v40, 2026-11-23).** iOS
  boot log (device/captures/sweetlog_v40_gpu.txt):
  `GPUEB power-on 5 x100us, GPU_ID 0xb8a31030` / `firmware 0, 7 sections,
  pool 0xb9000 used` / `MCU boot 2 x10us, interface 0x2050000, 8 groups` /
  `GLB config 2 ping 1, CSG start 2, CS start 1` / `command stream RAN:
  target 0xc0ffee42 after 3 x10us, AS1 fault 0` / `stopped and powered off`;
  iOS then boots as before. Firmware rides in the boot image at PA 0x43800000
  (lapis_boot_pack.py --gpufw), page pool at PA 0x58000000 (both below XNU's
  RAM). v39 failed at `firmware -4`: the header's size field (offset 16) is
  the header length (0x3c0), not the file length.
- ~~Milestone 5: the GPU stays up~~ **done (dual_v42, 2026-11-23).** No IRQ
  line in this port, so a 20 ms service thread does panthor's IRQ work
  (JOB_INT clear, GLB/CSG/CS event acks, GPU/MMU/CS fault logs); the queue
  takes any number of submissions (sweet_lapis_gpu_submit). Self-test
  (device/captures/sweetlog_v42_gpu.txt): 300 back-to-back streams all ran,
  avg 21 us, worst 74 us, ring wrapped 4 times; a stream + PING at 1/10/30/
  60/120 s all fine, MCU enabled, 0 faults; iOS boots as before.
  **Submissions must be whole 64-byte blocks (NOP padded):** v41 sent
  32-byte ones and every one at +32 in a block was skipped (extract still
  reached insert): the stream replays its stale prefetch of the block.
  lapisgpu=2 stops the GPU after the self-test.
- ~~Milestone 6: panthor's interface in the kernel~~ **done (patch 0092,
  device/xnu-lapis/driver/LapisGPU.cpp + sweet_lapis_gpu.c, dual_v47,
  2026-11-23).** IOKit service `LapisGPU`, user client selectors = 0x40 +
  panthor request (DEV_QUERY, VM_CREATE/DESTROY/BIND/GET_STATE, BO_CREATE,
  BO_MMAP_OFFSET (= client memory type), GROUP_CREATE/DESTROY/SUBMIT/
  GET_STATE, GEM_CLOSE) plus 0x7d LAPIS_WAIT {group, seq, timeout_ns}.
  VMs 1..4 have their own tables on AS 2..5 (user VA below 0xff0000000000);
  groups use CSG slots 1..7, one queue each, doorbell slot + 1; a submission
  is a 64-byte ring entry: CALL the user stream, WAIT, store the number to
  the group's sync page. Boot self-test through the same calls
  (device/captures/sweetlog_v47_gpu.txt): 200/200 streams CALLed from
  kernel memory (avg 26 us); a store to an unbound VA faults AS2, the AS is
  disabled as panthor does, wait -2 / group fatal; a new group after it
  10/10; MCU fine to 120 s, 1 fault counted. What it took:
  - v43/v44: **write-combined IOBufferMemoryDescriptors panic this port**
    (phystokv_range: illegal PA 0x4aed0000): buffers are cacheable, cleaned
    after allocation, and the CPU cleans / invalidates around the GPU
    (flush_dcache64 by VA). Programs will need a cache sync request.
  - v45: on a GPU page fault the AS must be disabled (TRANSCFG UNMAPPED +
    UPDATE) or the stream stays stalled and its group never terminates;
    never free a group's MCU pages unless the firmware acked TERMINATE
    (freeing them under it faulted AS0 and killed the MCU).
  - v46: MMU_INT bits 16..31 are REQ_COMPLETE, not faults.
  - `device/device_types.h` in an IOKit user client changes io_object_t and
    the link then misses IOUserClient::exportObjectToClient.
  - lapisgputrace=1 (+ ufslogsecs=1) logs and flushes every step.
- Next: a user-space test client (IOKit, built on CI for iOS, put into the
  iOS root: needs the PC), then the stand-in for libdrm that Mesa's panfrost
  (panthor backend) calls, a cache sync request, sync objects on 0x7d.

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
