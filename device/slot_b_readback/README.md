# Lapis slot B readback

Read-only rooted ADB capture on 2026-10-28 while the phone was running Android on active slot B. The local raw partition copies are:

| Partition | Bytes | SHA-256 | Factory comparison |
| --- | ---: | --- | --- |
| `lk_b.img` | 16,777,216 | `8325b95fa42e230a886c2b96895a18b4308fcc6a27e623ed8bb68b8dae715364` | First 3,595,360 bytes match supplied `images/lk.img`; remaining partition bytes are zero. Build string: `3f2e32c7d_202609200804`. |
| `vbmeta_b.img` | 8,388,608 | `f680eda5f0721aff3e7d0d6a0d41fb9b9975b6f22dd0240c45ec25b9b155b879` | First 12,288 bytes match supplied `images/vbmeta.img`; remainder is zero. |
| `vbmeta_system_b.img` | 8,388,608 | `49bdff43da3f3d74bbdc0b68b42d92a1fd0c9369a59e0fcbb21828525b4bce42` | First 4,096 bytes match supplied `images/vbmeta_system.img`; remainder is zero. |
| `vbmeta_vendor_b.img` | 8,388,608 | `374d1c70f9ef320481729c750b89e21270c2e346db1cb9dd1ca3ae71a83e61f0` | First 4,096 bytes match supplied `images/vbmeta_vendor.img`; remainder is zero. |

The active boot set was completed with a second read-only rooted ADB capture on 2026-10-28, while `ro.boot.slot_suffix` was `_b`:

| Partition | Bytes | SHA-256 | Comparison with supplied factory image |
| --- | ---: | --- | --- |
| `boot_b.img` | 67,108,864 | `851a543569e0af076cfbe2780d9b67d6a1f8721e4a1fc38544cf0448ff6ae4f5` | Byte-for-byte identical to `images/boot.img` |
| `init_boot_b.img` | 8,388,608 | `5ccd02f536f062d3858c63246ad9b147034dc05b8afdab068d59f4bf62f9c823` | Byte-for-byte identical to `images/init_boot.img` |
| `vendor_boot_b.img` | 67,108,864 | `ad05c3df806140ae98cf5b6a73e1981bb1a6da79b259b1c75e3bd48a53c6eb42` | Byte-for-byte identical to `images/vendor_boot.img` |
| `dtbo_b.img` | 8,388,608 | `9e9912b02697678e8d5de45c2622e32b481221594b2f6375fb9207e6ca8c86e0` | Byte-for-byte identical to `images/dtbo.img` |

The AVB headers in the B vbmeta images report rollback index 0 and the same embedded public-key bytes as A. This is a structural comparison only; signatures and partition descriptors have not been cryptographically verified.

This capture is only a local reference. No device partition was written or flashed.

The full active B boot set is now present locally. This is a verified copy for later recovery planning, not proof that a recovery operation will succeed. In particular, the phone's LK rejected `fastboot boot`, and slot A has not been demonstrated to boot as a recovery system.
