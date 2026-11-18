#!/usr/bin/env python3
"""Build a new lapis LUN2 GPT that carves `iosroot` out of the tail of
`userdata` WITHOUT changing the number of GPT entries.

The preloader rejects tables whose num_partition_entries "overflow" an
unknown limit, and lapis's table already uses all 94 slots. So instead of
adding an entry, this moves an existing empty padding partition (default
`bk3`, all-zero and unreferenced by Android, LK or the preloader) onto the
freed tail and renames it. Only two entries change: userdata's last LBA and
the moved entry's LBAs + name. Both the primary (LBA 1 + entries at LBA 2)
and the backup (entries + header at the end of the LUN) get new CRCs.

Inputs are read-only captures made on rooted Android:
  dd if=/dev/block/sdc bs=4096 count=64                 of=primary.bin
  dd if=/dev/block/sdc bs=4096 skip=<last-7> count=8    of=end.bin

Outputs (to be written back with dd only after review and an explicit OK):
  OUT_primary.bin  = LBA 1 .. 7      (header + entry blocks)
  OUT_end.bin      = LBA last-7 .. last  (backup entries + backup header)

It never touches the device itself.
"""
import argparse
import struct
import sys
import zlib

BS = 4096
HDR = struct.Struct("<8sIIII QQQQ16sQIII")


def crc(b):
    return zlib.crc32(b) & 0xffffffff


def parse_hdr(blk):
    f = HDR.unpack(blk[:HDR.size])
    keys = ("sig", "rev", "hsz", "hcrc", "res", "my", "alt", "first_usable",
            "last_usable", "guid", "elba", "n", "esz", "ecrc")
    return dict(zip(keys, f))


def check_hdr(blk, entries):
    h = parse_hdr(blk)
    hh = bytearray(blk[:h["hsz"]])
    hh[16:20] = b"\0" * 4
    ok = (h["sig"] == b"EFI PART" and crc(bytes(hh)) == h["hcrc"]
          and crc(entries[:h["n"] * h["esz"]]) == h["ecrc"])
    return h, ok


def entry(entries, i, esz=128):
    e = entries[i * esz:(i + 1) * esz]
    first, last, attrs = struct.unpack("<QQQ", e[32:56])
    return e[:16], e[16:32], first, last, attrs, e[56:128].decode("utf-16le").rstrip("\0")


def index_of(entries, n, name):
    hits = [i for i in range(n) if entry(entries, i)[5] == name]
    if len(hits) != 1:
        sys.exit(f"expected exactly one '{name}', found {hits}")
    return hits[0]


