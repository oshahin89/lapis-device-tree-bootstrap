# Lapis XNU port workspace

## Direction (2026-11-11)

The port now builds on the **ios-sweet → ios-topaz → ruby-ios-bringup** line
instead of re-deriving an XNU port from vanilla `xnu-8796.141.3`.

- `ios-sweet` patches 0001–0079 (taken from the pinned ios-topaz commit
  `5095ac19dbdcf37b64b81fe443f4027ad51b84bf`) already provide what the old
  LAPIS patches 0004–0010 were working toward (open-source ARM asm/pmap, KPC
  and Apple-header stubs), plus the device-tree platform expert, boot CPU
  driver, PSCI SMP, UFS root, HFS, IOSurface/IOMobileFramebuffer and HID.
- `ruby-ios-bringup` runs the same series on a MediaTek **MT6877** (stock LK,
  no `fastboot boot`, GICv3 at `0x0c000000`, 13 MHz timer, VTIMER INTID 27 —
  identical to lapis) and reaches SpringBoard.
- The kernel keeps `MACHINE_CONFIG=SWEET`: the whole series and its stage
  scripts are gated on it. Lapis differences go into the ADT
  ([`tools/lapis_mkadt.py`](../../tools/lapis_mkadt.py)), boot-args and the
  LAPIS patches in `patches/`.

### Licensing

ios-sweet and ios-topaz had **no license file** at the pinned commits. Their
code is therefore never committed here: `ci/prepare_baseline.py` fetches the
pinned commit at build time. Keep this repository private, or get the
upstream authors' permission, before publishing anything derived from them.
`patches/0080-lapis-ufs-gpt-layout-from-ruby.patch` is ruby-ios-bringup's
`0080-ruby-ufs.patch` unchanged (an XNU modification, APSL 2.0).

## Layout

| Path | Contents |
|---|---|
| `ci/prepare_baseline.py` | fetch pinned topaz, drop topaz's SM6225 patches (`ci/topaz-patches-to-remove.txt`, same list as ruby), add `patches/`, print the patchset hash |
| `ci/strict_apply.py` | apply a queue like darwin-xnu-build, but fail on the first patch that does not apply |
| `patches/` | LAPIS delta on top of ios-sweet 0001–0079 |
| `legacy-patches/` | the earlier vanilla-XNU attempt (0001–0011); kept for reference, not built |
| `xnu/` | old `lapis-prototype` worktree of the vanilla attempt; not used by the new recipe |

## Status

- `python3 tools/lapis_xnu_patch_preflight.py` assembles the queue from
  `reference/xnu-topaz-baseline`, runs the libpthread/BearSSL/HFS stage
  scripts (network) and applies all **83 patches strictly** to a clean export
  of `xnu-8796.141.3`. Patchset hash
  `ed67418cb2fbfd856f7604097b5cd583e3d6ee948c79e255cf2fc0233701ee46`
  (86 files). This checks patch order only; it does not compile.
- `.github/workflows/xnu-lapis.yml` builds the queue on `macos-15` / Xcode
  16.0 (macos-14 and Xcode 15 were retired from GitHub-hosted runners on
  2026-11-02; ruby-ios-bringup builds the same series with Xcode 16.0) and
  fails if darwin-xnu-build skips any patch (it only warns by default).
  **First green build (2026-11-11):** run `42987441560` at `40b80c6`
  produced `kernel.development.sweet` (Mach-O arm64, xnu-8796.141.3, Xcode
  16.0) with all 80 queue patches applied; SHA-256
  `d1904ddade62244d6a3779a721e6e86629338983219a7e1c04e9eaaf42400b85`. The
  hash differs between runs (build timestamp), so it is not yet a
  reproducible-build gate. Compile artifact only; see blockers below.
- `tools/lapis_mkadt.py` writes a lapis ADT from measured values (RAM bank
  `0x140000000`+4 GiB, GICv3, 13 MHz timer, VTIMER 27, UFS `0x112b0000`, four
  A55 MPIDRs `0x000`–`0x300`). It refuses to write a phone tree; its
  framebuffer defaults are the measured values. `--qemu` writes the QEMU virt
  variant.

## QEMU boot (2026-11-11)

The CI kernel from run `42987441560` boots in QEMU `virt` (GICv3,
cortex-a76, 1 CPU) with a lapis QEMU ADT, using ios-topaz's host tools from
the local reference checkout (not committed here):

```sh
python3 tools/lapis_mkadt.py qemu.adt --qemu
python3 tools/ios-sweet/mkpayload.py kernel.development.sweet qemu.adt qemu.payload \
  --cmdline "debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress sweethw=0 wdt=-1"
# the run directory path must be short: QEMU's QMP unix socket is limited to 108 bytes
python3 reference/xnu-topaz-baseline/tools/qemu-sweet.py qemu.payload /tmp/lq --secs 60 --kernel kernel.development.sweet
```

