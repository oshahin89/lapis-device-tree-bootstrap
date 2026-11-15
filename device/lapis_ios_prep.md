# Preparing the iPhone X iOS userland for lapis (study + flow)

The lapis port runs the **unmodified iOS 16 userland of the iPhone X**
(`iPhone10,3` / `D22AP`) on the SWEET-config xnu-8796, the way ios-sweet and
ios-topaz do. This file is the end-to-end plan; the storage half (the iOS root
on a UFS partition) is still blocked on the UFS host bring-up
([lapis_ufs_bringup.md](lapis_ufs_bringup.md)).

## No Apple material is in this repository

The IPSW, its DeviceTree, its dmgs, the dyld shared cache and any iOS binary
are **yours to supply** and never committed here (see ruby's `LEGAL.md`). The
tools read them on your own machine.

## What you need

- An **iPhone X (`iPhone10,3` / `D22AP`) IPSW, iOS 16.7.x** (the last iOS for
  the A11; same xnu generation, 8796). Any 16.7.x point release is fine.
- A Linux/Mac host with `apfs-fuse`, `hfsprogs` (`mkfs.hfsplus`), `rsync`,
  `lzfse`, and root (for the loop mount).

## Identity — already in the ADT

`tools/lapis_mkadt.py` already presents the device as the iPhone X:
`compatible = D22AP, iPhone10,3, …`, `model = iPhone10,3`, `target-type D22`,
`target-sub-type D22AP`, `chip-id 0x8015` (A11), `display-scale 3`, the iPhone
X canvas 1125×2436, the security tuple for an unactivated dev board, and the
`/chosen` boot-manifest / crypto-hash / SIP fields the userland checks. The
only piece that comes from Apple is the **`/product` node**, which
MobileGestalt reads for product type, artwork, feature flags, etc.

## Step 1 — the `/product` node from the IPSW DeviceTree

Extract `DeviceTree.d22ap*.im4p` from the IPSW (it is under
`Firmware/all_flash/`), then let mkadt read its `/product`:

```sh
# the .im4p is already lzfse-decoded and parsed by tools/ios-sweet/adt.py
python3 tools/lapis_mkadt.py build/phone.adt --product DeviceTree.d22ap.im4p \
  --with-a78
```

`--product` copies the real D22AP `/product` properties into the lapis ADT
(minus the syscfg placeholders). Without it MobileGestalt has no `/product`
and SpringBoard's device class is wrong.

## Step 2 — the iOS root filesystem

From the IPSW's three dmgs (the OS root, the SystemOS cryptex, the AppOS
cryptex — their names are in the IPSW's `BuildManifest.plist`):

```sh
sudo tools/ios-sweet/mkiosroot.sh OS.dmg SYSTEMOS.dmg APPOS.dmg \
  build/iosroot.img 16G
# then the bring-up tweaks (unsigned launchd path, keybag, etc.)
fsck.hfsplus -fy build/iosroot.img
sudo mount -t hfsplus -o loop,force build/iosroot.img /mnt/iosroot
sudo python3 tools/ios-sweet/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
```

This produces a case-sensitive, journaled HFS+ volume (~12 GB used of 16) with
the iOS root, both cryptexes, the dyld shared cache hard-linked where
`ignition_level=1` looks for it, and the plain system keybag. It is the same
format the small `tools/lapis_mkroot_hfs.sh` test root uses — which already
mounts and runs PID 1 on the phone — only full-size.

## Step 3 — where the root goes (blocked on UFS)

The ~16 GB root cannot be a RAM disk. It must live on a UFS partition the
kernel reads as the root device (`rd=ufsroot0 ufsroot=<name>`). Two options:

- **Dual boot**: carve an `iosroot` partition (GPT edit; slot a/b keeps
  Android on b). See ruby's `appendix-dualboot.md` and
  [ios_experiment_readiness.md](ios_experiment_readiness.md).
- **Overwrite `userdata`** (erases Android data) as sweet does.

Both need the UFS host bring-up working first (the controller is left disabled
by LK). Until then the root can only be exercised on a small RAM disk.

## Step 4 — boot-args for the real userland

ios-sweet/ruby boot the iOS userland with (adapted for lapis):

```
debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress sweethw=0 wdt=-1 \
  rd=ufsroot0 ufsroot=iosroot ufsrw=1 rootrw=1 \
  launchd_unsecure_cache=1 unentitled_ios_sim_launch=1 ignition_level=1 \
  lapispanicreboot=15
```

- `rootrw=1`/`ufsrw=1`: iOS writes to its root (no separate data volume here).
- `ignition_level=1`: dyld's libignition finds the shared cache in the root.
- `launchd_unsecure_cache=1`, `unentitled_ios_sim_launch=1`: AMFI/codesigning
  are relaxed (patch 0035 of the sweet series; security is off in this port).
- `sweethw=0`: the lapis touch/keys/USB drivers are not wired yet; turn on when
  they are.

## Order of work

1. UFS host bring-up (blocks everything storage).
2. `iosroot` partition + flash the HFS+ root (needs the owner's OK; back up).
3. First boot: expect launchd → libignition → the known first-boot hurdles
   (ruby's three walls: image layout, display path, a MobileGestalt/
   LaunchServices deadlock). Read the kernel log from the screen.
4. Touch into SweetHID so the lock screen can be used.

Nothing here has been run; the IPSW and the device steps are the owner's.