def rebuild_hdr(blk, ecrc):
    b = bytearray(blk)
    h = parse_hdr(b)
    struct.pack_into("<I", b, 88, ecrc)
    struct.pack_into("<I", b, 16, 0)
    struct.pack_into("<I", b, 16, crc(bytes(b[:h["hsz"]])))
    return bytes(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("primary")
    ap.add_argument("end")
    ap.add_argument("out")
    ap.add_argument("--iosroot-gib", type=int, default=16)
    ap.add_argument("--donor", default="bk3")
    ap.add_argument("--name", default="iosroot")
    a = ap.parse_args()

    pri = open(a.primary, "rb").read()
    end = open(a.end, "rb").read()
    ph, ok = check_hdr(pri[BS:2 * BS], pri[2 * BS:])
    if not ok or ph["my"] != 1:
        sys.exit("primary GPT invalid")
    n, esz, elba = ph["n"], ph["esz"], ph["elba"]
    last = ph["alt"]
    end_lba0 = last - (len(end) // BS) + 1
    bh_blk = end[(last - end_lba0) * BS:(last - end_lba0 + 1) * BS]
    bh = parse_hdr(bh_blk)
    bents = end[(bh["elba"] - end_lba0) * BS:]
    bh, ok = check_hdr(bh_blk, bents)
    if not ok or bh["my"] != last or bh["n"] != n:
        sys.exit("backup GPT invalid")
    ents = bytearray(pri[elba * BS:elba * BS + n * esz])
    if bytes(ents) != bents[:n * esz]:
        sys.exit("primary and backup entries differ")

    ud = index_of(ents, n, "userdata")
    dn = index_of(ents, n, a.donor)
    if any(entry(ents, i)[5] == a.name for i in range(n)):
        sys.exit(f"'{a.name}' already exists")
    _, _, ud_first, ud_last, _, _ = entry(ents, ud)
    _, _, dn_first, dn_last, _, _ = entry(ents, dn)
    if bytes(ents[dn * esz + 32:dn * esz + 56]) and dn_last - dn_first > 0x10000:
        sys.exit("donor is not a small padding partition")

    need = a.iosroot_gib * (1 << 30) // BS
    new_ud_last = ud_last - need
    new_first, new_last = new_ud_last + 1, ud_last
    if new_ud_last <= ud_first:
        sys.exit("userdata too small")

    struct.pack_into("<Q", ents, ud * esz + 40, new_ud_last)
    struct.pack_into("<QQ", ents, dn * esz + 32, new_first, new_last)
    nm = a.name.encode("utf-16le")
    ents[dn * esz + 56:dn * esz + 128] = nm + b"\0" * (72 - len(nm))
    ents = bytes(ents)
    ecrc = crc(ents)

    # primary: LBA 1 .. elba + entry blocks - 1 (keep untouched tail bytes)
    p_out = bytearray(pri[BS:ph["first_usable"] * BS])
    p_out[0:BS] = rebuild_hdr(pri[BS:2 * BS], ecrc)
    eo = (elba - 1) * BS
    p_out[eo:eo + len(ents)] = ents
    # backup: same end window
    e_out = bytearray(end)
    beo = (bh["elba"] - end_lba0) * BS
    e_out[beo:beo + len(ents)] = ents
    ho = (last - end_lba0) * BS
    e_out[ho:ho + BS] = rebuild_hdr(bh_blk, ecrc)

    # re-parse what we built, independently
    h1, ok1 = check_hdr(bytes(p_out[0:BS]), bytes(p_out[eo:]))
    h2, ok2 = check_hdr(bytes(e_out[ho:ho + BS]), bytes(e_out[beo:]))
    assert ok1 and ok2 and h1["n"] == n and h2["n"] == n
    changed = [i for i in range(n) if ents[i * esz:(i + 1) * esz] !=
               pri[elba * BS + i * esz:elba * BS + (i + 1) * esz]]
    assert sorted(changed) == sorted({ud, dn}), changed
    spans = sorted((entry(ents, i)[2], entry(ents, i)[3], entry(ents, i)[5])
                   for i in range(n))
    for (f1, l1, n1), (f2, l2, n2) in zip(spans, spans[1:]):
        if f2 <= l1:
            sys.exit(f"overlap: {n1} {f1}..{l1} and {n2} {f2}..{l2}")
    if spans[0][0] < ph["first_usable"] or spans[-1][1] > ph["last_usable"]:
        sys.exit("entry outside usable range")

    open(a.out + "_primary.bin", "wb").write(p_out)
    open(a.out + "_end.bin", "wb").write(e_out)
    print(f"entries {n} (unchanged), changed: {sorted(changed)}")
    print(f"userdata  {ud_first}..{ud_last} -> {ud_first}..{new_ud_last} "
          f"({(new_ud_last - ud_first + 1) * BS / (1 << 30):.2f} GiB)")
    print(f"{a.donor:9s} {dn_first}..{dn_last} -> '{a.name}' {new_first}..{new_last} "
          f"({(new_last - new_first + 1) * BS / (1 << 30):.2f} GiB)")
    print(f"write {a.out}_primary.bin at LBA 1 ({len(p_out) // BS} blocks), "
          f"{a.out}_end.bin at LBA {end_lba0} ({len(e_out) // BS} blocks)")
    print(f"f2fs block_count must be <= {new_ud_last - ud_first + 1} "
          f"and a multiple of the section size")


if __name__ == "__main__":
    main()