Result after 60 s: no panic. The kernel runs at the ADT's RAM base
`0x140000000`, loads the SWEET AMFI/Sandbox stand-ins and HFS, finishes VM
bootstrap, mbuf/dlil and loopback/gif/stf, and stops at
`Waiting on <IOMedia Apple_HFS>` — expected, as QEMU has no UFS and no iOS
root was attached. `qemu.log` shows timer IRQs and one handled EL1 write
fault in `skmem_sysctl_init`. This validates the kernel and the ADT layout
against QEMU's GIC/timer, not lapis hardware.

**Run `42996879535` (2026-11-11, `ff72e27`, 82 patches incl. 0081/0082):**
green; `kernel.development.sweet` sha256 `8a7d3723…8790`. Disassembly shows
the TOPRGU store (`0x1c00a000` ← `0x22000000`) at the start of
`start_first_cpu` and the stage-0 band at `0xfdfc7000`
(= `0xfd72f000 + 1760 × 5120`). The same QEMU run as above gives the same
result: no panic, `Waiting on <IOMedia Apple_HFS>`, one handled EL1 write
fault, timer IRQs. (In QEMU virt the watchdog store lands in the empty PCIe
window.)

## First XNU runs on the phone (2026-11-11) — boots to the root wait

Image `5bb385fd…7e1c` (shim + run-42996879535 kernel, ADT `--no-ufs`,
`cpus=1 sweethw=0 wdt=-1`) flashed to `boot_b`. The user saw many lines of
white kernel console text, a blue band near the bottom, and as the bottom
line `Security policy loaded: SWEET built-in Mobile File Integrity`, which did
not change for two minutes. Power + Vol-down reached fastboot; `boot_b`
restored and verified (`851a5435…`).

So on lapis hardware LK → shim → xnu works: the kernel ran with the MMU on,
through pmap/VM bring-up and the fbcon, up to MAC policy registration at
least. The sweet fbcon **wraps to the top instead of scrolling**. Replaying the
same kernel in QEMU with the phone's exact console geometry
(`lapis_mkadt.py --qemu --qemu-lapis-fb`, 1280×2772 → 86 rows) puts that AMFI
line on the last screen row; the next line wraps to row 2 and the boot
continues there to `Waiting on <IOMedia Apple_HFS>` (row ~23 after 60 s). The
phone screen therefore matches a kernel that kept running; the top of the
screen was not read.

**Confirmed (second run, same image, 12:32):** the user read the top part of
the screen and found the `Waiting on <dict … IOMedia … Apple_HFS …>` line.
xnu 8796 therefore boots on lapis hardware — EL1 under GenieZone, GICv3
timer interrupts, scheduler, IOKit, MAC policies, HFS, networking — up to the
root-device wait, the same point as in QEMU. Recovered with Power + Vol-down →
fastboot → stock `boot_b` (`851a5435…`, Android `_b` in ~21 s).

## RAM-disk PID 1 (QEMU and phone, 2026-11-11)

With ios-sweet's static init (`tools/ios-sweet/sweet-init.S` via `mkinit.py`,
vendored MIT) as the RAM disk and `rd=md0`, the run-42996879535 kernel under
the lapis shim in QEMU (`--qemu --qemu-lapis-fb` ADT) reaches
`Added memory device md0` → `BSD root: md0` →
`load_init_program: attempting to load /sbin/launchd` →
`*** SWEET userspace: init running as PID 1 ***` → `SWEET init: still alive`
every 5 s (150 s, no panic). mockfs serves the RAM disk file as
`/sbin/launchd`; no storage is touched. Phone image (ADT `--no-ufs`):
`393c3cd3…1020`.

**On the phone (13:02, same image):** the user saw
`*** SWEET userspace: init running as PID 1 ***` and `SWEET init: still
alive` lines being added. xnu on lapis loads and execs a signed Mach-O,
switches to EL0 and back through syscalls, opens `/dev/console` (devfs) and
wakes from `select` timeouts. Recovered with Power + Vol-down → stock
`boot_b` (`851a5435…`, Android `_b` in ~18 s).

## Read-only UFS test (phone, 2026-11-11) — host left disabled by LK

Layout read from Android (read-only): `userdata` = `sdc93` on **LUN 2**,
4 KiB logical blocks, GPT header at byte 4096 (LBA 1), 94 entries from LBA 2;
simulated driver lookup: `userdata` first block `0x366800`, 230119 MiB.

Image `e6d438af…07f1`: phone ADT with `/sweet-ufs` (`0x112b0000`), boot-args
`rd=ufsroot0 ufsroot=userdata ufslun=2 sweetlog=0`, **no `ufsrw`/`rootrw`**.
Result on screen: `SWEET UFS: host not ready (hce 0x0 hcs 0x0)`, then a panic
because the root device is missing (expected with `rd=ufsroot0`). The driver
stops before ringing any doorbell, so no UFS command was sent.

