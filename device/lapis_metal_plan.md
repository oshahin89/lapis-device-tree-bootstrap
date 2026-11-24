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
3. (next) command buffers, a blit and a compute kernel through Metal's own
   objects over gallium; buffers in GPU memory (liblapis_gallium).
4. Render pass: a triangle through `MTLRenderCommandEncoder`.
5. IOSurface textures; then backboardd (CoreAnimation) on the device.
