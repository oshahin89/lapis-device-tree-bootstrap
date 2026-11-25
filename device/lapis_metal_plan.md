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
5. ~~Render pass~~ **done (2026-11-25, device/captures/sweetlog_m5c_render.txt;
   m5 and m5b logs beside it).** liblapis_gallium: 2D textures in Metal pixel
   formats (`lapis_gpu_tex_*`, linear, read back through transfer maps),
   render pipelines from a vertex and a fragment AIR function, passes
   (`lapis_gpu_pass_begin`: clear or keep the colour attachment; Metal's
   z in [0, 1] via clip_halfz and y up via a negative viewport y scale), draws
   with each stage's buffers by Metal index. LapisMetal: `newTextureWithDescriptor:`,
   `getBytes:` / `replaceRegion:`, `newRenderPipelineStateWithDescriptor:`,
   `renderCommandEncoderWithDescriptor:` with vertex/fragment buffers and
   bytes and `drawPrimitives:`. On the G615 through Metal's own API: the red
   triangle ff0000ff, covering 20808 pixels (20972 by area, edges excepted),
   apex at the top (rows 27-229); interpolated colours; constant and device
   buffers in the fragment stage; 100 frames at 2.1 ms each.
   What it took: panfrost's fragment shaders read gallium constant buffer 0
   as zero (m5 drew 00000000, m5b isolated it to `constant [[buffer(0)]]` in
   the fragment stage), so Metal's constant buffer n is gallium's n + 1 in
   every stage (a NIR pass on load_ubo). Each render pipeline's NIR is in the
   log (lapis_gpu_render_nir) for the next shader that goes wrong.
   And air_to_nir leaves system values as derefs of `system` variables
   (Metal's `[[position]]` in a fragment shader), which panfrost's compiler
   crashed on (m5c: SIGSEGV in bi_alu_src_index); `nir_lower_system_values`
   runs first now, as gallium's own frontends do. m5d
   (device/captures/sweetlog_m5d_render.txt): every check right, `[[position]]`
   included, through to the end.
   Not yet: `[[stage_in]]` vertex attributes (vertex descriptors), viewport
   and scissor, depth/stencil, blending, textures and samplers in shaders,
   indexed draws, several colour attachments, MSAA.
6. ~~Textures and samplers in shaders, vertex descriptors, blending, viewport,
   scissor, indexed draws~~ **done (2026-11-25, device/captures/sweetlog_m6_textures.txt),
   every check right on the first run.** liblapis_gallium: sampler states
   (MTLSamplerDescriptor and the shader's own constexpr samplers, after
   hakios's air_run.c), sampler views made on a texture's first use, Metal's
   null texture as a 1x1 zero texture, blend state per pipeline, `[[stage_in]]`
   vertex elements by the front end's driver_location with each Metal buffer
   given a vertex-buffer slot, viewport/scissor/blend colour, indexed draws
   (`lapis_gpu_draw2`). On the phone: a 4x4 texture over the target through an
   API nearest sampler and a vertex descriptor, 65536/65536 pixels their texel;
   a constexpr linear sampler between black and white, 7f/80 in the middle;
   indexed quad 65536/65536; viewport {0,0,128,128} 16384 in the top-left
   quarter; scissor {64,64,64,64} 4096 inside; source-alpha blending
   ff7f007f.
   Seen on the way: the per-submission cache sync grows with Mesa's maps
   (140 maps, 4.9 MiB by the end of this test), the next cost to cut.
   **Speed, p1 (2026-11-25, device/captures/sweetlog_p1_speed.txt):** the cost
   was lapis-drm's fence handling, not the cache flush (~150 us): every poll
   of a fence was a kernel round trip (~76 us) even for one long done, and
   every successful wait, Mesa's BO-cache probes included, synced all the
   caches. Now done fences are remembered per group (a group's submissions
   complete in order) and a wait syncs only when the GPU finished something
   new. A command buffer 38.6 ms -> 0.224 ms at boot, 0.140 ms settled; the
   1 MiB copy 0.23 ms; compute dispatches 0.15-0.24 ms; cache syncs over the
   whole test ~1300 -> ~100; every check still right. All 1600 instant probes
   found their buffer done. Still slow: a render frame (3.7 ms), which makes
   a new BO and VM_BIND each (139 of each per 500 requests, maps growing).
   **p2/p3 (device/captures/sweetlog_p2_speed.txt, sweetlog_p3_speed.txt):**
   the rest of a frame's cost was LapisMetal itself: every setBytes made a GPU
   buffer (BO, VM_BIND, map, ~330 us). setBytes now copies into 64 KiB upload
   arenas a command buffer takes from a pool and gives back on completion;
   Mesa's maps stay at 32-37 instead of growing. Settled (after 20 s, iOS
   idle): a command buffer 0.149 ms, **a frame (pass clear + quad +
   setFragmentBytes, 256x256) 0.509 ms**, colours right. Numbers taken during
   iOS's boot swing widely (1-200 ms) from CPU contention on the 4 A55s.
7. **CoreAnimation (backboardd) on the device, in progress (2026-11-25, m7-m7n
   logs in device/captures/).** backboardd is shown the device behind a crash
   guard (2 starts a boot, keyed by kern.bootsessionuuid: kern.boottime is the
   same every boot, no RTC). IOSurface textures by copy work (test: 64x64
   BGRA surface right). What CoreAnimation needed so far, each found from its
   own code (tools/dsc symbolizes the shared cache) or its own log
   (QuartzCore's x_log to stderr through x_set_log_filename, called by
   address): the device hides -targetDeviceInfo (Metal's pipeline-library
   builder read NULL+0x10); depth/stencil states; function constants
   (recorded on MTLFunctionConstantValuesInternal, given to the AIR front end
   by index or name); function descriptors; mipmapped and 1D textures; the
   common pixel formats; texture copies; mipmap generation; setGPUPriority:.
   It then compiled ~20 of QuartzCore's pipelines and warmed up, but drew no
   frame: its IOMFB display gives a 'b3a8' framebuffer (RGB10A8_2P_XR10, two
   planes) and four_cc_to_mtl_format maps that only with GPU family Apple3
   (limits bit 0x100, set in new_metal_context from supportsFamily:1003;
   _MTLDevice says Apple2). With supportsFamily: up to Apple3 (m7m, m7n) the
   phone resets silently within a second after the warm-up, before our first
   IOSurface texture or commit (checkpoints): most likely a kernel panic
   (panics are not kept: gPanicBase is not set) in what follows the format
   check, is_viable_texture_from_iosurface on the two-plane surface, i.e. the
   IOSurface driver (SWEET SURF). Next: keep kernel panics (sweetlog), and/or
   a 2-plane 'b3a8' surface exercised from the test program step by step.
8. IOSurface textures without copies; then CoreAnimation at speed (CoreAnimation) on the device.
