# Lapis HyperOS 4 (Android 17) boot-chain reference

Extracted from the official HyperOS 4 OTA `payload.bin` (via `payload_dumper`)
after the 2026-11-16 upgrade, using `tools/` on the host — the device was not
read for this. These are the stock images for the current boot chain; the
large `*.img` binaries are kept locally (git-ignored, Xiaomi proprietary), this
README is the committed recovery contract.

LK build string: `8d774899e_202611111716` (replaces the old
`3f2e32c7d_202609200804`). The framebuffer stayed at `0xfd72f000`.

| Partition | Bytes | SHA-256 |
| --- | ---: | --- |
| `boot.img` | 67,108,864 | `a01d0f11fb35c95f7410df5a27c379edaec65bbef01ab994c74b88579f6bd6c5` |
| `init_boot.img` | 8,388,608 | `d040c320190dd5612f56f4b6955cad0983fd56ca5700f1944b5f08bd927ac2f7` |
| `vendor_boot.img` | 67,108,864 | `ef5dbed7e4652626b91ddee818d87a1b4775c381e32bfdeb14ccf7a1499d3f51` |
| `dtbo.img` | 8,388,608 | `efe0538d8e6d05b8c2c12005eff0de3791cd1fdada518b9e8d91d7083eb7c713` |
| `lk.img` | 3,592,192 | `58e7882137119338ed4ce262b12b54b18bb29d737d79069c4d001af4eb5045df` |
| `gz.img` | 1,294,336 | `16b26e1b269b99e7275033a7be10f6211622b37b6bdd1c5395b93230eddfee5f` |
| `vbmeta.img` | 12,288 | `d4fe0611185a0128cd16639e5dc8cc3dbf0608b74decf1263cfca6d9e0862676` |
| `vbmeta_system.img` | 4,096 | `d4ae5bb411f9cc14ff835d3145f38086c23854fe8b25e348930d3140e91ff557` |
| `vbmeta_vendor.img` | 8,388,608 (padded) | `ca99078231fa737db46e3c1f64442db1e0cb47cff47bddfea714a5624008a7ad` |

## Recover Android after an experiment

Active slot is `b`. After running any probe/iOS boot image, return to fastboot
(Power + Vol-down — the new LK does **not** honour PSCI reboot-to-bootloader),
then restore the stock Android boot:

```
fastboot flash boot_b device/hyperos4_readback/boot.img
fastboot reboot
```

If the A/B retry counter got burned and the slot flipped to `a`:
`fastboot --set-active=b && fastboot reboot bootloader` (then re-check
`fastboot getvar current-slot`), and flash `boot_b` as above.
