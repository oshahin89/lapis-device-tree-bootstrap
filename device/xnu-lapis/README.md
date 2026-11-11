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
  scripts (network) and applies all **82 patches strictly** to a clean export
  of `xnu-8796.141.3`. Patchset hash
  `14f8402b7528a51a1710e76055493b2905eda257e073ea6a376f319bb3054408`
  (85 files). This checks patch order only; it does not compile.
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
python3 reference/xnu-topaz-baseline/tools/mkpayload.py kernel.development.sweet qemu.adt qemu.payload \
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

## Open blockers before any phone experiment

1. **Early framebuffer constants — patched, not yet run.** ios-sweet
   0010/0012 hard-code sweet's framebuffer (`0x9c000000`, stride 4320).
   `patches/0081-lapis-early-framebuffer.patch` switches them to the measured
   lapis values: scan-out buffer `0xfd72f000`, ARGB8888, stride 5120,
   1280 wide. Only one 32 MiB block (`0xfc000000`) is mapped, and the buffer
   starts `0x172f000` into it, so the early console uses the first 1792 rows.
2. **Boot shim / image.** Lapis LK loads an Android boot **v4** image with an
   **LZ4** kernel (ruby: v2 + gzip; topaz: v4 shim via Qualcomm ABL). A lapis
   shim and packer, plus the LK handoff contract, are still to be written.
3. **Recovery proof.** Show that flashing the exact factory image back to a
   boot slot restores Android before writing any experiment image.
4. **Watchdog — patched, not yet run.** LK leaves the MT6878 TOPRGU
   (`0x1c00a000`) armed; the parked probe was reset five times.
   `patches/0082-lapis-disable-toprgu-watchdog.patch` clears WDT_MODE with
   the `0x22` key at the very start of `start_first_cpu` (MMU off), the same
   store ruby makes on MT6877. To be confirmed on the device first with the
   `-DDISABLE_WDT` probe (no resets in expdb; Power + Vol-down still recovers).
5. UFS LUN/GPT layout for the `iosroot` partition (`ufslun=` boot-arg) and the
   A78 cluster (kept out of the ADT for now).