Unlike ruby's MT6877 LK, **lapis LK leaves the UFSHCI disabled** (HCE = 0,
HCS = 0, likely clocks gated) when it jumps to the kernel; the ios-sweet
driver only adopts a running host. Booting a root from UFS needs a real host
bring-up (MediaTek clocks/power via SIP SMCs, HCE reset, UIC link startup,
NOP/fDeviceInit), modelled on Linux `ufs-mediatek`. RAM-disk roots keep working
meanwhile.

Checks after restoring `boot_b` (`851a5435…`): LUN 2 GPT (first 32 KiB)
hash unchanged. The first 4 KiB of `userdata` did change, but a plain Android
reboot alone also changes 16 bytes there (offset ~0xEC0; the block is not an
F2FS superblock, probably encryption metadata), so Android rewrites it on
every boot; XNU sent no command.

## SMP: four A55 cores (QEMU and phone, 2026-11-11)

The SWEET CPU driver starts secondaries with PSCI `CPU_ON` over `smc #0`
(the conduit Linux uses on lapis; GenieZone forwards it) and requires
MPIDR Aff1 numbering (`0x000`…`0x300`), which is exactly the lapis A55
layout already in the ADT. QEMU virt with GICv3 puts 16 CPUs per cluster, so
`lapis_mkadt.py --qemu --qemu-smp 4` lists MPIDR `0x000/0x100/0x200/0x300`
and `qemu_shim.py --smp 49` provides them (other CPUs stay off); `--smp > 1`
also enables EL2 so QEMU answers PSCI on `smc`, and the shim now drops
EL2 → EL1 when it is entered at EL2 (never taken on the phone, which enters
at EL1). With `cpus=4` and the RAM-disk init: `CPU_ON rc=0` and
`came alive; SGI1 OK` for CPUs 1–3, `SWEET CPU: 4/4 CPUs initialized, SGI1
verified`, PID 1 heartbeat running. One CPU still works (23 heartbeats in
150 s). Phone image (shim with the EL2 drop, `cpus=4`, `--no-ufs`, RAM-disk
init): `1d6cbc1f…640f`.

**On the phone (14:32, same image):** the user saw `SWEET CPU: 4/4 CPUs
initialized` — PSCI `CPU_ON` through GenieZone starts all four A55 cores
(MPIDR `0x000`–`0x300`) and inter-processor SGIs work. Recovered with
Power + Vol-down → stock `boot_b` (`851a5435…`, Android `_b` in ~18 s).

## SMP: all eight cores (QEMU and phone, 2026-11-11)

`lapis_mkadt.py --with-a78` adds the four Cortex-A78 cores (MPIDR
`0x400`–`0x700`) to the same homogeneous SMP cluster, as ios-sweet does with
its A55 + A76 phone (no AMP/P-cluster scheduling yet). QEMU
(`--qemu-smp 8`, `qemu_shim.py --smp 113`, `cpus=8`): `SWEET CPU: 8/8 CPUs
initialized, SGI1 verified`, PID 1 heartbeat. The default ADT still lists the
four A55s only. Phone image (`--with-a78 --no-ufs`, `cpus=8`, RAM-disk init):
`adc0d35d…6586`.

**On the phone (14:49):** the user saw `SWEET CPU: 8/8 CPUs initialized` —
all four A78 cores start too, running the 16K-page kernel. Recovered with
Power + Vol-down → stock `boot_b` (`851a5435…`, Android `_b` in ~21 s).

## HFS+ RAM-disk root (QEMU and phone, 2026-11-11)

`tools/lapis_mkroot_hfs.sh` (run with sudo; needs hfsprogs) builds an 8 MiB
HFS+ volume `SweetRoot` with `/sbin/launchd` = ios-sweet's init (local output
`build/root-hfs.img`, sha256 `833c808e…`, not committed). As the RAM disk
with `rd=md0`, QEMU shows `BSD root: md0` → `hfs: mounted SweetRoot on device
b(1, 0)` → `load_init_program: /sbin/launchd` → `init running as PID 1`
(built-in Apple OSS HFS, patches 0026/0027). Same format the iOS root will use.

**On the phone (15:52, image `468dc8fd…2a45`, 8 cores, `--no-ufs`):** the user
saw `hfs: mounted SweetRoot` and `init running as PID 1`. Recovered with
Power + Vol-down → stock `boot_b`. That restore boot then hit an unrelated
**stock Linux oops** after 398 s (NULL dereference in
`rt_mutex_adjust_prio_chain` from a userspace `sched_setattr`; stock kernel,
clean LK boot, no lapis code involved) and rebooted itself
(`sys.boot.reason = kernel_panic`). The next boot is normal; `boot_b` reads
back `851a5435…`. The oops log was recovered from
`/sys/fs/pstore/console-ramoops-0`, so pstore does survive a warm
(panic) reboot — only the Power + Vol-down reset loses it.

