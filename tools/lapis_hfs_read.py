#!/usr/bin/env python3
"""Read-only HFS+/HFSX reader for inspecting the iOS root image after a boot.

No mount (and no sudo) needed: it parses the volume header and the catalog
B-tree directly. Used to see what iOS wrote to `iosroot` on the phone.

  lapis_hfs_read.py info  IMG
  lapis_hfs_read.py newer IMG [--since EPOCH] [--limit N]   files/dirs modified after EPOCH
  lapis_hfs_read.py ls    IMG PATH
  lapis_hfs_read.py cat   IMG PATH [--out FILE]
  lapis_hfs_read.py diff  ORIG.img AFTER.img                1 MiB chunks that differ

The journal is NOT replayed: data that iOS committed only to the journal before
the reset is invisible here (`info` prints whether the journal is dirty).
Fragmented files whose extents spill into the extents-overflow file are read
only up to their first 8 extents (flagged).
"""
import argparse
import os
import struct
import sys
import time

HFS_EPOCH = 2082844800  # seconds from 1904-01-01 to 1970-01-01


def be16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def be64(b, o):
    return struct.unpack_from(">Q", b, o)[0]


class Fork:
    def __init__(self, raw):
        self.size = be64(raw, 0)
        self.total_blocks = be32(raw, 12)
        self.extents = [(be32(raw, 16 + 8 * i), be32(raw, 20 + 8 * i)) for i in range(8)]
        self.extents = [e for e in self.extents if e[1]]
        self.overflow = sum(c for _, c in self.extents) < self.total_blocks


class Volume:
    def __init__(self, path):
        self.f = open(path, "rb")
        vh = self.read(1024, 512)
        self.sig = vh[0:2]
        if self.sig not in (b"H+", b"HX"):
            sys.exit(f"not HFS+/HFSX: {self.sig!r}")
        self.attributes = be32(vh, 4)
        self.journal_block = be32(vh, 12)
        self.modify = be32(vh, 20)
        self.files, self.folders = be32(vh, 32), be32(vh, 36)
        self.bs = be32(vh, 40)
        self.total, self.free = be32(vh, 44), be32(vh, 48)
        self.write_count = be32(vh, 68)
        self.catalog = Fork(vh[272:352])
        hdr = self.fork_read(self.catalog, 0, 512)
        self.root_node = be32(hdr, 14 + 2)
        self.first_leaf = be32(hdr, 14 + 10)
        self.node_size = be16(hdr, 14 + 18)

    def read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def fork_read(self, fork, off, n):
        out = bytearray()
        pos = 0
        for start, count in fork.extents:
            ext_bytes = count * self.bs
            if off < pos + ext_bytes and n > 0:
                inner = off - pos
                take = min(n, ext_bytes - inner)
                out += self.read(start * self.bs + inner, take)
                off += take
                n -= take
            pos += ext_bytes
        return bytes(out)

    def leaf_records(self):
        node = self.first_leaf
        ns = self.node_size
        while node:
            b = self.fork_read(self.catalog, node * ns, ns)
            flink, kind, nrec = be32(b, 0), struct.unpack_from("b", b, 8)[0], be16(b, 10)
            if kind != -1:
                break
            for i in range(nrec):
                ro = be16(b, ns - 2 * (i + 1))
                klen = be16(b, ro)
                parent = be32(b, ro + 2)
                nlen = be16(b, ro + 6)
                name = b[ro + 8:ro + 8 + 2 * nlen].decode("utf-16-be", "replace")
                d = ro + 2 + klen
                yield parent, name, b[d:d + 248]
            node = flink

    def path_index(self):
        """Build CNID -> path from folder records and their thread records."""
        names = {}
        entries = []
        for parent, name, rec in self.leaf_records():
            rtype = struct.unpack_from(">h", rec, 0)[0]
            if rtype == 1:
                cnid = be32(rec, 8)
                names[cnid] = (parent, name)
                entries.append(("d", parent, name, cnid, rec))
            elif rtype == 2:
                cnid = be32(rec, 8)
                entries.append(("f", parent, name, cnid, rec))
        cache = {1: "", 2: ""}

        def path(cnid):
            if cnid in cache:
                return cache[cnid]
            if cnid not in names:
                return f"<{cnid}>"
            p, n = names[cnid]
            cache[cnid] = path(p) + "/" + n
            return cache[cnid]
        return entries, path


