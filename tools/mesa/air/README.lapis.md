# AIR -> NIR front end (from hakios)

Apple's Metal shader bitcode (AIR, the LLVM bitcode inside a `.metallib`) to
Mesa's NIR. Taken unchanged in substance from hakios-xnu-kernel,
`components/metal/plugin/tools/mesa/air` (MIT, "Darwin on redwood
contributors"; see each file's header), where it feeds Mesa's ir3 for
Adreno. On lapis the NIR goes to Mesa 26.3.1's panfrost for the Mali-G615.

`tools/mesa/build-mesa26-ios.sh` copies this directory to `src/compiler/air`;
`tools/mesa/patches26` adds the `subdir('air')` and links it into the
`lapis` gallium target. Changes made here for Mesa 26's NIR are listed in
the commit history of this directory.

Bindings (nir_air.h): a `device` buffer is the SSBO at Metal's buffer index,
a `constant` buffer the UBO at that index, textures/images/samplers at
theirs; threadgroup memory is shared memory sized at dispatch.