## Patch 0083: pstore console mirror and panic warm reset (2026-11-11, offline)

With the ADT node `/lapis-debug` (`lapis_mkadt.py --debug`: `pstore-reg` =
ramoops console zone `0x480af000`+`0x40000`, `rgu-reg` = TOPRGU), the sweet
fbcon also writes every console byte into that zone as a ring in Linux's
`persistent_ram_buffer` format, and `panic_spin_forever()` calls
`lapis_panic_reboot()`: with boot-arg `lapispanicreboot=N` it waits N s and
resets into LK fastboot (reboot mode 3 + PSCI `SYSTEM_RESET`, as Linux on
arm64; proven on the phone with `pstest` v4). The PSCI reset is cold, so the
pstore copy is lost on the phone; it is useful in QEMU and for warm resets.
Without the node or the boot-arg nothing changes.

**CI run `43179013523` (`07eab02`, PSCI version of 0083):** green, kernel
`0335d027…ef40`. QEMU forced panic (`rd=ufsroot0` without UFS,
`lapispanicreboot=5`): with EL2 (`qemu_shim.py --smp 2`) the machine resets
through PSCI after the panic; without EL2 the pstore copy shows the panic
string and `lapis: reset to fastboot (PSCI SYSTEM_RESET)`. Phone test image
(same panic on purpose, 8 cores, `--no-ufs --debug`, `lapispanicreboot=15`):
`build/xnu_panicreboot_boot.img` `04f5aae0…d153` — expected: xnu console,
panic text, then back to fastboot alone about 15 s later.

**CI run `43037314359` (`9bf5bec`, 83 patches):** green, kernel
`a4374bc0…6ed9`. QEMU with `--debug` and the phone console geometry:
normal HFS boot → the pstore zone decodes (Linux's algorithm, 129 lines) to
the full console log incl. `hfs: mounted SweetRoot`, PID 1 and the heartbeat;
forced panic (`rd=ufsroot0`, no UFS node) → the zone ends with the panic
string and `lapis: warm reset to fastboot` after `lapispanicreboot=5` s (the
reset itself is a no-op in QEMU).

## Open blockers before any phone experiment

1. **Early framebuffer constants — patched, not yet run.** ios-sweet
   0010/0012 hard-code sweet's framebuffer (`0x9c000000`, stride 4320).
   `patches/0081-lapis-early-framebuffer.patch` switches them to the measured
   lapis values: scan-out buffer `0xfd72f000`, ARGB8888, stride 5120,
   1280 wide. Only one 32 MiB block (`0xfc000000`) is mapped, and the buffer
   starts `0x172f000` into it, so the early console uses the first 1792 rows.
2. **Boot shim / image — written, QEMU-tested.** `tools/lapis_shim/` (console
   and loader derived from ios-sweet's MIT shim) is an arm64 Image that LK
   loads at `0x40000000`; it clears the TOPRGU watchdog, prints the entry
   state on the measured framebuffer, finds the SWEETXNU payload appended at
   the next 4 KiB boundary, refuses kernel RAM outside the measured free bank
   `0x140000000–0x32df00000`, copies segments/ADT/boot_args (Video = LK
   buffer) and jumps to xnu at EL1 with the MMU off.
   `lapis_boot_pack.py STOCK OUT --image Image --append payload` packs it into
   a boot_b copy (whole Image LZ4-compressed, verified by decompression).
   `tools/lapis_shim/qemu_shim.py` runs the shim+payload in QEMU virt: the
   shim console, then xnu's early console (`SWEET EARLY 1`…`20`, i.e. patch
   0081) on the lapis framebuffer, then the same boot as the direct QEMU run.
3. **Recovery proof.** Show that flashing the exact factory image back to a
   boot slot restores Android before writing any experiment image.
4. **Watchdog — patched; the same store is confirmed on the device.** LK leaves the MT6878 TOPRGU
   (`0x1c00a000`) armed; the parked probe was reset five times.
   `patches/0082-lapis-disable-toprgu-watchdog.patch` clears WDT_MODE with
   the `0x22` key at the very start of `start_first_cpu` (MMU off), the same
   store ruby makes on MT6877. **Confirmed on the device** with the
   `-DDISABLE_WDT` probe: no resets for two minutes, and Power + Vol-down
   still reaches fastboot with the watchdog off.
5. **UFS host bring-up**: LK leaves the host disabled (see above); an
   `iosroot` partition also needs a GPT change (dual boot) — and the A78
   cluster is still kept out of the ADT.
