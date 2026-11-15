#!/usr/bin/env python3
"""Read a GPT and plan an `iosroot` partition for dual-booting iOS (read-only).

  lapis_gpt_plan.py GPT.bin [--bs 4096] [--disk-lba N] [--iosroot-gib 16] [--name iosroot]

GPT.bin is a dump of the start of the UFS LUN that holds the Android GPT
(LUN 2 on lapis): enough to cover the primary header (LBA 1) and the entry
array. Capture it on the device, read-only:
  adb shell su -c 'dd if=/dev/block/sdc bs=4096 count=64 of=/sdcard/gpt.bin'

The tool lists every partition, finds free space, and prints a plan to add an
`iosroot` partition of the requested size (default 16 GiB) -- by using trailing
free space if there is any, otherwise by shrinking the largest data partition
(`userdata`). It only reads and computes; it writes nothing and runs no tool.
Every change it proposes is shown as an exact command for you to review.
"""
import struct
import sys


def rd_header(b, bs):
    # header is at LBA 1
    h = b[bs:bs + 92]
    if h[:8] != b"EFI PART":
        sys.exit("no GPT header at LBA 1 (wrong --bs? try 512 or 4096)")
    # sig(8) rev(4) hsz(4) crc(4) reserved(4) current(8) backup(8) first_usable(8) last_usable(8)
    (sig, rev, hsz, hcrc, _, cur, bak, first_usable, last_usable) = struct.unpack_from("<8sIIIIQQQQ", h, 0)
    disk_guid = h[56:72]
    entry_lba, nent, esz, ecrc = struct.unpack_from("<QIII", h, 72)
    return dict(cur=cur, bak=bak, first_usable=first_usable, last_usable=last_usable,
                entry_lba=entry_lba, nent=nent, esz=esz)


def rd_entries(b, bs, hdr):
    out = []
    base = hdr["entry_lba"] * bs
    for i in range(hdr["nent"]):
        e = b[base + i * hdr["esz"]: base + (i + 1) * hdr["esz"]]
        if len(e) < 128 or e[:16] == b"\0" * 16:
            continue
        first, last, flags = struct.unpack_from("<QQQ", e, 32)
        name = e[56:128].decode("utf-16le", "replace").split("\0")[0]
        out.append(dict(i=i, first=first, last=last, name=name))
    return out


def main():
    import argparse
    a = argparse.ArgumentParser()
    a.add_argument("gpt")
    a.add_argument("--bs", type=int, default=4096)
    a.add_argument("--disk-lba", type=int, default=0, help="total LBAs on the LUN (for the plan)")
    a.add_argument("--iosroot-gib", type=int, default=16)
    a.add_argument("--name", default="iosroot")
    args = a.parse_args()
    b = open(args.gpt, "rb").read()
    hdr = rd_header(b, args.bs)
    parts = sorted(rd_entries(b, args.bs, hdr), key=lambda p: p["first"])
    bs = args.bs
    need = args.iosroot_gib * (1 << 30) // bs

    print(f"block size {bs}, first usable LBA {hdr['first_usable']}, last usable LBA {hdr['last_usable']}")
    print(f"{len(parts)} partitions:")
    prev_end = hdr["first_usable"] - 1
    gaps = []
    for p in parts:
        gap = p["first"] - prev_end - 1
        if gap > 0:
            gaps.append((prev_end + 1, p["first"] - 1, gap))
        sz = (p["last"] - p["first"] + 1) * bs
        print(f"  [{p['i']:3}] {p['name']:16} LBA {p['first']:>12}..{p['last']:>12}  {sz/(1<<30):8.2f} GiB")
        prev_end = max(prev_end, p["last"])
    tail = hdr["last_usable"] - prev_end
    if tail > 0:
        gaps.append((prev_end + 1, hdr["last_usable"], tail))

    print(f"\nneed {need} LBAs ({args.iosroot_gib} GiB) for '{args.name}'")
    big = [g for g in gaps if g[2] >= need]
    if big:
        s, e, n = big[0]
        print(f"FREE SPACE is enough: LBA {s}..{s + need - 1}")
        print("Plan (review before running on the device, with a full GPT backup):")
        print(f"  # new partition in free space, no existing data moved")
        print(f"  sgdisk --new=0:{s}:{s + need - 1} --change-name=0:{args.name} /dev/block/sdc")
        return
    # no free space: shrink the largest data partition (userdata)
    data = max(parts, key=lambda p: p["last"] - p["first"], default=None)
    if not data:
        sys.exit("no partition to shrink")
    dsz = (data["last"] - data["first"] + 1)
    if dsz <= need:
        sys.exit(f"largest partition {data['name']} is smaller than {args.iosroot_gib} GiB")
    new_last = data["last"] - need
    print(f"NO free space. Largest partition is '{data['name']}' "
          f"({dsz * bs/(1<<30):.1f} GiB, LBA {data['first']}..{data['last']}).")
    print("Plan (DESTRUCTIVE to that partition's tail; needs a resize of its filesystem first,")
    print(" an explicit OK, and a full GPT + userdata-header backup):")
    print(f"  1. back up: dd if=/dev/block/sdc bs=4096 count=64 of=gpt-backup.bin")
    print(f"  2. shrink the F2FS/ext4 on '{data['name']}' to <= {(new_last - data['first'] + 1)*bs//(1<<30)} GiB")
    print(f"  3. sgdisk --delete={data['i']+1} \\")
    print(f"       --new={data['i']+1}:{data['first']}:{new_last} --change-name={data['i']+1}:{data['name']} \\")
    print(f"       --new=0:{new_last+1}:{data['last']} --change-name=0:{args.name} /dev/block/sdc")
    print(f"  (iosroot would be LBA {new_last+1}..{data['last']}, {need*bs/(1<<30):.0f} GiB)")


if __name__ == "__main__":
    main()
