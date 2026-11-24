# lapis dual boot (Android + iOS from one boot_b)

**This is the official way to ship iOS builds for lapis.** Every new payload
goes out as a dual image:

    tools/lapis_dual_image.sh build/payload_X.bin build/lapis_dual_X.img

and the user flashes it from rooted Android
(`dd if=lapis_dual_X.img of=/dev/block/by-name/boot_b bs=4M && sync && reboot`).
Single-OS images (`--image --append` without `--android`) and shim probes are
for bring-up experiments only. Recovery is unchanged: Power + Vol-down into
LK fastboot, then flash `device/hyperos4_readback/boot.img` (`a01d0f11…`) to
boot_b from the PC or from a second phone (Termux, `termux-usb`).

Working on the phone on 2026-11-22 (`build/lapis_dual_v1.img`, sha256 `1073ab60…`):
Android starts by default, iOS when Volume up is pressed while the shim's
"Vol+ now = iOS" line is on screen, and a reboot from iOS comes back to Android.
No PC is needed after the first flash; it is flashed from rooted Android with
`dd if=dual_v1.img of=/dev/block/by-name/boot_b`.

Idea from hakios-mt6768's mtkloader: the boot image's kernel is the lapis shim,
the SWEETXNU payload behind it, then the stock Android Image.

- Build: `tools/lapis_shim/build.sh $PWD/build/dual_build dual` (shim.c +
  dual.c, `-DLAPIS_DUAL`), then
  `tools/lapis_boot_pack.py build/hyperos4_b/boot.img OUT.img --image build/dual_build/Image --append PAYLOAD --android`.
- Layout (decompressed, loaded by LK at 0x40000000): shim | payload at the next
  4 KiB | Android Image at the next 2 MiB boundary (0x1200000 with the v18
  payload) | 64 KiB stub area. Header res2/res3/res4 (0x20/0x28/0x30) = Image
  offset, length, stub area offset. Total 0x340d000, below LK's DTB at 0x47c80000.
- `dual_pick_ios()`: one console line over LK's logo, then Volume up (MT6363
  TOPSTATUS 0x1e bit 3, clear while down, PMIF SWINF ch 2) polled every 20 ms
  for 2 s, 3 consecutive "down" reads = iOS. PMIF errors count as not pressed.
- Android path: the watchdog is left armed as LK left it (start.S skips its
  WDT store under LAPIS_DUAL); `chain_stub` (start.S) is copied to the stub
  area, moves the Image down onto the shim's load address, `ic iallu`, enters
  it with x0 = LK's DTB, x1..x3 = 0.
- iOS path: `dual_wdt_off()` (same store as start.S), then the normal shim.
- Holding Volume up from power-on may make LK pick recovery; press it only once
  the line shows.

## Probes without a PC (dual_v2 and later)

`tools/lapis_dual_image.sh PAYLOAD OUT.img PROBE` builds a shim probe
(`tvib`, `tdsi`, … — any probe whose END returns under `LAPIS_DUAL_PROBE`) into the
dual shim: the prompt reads "Vol+ = iOS  Vol- = test  (else Android)", and
Volume down (GPIO 54, active low, 3 reads) runs the probe after stopping the
watchdog. When the probe ends, a shim error or an exception, the screen says
"press Power = reboot to Android": Power (TOPSTATUS bit 1) pressed and released
→ TOPRGU reboot mode 0 + PSCI SYSTEM_RESET → LK → the shim → Android.
QEMU (`DUAL_TEST=1|2 build.sh … dual PROBE` forces the XNU / probe path) ran
all three paths. `build/lapis_dual_v2.img` = v18 payload + `tvib`
(`9999675a…`).

## iOS log without a PC

From rooted Android (Termux), the sweetlog tail of the last iOS boot:

    su -c 'dd if=/dev/block/sdc92 of=/sdcard/Download/sweetlog.bin bs=4096 skip=4193279 count=1025'

then send `sweetlog.bin`; decode with
`reference/xnu-topaz-baseline/tools/sweet-log.py`.
