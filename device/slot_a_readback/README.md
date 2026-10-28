# Lapis slot A readback

Captured read-only from `/dev/block/by-name/*_a` via rooted ADB on 2026-10-28. The phone remained in Android and the active slot was B. These files are local raw partition readbacks; no device partition was written.

| Partition | Bytes | SHA-256 | Comparison with supplied factory image |
| --- | ---: | --- | --- |
| `boot_a.img` | 67,108,864 | `2701e775484eed7ed783b8e3fc50e5226207381112ec45f0869c2f2ab824df29` | Different overall; Android v4 header and 16,544,915-byte kernel payload match `images/boot.img`; AVB vbmeta differs |
| `init_boot_a.img` | 8,388,608 | `7c9befbef70ec722282e20e4afa9b16487e4c1acca2f8071a9a1027c8b33c229` | Different generic ramdisk and AVB vbmeta; v4 header matches |
| `vendor_boot_a.img` | 67,108,864 | `97799b3ba58d40cfb56a90649b1039ee3a325ed218d1068dea5e1639d60d60d2` | Different vendor ramdisk/header/AVB; embedded 331,202-byte DTB matches factory image |
| `dtbo_a.img` | 8,388,608 | `86a44b41eb5247f8c55eac173194a616a5cfe5915bcade03b0f381513919ba61` | DTBO header and 253,381-byte overlay match; AVB vbmeta differs |
| `vbmeta_a.img` | 8,388,608 | `b7c612bdd211c4e8ea913f8b989c5cfcd3ffa9c5357d64b4ea9a8ee1fe88378b` | No exact factory partition image available locally for comparison |
| `vbmeta_system_a.img` | 8,388,608 | `eccef4f34df1ceb44858079459084b1312e8bbc5060917212f8ee87152176d08` | No exact factory partition image available locally for comparison |
| `vbmeta_vendor_a.img` | 8,388,608 | `b6b55a8cd30c9f332a5ce79e910c9a6f559f9c3c129987ac2824397b88c6a7e8` | No exact factory partition image available locally for comparison |
| `lk_a.img` | 16,777,216 | `7e3cdfc9ace4927a0035d048bdbbb32ffa41b0e56acfd5fc9009e5166486abb2` | Raw partition includes zero padding; embedded build string is `3f2e32c7d_202608131223` |

The stock `boot_a` is not byte-for-byte the supplied current factory `boot.img`; although its header and kernel payload match, this does not prove slot A is bootable or a safe recovery slot. Its init/vendor ramdisks and chained AVB metadata differ. No AVB signature verification was performed on slot A. Preserve these originals before any later experiment, and do not flash or erase based on this comparison alone.

The ramdisks identify slot A as the previous OS3.0.308.0.WPPMIXM build: `init_boot_a` has a 25-entry CPIO archive and differs from current OS3.0.309.0 only in `system/etc/ramdisk/build.prop`; `vendor_boot_a` normal and recovery archives likewise differ only in build properties and, for normal boot, `system/etc/copylib.txt`. The version properties say OS3.0.308.0 while the supplied factory set is OS3.0.309.0. This strongly indicates an older OTA slot, but does not itself prove that its full system/vendor partitions or boot chain remain bootable.

The active `lk_b` readback is stored separately at `../slot_b_readback/lk_b.img`. Its first 3,595,360 bytes match the supplied factory `images/lk.img` exactly; both contain build string `3f2e32c7d_202609200804`. Slot A's LK string is older (`3f2e32c7d_202608131223`).

The three A vbmeta headers report rollback index 0 and contain the same embedded public-key bytes as the active B vbmeta headers. The A vbmeta payloads differ from the supplied current factory and B partition contents. This is structural data only; the A signatures, chained descriptors and hashes have not been cryptographically verified.
