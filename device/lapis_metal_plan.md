# lapis Metal — plan

Started 2026-11-24, after Mesa's gallium panfrost drew on the G615 from an iOS
program (device/lapis_gpu_plan.md, milestone 10: a triangle, 481 us a frame).
iOS still draws with the CPU (topaz's software CoreAnimation path); this is the
work that lets CoreAnimation draw on the GPU.

## How hakios did it (hakios-xnu-kernel, components/metal, MIT for plugin/)

- Metal.framework finds a GPU driver through IOKit: the accelerator service
  (`IOAcceleratorES`) carries `MetalPluginName` and `MetalPluginClassName`;
  Metal loads `/System/Library/Extensions/<Name>.bundle` and makes the device
  from that class. hakios publishes the two properties behind a boot-arg
  (`metal=1`) and only shows the device to the window server by default.
- Their plug-in (`HakiosMetal.m`, `HakiosMetalCA.m`) subclasses Metal's private
  base classes (iOS 26/27, arm64e) over a C core (`hakios_metal.h`): device,
  buffers, textures, samplers, libraries, pipelines, queues, command buffers,
  blits, compute, render passes, IOSurface textures.
- The core is Adreno-specific (their own command streams, no Mesa driver);
  Mesa is only the compiler: Apple's AIR (LLVM bitcode in .metallib) -> NIR by
  their front end (`tools/mesa/air`), NIR -> ir3 by Mesa.
- Result on their Adreno phone: Liquid Glass on iOS 27 at 60-80 fps.

## lapis: the same plug-in shape, the core over gallium panfrost

- iOS 16.7's Metal has the same discovery strings (`MetalPluginName`,
  `MetalPluginClassName`, `IOAcceleratorES` in the image), and the iPhone X's
  own driver bundle `AGXMetalA11`.
- Core: `hakios_metal.h`'s operations implemented over Mesa's gallium
  (liblapis_gallium, already working): buffers/textures = pipe_resource,
  programs = NIR (hakios's AIR front end) -> pipe shader states (panfrost
  compiles NIR), draws = draw_vbo, compute = launch_grid, blits = copy/clear.
- Plug-in: hakios's, adapted to iOS 16's private Metal classes (arm64, not
  arm64e), learnt on the phone from the runtime itself.
- Delivery without the PC: the program slots of tools/lapis_rootslot.sh; the
  bundle most likely over `AGXMetalA11.bundle`'s executable (Apple's A11
  driver, which can never run on a Mali).

## Milestones

1. ~~Probe~~ done (2026-11-24, device/captures/metalprobe_m1_ios16.txt):
   no accelerator service, backboardd keeps matching `IOAcceleratorES`,
   `MTLIOAccelDevice < _MTLDevice`, the private classes with their methods.
2. ~~Metal gives our device~~ **done (v50 + LapisMetal, 2026-11-24,
   device/captures/sweetlog_m2e_metal.txt).** The kernel publishes
   `IOAcceleratorES` (boot-arg `lapismetal=1`: the device shown to the test
   program only); Metal loads AGXMetalA11.bundle, whose executable is now our
   dylib over `ds.g10p`; `MTLCreateSystemDefaultDevice()` returns
   `LapisMetalDevice "Mali-G615 MC2 (lapis, Mesa panfrost)"`, a queue, and
   buffers (1024/1024 words read back). backboardd and SpringBoard load the
   plug-in and are not shown the device; iOS runs as before. What it took:
   - Metal takes the class from the bundle's `-principalClass` (iOS 16), and a
     class made at run time is in no image: the plug-in answers
     `-principalClass` / `-classNamed:` for that bundle itself.
   - Queues and buffers are made with plain `init` and freed with NSObject's
     `-dealloc`: Metal's `-initWithDevice:` / `-dealloc` need state only an
     `MTLIOAccelDevice` has.
   - ARC retains a C function's `id self`: every `-dealloc` IMP takes it
     `__unsafe_unretained`, or it is released after being freed.
3. ~~Command buffers and blits on the GPU~~ **done (2026-11-25,
   device/captures/sweetlog_m3_metal.txt).** liblapis_gallium exports
   `lapis_gpu_*` (persistently mapped buffers, copy, fill, flush, fence
   wait), which LapisMetal reaches by dlopen: buffers in GPU memory,
   `LapisMetalCommandBuffer` (commit = one submission, its fence waited for
   off-thread, then the completed handlers) and
   `LapisMetalBlitCommandEncoder`. Through Metal's own API: a 1 MiB copy
   262144/262144 words, fills (one unaligned) 65536/65536 bytes, status
   Completed seen by the handler, 100 command buffers right (5.6 ms each at
   boot; every submission syncs the CPU caches over all of Mesa's maps,
   2.4 MiB).
4. ~~Compute kernels~~ **done (2026-11-25, device/captures/sweetlog_m4_compute.txt).**
   hakios's AIR -> NIR front end (tools/mesa/air) ported to Mesa 26.3.1's NIR
   (`fp_math_ctrl` for `exact`, `nir_def_instr`, `mesa_shader_stage_name`,
   the new block/shader/constant-eval signatures) and built into
   liblapis_gallium (`lapis_gpu_metallib_*`, `lapis_gpu_compute_*`,
   `lapis_gpu_dispatch`). LapisMetal: `newLibraryWithData:` / URL / file /
   default library, `newFunctionWithName:`, `newComputePipelineStateWithFunction:`,
   and a compute encoder (pipeline, buffers by Metal index, `setBytes` as a
   small GPU buffer, `dispatchThreadgroups` / `dispatchThreads`). Three kernels
   compiled by Apple's `metal` on CI (tools/lapis_metal/lapis_test.metal), right
   on the first run on the G615: `lapis_madd` (device buffers and a constant)
   4096/4096, `lapis_ids` (grid/group/thread ids) 4096/4096, `lapis_sum`
   (sqrt, threadgroup memory, barriers, reduction) 64/64. Pipelines compile in
   2.5-14 ms, a dispatch round trip is 0.7-0.8 ms.
   Not yet: dynamic threadgroup memory (`setThreadgroupMemoryLength:`),
   non-uniform threadgroups, textures/samplers in kernels. The cache sync on
   every submission (all of Mesa's maps, ~2.6 MiB) dominates small command
   buffers (21 ms each this boot, measured early while iOS was still starting).
5. Render pass: a triangle through `MTLRenderCommandEncoder`.
6. IOSurface textures; then backboardd (CoreAnimation) on the device.
