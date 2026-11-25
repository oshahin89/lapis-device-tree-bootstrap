# dyld shared cache helpers (iOS 16.7 cache in build/dsc, not committed)

Crash addresses in sweetlog's `CRASH ... bt` are slid: unslid = addr - slide.

- `dsc_sym.py CACHE VA...` — nearest local symbol for each unslid VA (from the `.symbols` subcache).
- `dsc_find.py CACHE SUBSTR` — local symbols whose name contains SUBSTR, with addresses.
- `dsc_read.py CACHE VA LEN OUT` — LEN bytes at VA into OUT (disassemble with
  `llvm-mc --disassemble -triple=aarch64` on the bytes written as `0x..` hex).

Used for m7: backboardd's crash in Metal's `MTLPipelineLibraryBuilder::newLibraryWithFile`
(it reads `-[device targetDeviceInfo]`, NULL from `_MTLDevice`).
