# ios-sweet host tools (vendored, MIT)

Unmodified copies from [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)
at commit `45263bee51df4a0f55cdf9c649af886399488ddb` (MIT License since
`067c9b3`, see `LICENSE`):

| File | Use here |
|---|---|
| `mkpayload.py` (+ `adt.py`) | pack `kernel.development.sweet` + an ADT (+ `--ramdisk`) into the SWEETXNU payload the lapis shim loads |
| `mkinit.py` + `sweet-init.S` | first userspace program: a static, ad-hoc signed arm64 Mach-O PID 1 that prints to `/dev/console` and a 5 s heartbeat |

Build the init (needs a clang with Mach-O support, e.g. Ubuntu clang 21):

```sh
clang -target arm64-apple-ios16.0 -c tools/ios-sweet/sweet-init.S -o sweet-init.o
python3 tools/ios-sweet/mkinit.py sweet-init.o sweet-init
python3 tools/ios-sweet/mkpayload.py KERNEL ADT payload.bin --ramdisk sweet-init \
  --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress sweethw=0 wdt=-1 rd=md0"
```

With `rd=md0` and a single file as the RAM disk, the kernel's mockfs serves
that file as `/sbin/launchd`; nothing is read from or written to storage.
The ios-topaz tree (no license) is not used for these steps.