def fmt_time(hfs_t):
    if not hfs_t:
        return "-"
    return time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(hfs_t - HFS_EPOCH))


def cmd_info(v, _a):
    j_dirty = None
    if v.journal_block and (v.attributes & (1 << 13)):
        ji = v.read(v.journal_block * v.bs, 52)
        joff = be64(ji, 36)
        jh = v.read(joff, 512)
        # journal header is little-endian when written by a little-endian host
        magic = struct.unpack_from("<I", jh, 0)[0]
        endian = "<" if magic == 0x4A4E4C78 else ">"
        start, end = struct.unpack_from(endian + "QQ", jh, 8)
        j_dirty = start != end
    print(f"signature {v.sig.decode()}  block {v.bs}  blocks {v.total} free {v.free}")
    print(f"files {v.files}  folders {v.folders}  writeCount {v.write_count}")
    print(f"modified {fmt_time(v.modify)} UTC  attributes 0x{v.attributes:x} "
          f"(unmounted cleanly: {bool(v.attributes & (1 << 8))})")
    print(f"journal: {'none' if j_dirty is None else ('DIRTY (uncommitted txns)' if j_dirty else 'clean')}")
    print(f"catalog node size {v.node_size}, overflow extents: {v.catalog.overflow}")


def cmd_newer(v, a):
    entries, path = v.path_index()
    since = a.since + HFS_EPOCH if a.since else v.modify - 1
    rows = []
    for kind, parent, name, cnid, rec in entries:
        mod = be32(rec, 16)
        if mod > since:
            size = be64(rec, 88) if kind == "f" else 0
            rows.append((mod, kind, size, path(parent) + "/" + name))
    rows.sort()
    for mod, kind, size, p in rows[-a.limit:]:
        print(f"{fmt_time(mod)}  {kind} {size:>10}  {p}")
    print(f"-- {len(rows)} entries newer than {fmt_time(since)} UTC")


def lookup(v, target):
    entries, path = v.path_index()
    target = "/" + target.strip("/")
    for kind, parent, name, cnid, rec in entries:
        if path(parent) + "/" + name == target:
            return kind, cnid, rec, entries, path
    sys.exit(f"not found: {target}")


def cmd_ls(v, a):
    kind, cnid, _, entries, path = lookup(v, a.path) if a.path.strip("/") else ("d", 2, None, *v.path_index())
    for k, parent, name, c, rec in entries:
        if parent == cnid:
            size = be64(rec, 88) if k == "f" else 0
            print(f"{fmt_time(be32(rec, 16))}  {k} {size:>10}  {name}")


def cmd_cat(v, a):
    kind, _, rec, _, _ = lookup(v, a.path)
    if kind != "f":
        sys.exit("not a file")
    fork = Fork(rec[88:168])
    if fork.overflow:
        print("warning: file has overflow extents; output truncated", file=sys.stderr)
    data = v.fork_read(fork, 0, fork.size)
    if a.out:
        open(a.out, "wb").write(data)
    else:
        sys.stdout.buffer.write(data)


def cmd_diff(a):
    ch = 1 << 20
    n = 0
    with open(a.orig, "rb") as x, open(a.after, "rb") as y:
        i = 0
        while True:
            p, q = x.read(ch), y.read(ch)
            if not p and not q:
                break
            if p != q:
                print(f"chunk {i} (offset {i * ch:#x}) differs")
                n += 1
            i += 1
    print(f"-- {n} of {i} MiB chunks differ")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sp = ap.add_subparsers(dest="cmd", required=True)
    for name in ("info", "newer", "ls", "cat"):
        p = sp.add_parser(name)
        p.add_argument("img")
        if name in ("ls", "cat"):
            p.add_argument("path")
        if name == "newer":
            p.add_argument("--since", type=int, default=0)
            p.add_argument("--limit", type=int, default=200)
        if name == "cat":
            p.add_argument("--out")
    p = sp.add_parser("diff")
    p.add_argument("orig")
    p.add_argument("after")
    a = ap.parse_args()
    if a.cmd == "diff":
        return cmd_diff(a)
    v = Volume(a.img)
    {"info": cmd_info, "newer": cmd_newer, "ls": cmd_ls, "cat": cmd_cat}[a.cmd](v, a)


if __name__ == "__main__":
    main()
