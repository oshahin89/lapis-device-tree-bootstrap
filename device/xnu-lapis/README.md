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
  scripts (network) and applies all **80 patches strictly** to a clean export
  of `xnu-8796.141.3`. Patchset hash
  `cd347d9465809033ba792c1cebbea045efe9b9621466d4a7f3b00455abc1902b`
  (83 files). This checks patch order only; it does not compile.
- `.github/workflows/xnu-lapis.yml` builds the queue on `macos-15` / Xcode
  16.0 (macos-14 and Xcode 15 were retired from GitHub-hosted runners on
  2026-11-02; ruby-ios-bringup builds the same series with Xcode 16.0) and
  fails if darwin-xnu-build skips any patch (it only warns by default).
- `tools/lapis_mkadt.py` writes a lapis ADT from measured values (RAM bank
  `0x140000000`+4 GiB, GICv3, 13 MHz timer, VTIMER 27, UFS `0x112b0000`, four
  A55 MPIDRs `0x000`–`0x300`). It refuses to write a phone tree without
  `--fb-stride`, which has not been measured. `--qemu` writes the QEMU virt
  variant.

## Open blockers before any phone experiment

1. **Early framebuffer constants.** ios-sweet 0010/0012 hard-code sweet's
   framebuffer (`0x9c000000`, stride 4320); ruby replaces them with MT6877
   values. Lapis needs its own LK visible-layer address and pitch (reserved
   region `0xfd72f000`+`0x28d0000`, 1280×2772). Not measured yet — a kernel
   built now must not run on the phone.
2. **Boot shim / image.** Lapis LK loads an Android boot **v4** image with an
   **LZ4** kernel (ruby: v2 + gzip; topaz: v4 shim via Qualcomm ABL). A lapis
   shim and packer, plus the LK handoff contract, are still to be written.
3. **Recovery proof.** Show that flashing the exact factory image back to a
   boot slot restores Android before writing any experiment image.
4. UFS LUN/GPT layout for the `iosroot` partition (`ufslun=` boot-arg) and the
   A78 cluster (kept out of the ADT for now).
