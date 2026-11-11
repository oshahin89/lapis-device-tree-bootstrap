# Lapis iOS-port status (end of 2026-11-11)

## Working on the phone (each run flashed to `boot_b`, restored afterwards)

| Milestone | Image | Evidence |
|---|---|---|
| Framebuffer measured: ARGB8888, buffer 0 `0xfd72f000`, stride 5120 | probe `a66e4e30…` | screen pattern |
| TOPRGU watchdog disable; Power + Vol-down still recovers | probe `e4c7d108…` | no resets for 2 min |
| LK → `tools/lapis_shim` → xnu 8796, up to `Waiting on <IOMedia Apple_HFS>` | `5bb385fd…` | screen (wrapped console) |
| Userspace PID 1 from a RAM disk (`rd=md0`, mockfs) | `393c3cd3…` | `init running as PID 1`, heartbeat |
| SMP: 4× A55 | `1d6cbc1f…` | `4/4 CPUs initialized` |
| SMP: 4× A55 + 4× A78 | `adc0d35d…` | `8/8 CPUs initialized` |
| HFS+ RAM-disk root (`SweetRoot`) | `468dc8fd…` | `hfs: mounted SweetRoot`, PID 1 |
| UFS host state read at LK's jump | probe `407f270d…` | IP_VER `0x10420200` = MT6878, refclk on, HCE = 0 |

Images live in the session scratchpad / `build/` (not in git); every one can
be rebuilt with the tools below from the CI kernel.

## Not working / open

- **UFS root**: LK leaves the host disabled; needs a MediaTek host bring-up
  (study in [lapis_ufs_bringup.md](lapis_ufs_bringup.md)). Not written.
- **pstore across Power + Vol-down**: lost (LK reinitialises it). It survives
  a warm reset (a stock Linux panic log was read from it).
- **Warm reset into fastboot without keys** (`tools/lapis_shim/pstest.c`,
  image `727eb96d…`): flash was interrupted on 2026-11-11 before the result
  was seen. Phone state after that was unknown when the user left.
- Touch (NT38771 on SPI3, reset GPIO 90 shared with the panel), keys, USB,
  display power, AMP scheduling: not started.

## First things next session

1. Check the phone: `adb devices` / `fastboot devices`; if it is not in
   Android, Power + Vol-down → `fastboot flash boot_b
   device/slot_b_readback/boot_b.img && fastboot reboot`; then root
   `sha256sum /dev/block/by-name/boot_b` must be `851a5435…ae4f5`.
2. Re-run the `pstest` probe (warm reset to fastboot, pstore marker).
   Prepared: `build/pstest_boot.img` (`727eb96d…`).
3. If it works: flash `build/xnu_debug_boot.img` (`8c8fb61a…`: CI kernel
   `a4374bc0…` with patch 0083, ADT `--no-ufs --with-a78 --debug`, HFS RAM-disk
   root, `cpus=8 rd=md0 lapispanicreboot=15`). It boots to PID 1 and stays up;
   the console is mirrored into pstore. Recover with Power + Vol-down as
   usual, or — once the warm reset is proven — trigger a panic image to test
   the automatic return. After a warm return, read
   `/sys/fs/pstore/console-ramoops-0`. (`build/` is gitignored.)

## Tools

| Tool | Purpose |
|---|---|
| `tools/lapis_xnu_patch_preflight.py` | strict patch-queue check (83 patches) |
| `.github/workflows/xnu-lapis.yml` | CI kernel build (macos-15, Xcode 16.0) |
| `tools/lapis_mkadt.py` | ADT (`--no-ufs`, `--with-a78`, `--debug`, `--qemu…`) |
| `tools/ios-sweet/` | vendored MIT `mkpayload`, `mkinit`, `sweet-init.S` |
| `tools/lapis_mkroot_hfs.sh` | HFS+ root image (sudo, hfsprogs) |
| `tools/lapis_shim/` | LK boot shim + probes (`build.sh OUT shim|regdump|pstest`), `qemu_shim.py` |
| `tools/lapis_boot_pack.py` | pack into a boot_b copy (`--probe` / `--image --append`) |
