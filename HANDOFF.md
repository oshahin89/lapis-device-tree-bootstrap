# Handoff: continuing the lapis iOS port

For whoever picks this up next. Current state and evidence:
[device/lapis_status.md](device/lapis_status.md).

## Big picture

```
Xiaomi preloader → LK (unlocked, slot b) → boot_b: tools/lapis_shim (arm64 Image)
  → SWEETXNU payload: xnu-8796.141.3 (MACHINE_CONFIG=SWEET, ios-sweet 0001–0079
    + LAPIS 0080–0083) + lapis ADT (+ RAM disk) → PID 1
```

- The kernel is built by CI from the pinned ios-topaz commit plus
  `device/xnu-lapis/patches/` (`ci/prepare_baseline.py`, strict apply). The
  ios-sweet/topaz trees are fetched at build time; topaz has no license file,
  so its code is not committed here. ios-sweet is MIT since 2026-10-22; its
  host tools are vendored in `tools/ios-sweet/`.
- Lapis-specific hardware values come from the ADT (`tools/lapis_mkadt.py`),
  the shim, boot-args and patches 0081–0083.

## Build and run loop

```sh
# kernel: CI (needs gh auth), or download the latest artifact
gh workflow run xnu-lapis.yml --ref main -f kernel_config=DEVELOPMENT

# shim
tools/lapis_shim/build.sh build/shim shim

# ADT + payload (RAM-disk root from tools/lapis_mkroot_hfs.sh)
python3 tools/lapis_mkadt.py build/phone.adt --no-ufs --with-a78 --debug
python3 tools/ios-sweet/mkpayload.py KERNEL build/phone.adt build/payload.bin \
  --ramdisk build/root-hfs.img \
  --cmdline "debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress sweethw=0 wdt=-1 rd=md0 lapispanicreboot=15"

# boot_b image (verifies itself against the stock image)
python3 tools/lapis_boot_pack.py device/slot_b_readback/boot_b.img build/test_boot.img \
  --image build/shim/Image --append build/payload.bin

# QEMU first (phone console geometry; --smp 2 enables PSCI)
python3 tools/lapis_mkadt.py build/qemu.adt --qemu --qemu-lapis-fb --debug
python3 tools/lapis_shim/qemu_shim.py IMAGE /tmp/run --secs 150 --kernel KERNEL --pstore
```

On the phone (each flash needs the owner's OK): `adb reboot bootloader`,
`fastboot flash boot_b build/test_boot.img`, `fastboot reboot`, read the
screen (the sweet fbcon wraps to the top; look for the blank cursor line).
Recover: a panicking image with `lapispanicreboot=N` returns to fastboot by
itself; otherwise Power + Vol-down. Then flash
`device/slot_b_readback/boot_b.img` and check its root sha256
`851a5435…ae4f5`.

## Hard-won facts (each measured on the phone)

- Kernel entered at EL1 under GenieZone (stage 2), MMU off, at `0x40000000`.
- Free RAM bank for xnu: `0x140000000`–`0x32df00000` (ADT uses +4 GiB).
- LK framebuffer: buffer 0 `0xfd72f000`, ARGB8888, stride 5120.
- LK leaves the TOPRGU watchdog armed: clear WDT_MODE (`0x1c00a000` ← `0x22000000`).
- Return to fastboot: NONRST_REG2 `+0x24` mode 3 with bits `0x820` cleared,
  then PSCI `SYSTEM_RESET`. A direct `WDT_SWRST` does not reach fastboot.
- pstore console zone `0x480af000` survives warm resets only.
- UFS: LUN 2, 4 KiB blocks, GPT at LBA 1; host IP `0x10420200` (MT6878),
  refclk on, HCE = 0 at LK's jump.
- Touch: NT38771 on SPI3 GPIO 68–71, chip ID `71 87 03` read with SPI mode 0
  bit-bang; flashless, firmware `device/novatek_ts_fw_tm.bin`.
- The stock Android kernel has a recurring oops in `rt_mutex_adjust_prio_chain`
  (AEE logs since 2026-10-27, before any experiment); an extra reboot after a
  restore is that bug, not the port.

## Next work, in dependency order

1. **UFS host bring-up** (blocks the iOS root): follow the upstream Linux
   `ufs-mediatek.c` path for `IP_VER_MT6878`, starting from LK's state
   (refclk already on). See [device/lapis_ufs_bringup.md](device/lapis_ufs_bringup.md).
   Then the ios-sweet driver adopts the running host (`rd=ufsroot0`).
2. **iOS root**: an `iosroot` partition (GPT change; dual boot is possible
   with slot a/b, see ruby's `appendix-dualboot.md`) and an HFS+ image built
   from a user-supplied IPSW (topaz/sweet `mkiosroot.sh`).
3. **Touch**: NT38771 firmware download
   ([device/nt38771_host_download.md](device/nt38771_host_download.md)),
   then report reading into `SweetHID` (`device/nt38771_*.c` parser exists;
   SweetHID's logical size must become 1280×2772).
4. Keys, USB, display power, AMP scheduling, then the rest.

## Do not

- Commit `lapis_global_images_*/images/`, `reference/` or `build/` (gitignored).
- Touch GPIO 90 (shared panel/touch reset).
- Write to any storage without the owner's explicit OK and a restore plan;
  `userdata` has no backup.
