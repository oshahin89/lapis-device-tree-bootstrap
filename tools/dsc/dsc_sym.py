#!/usr/bin/env python3
"""Nearest local symbol for unslid VAs, from a split dyld cache's .symbols file."""
import bisect, struct, sys, os
cache = sys.argv[1]; vas = [int(v, 0) for v in sys.argv[2:]]
main = os.path.join(cache, "dyld_shared_cache_arm64")
symf = main + ".symbols"
def images(path):
    with open(path, "rb") as f:
        h = f.read(0x400)
        off, cnt = struct.unpack_from("<QQ", h, 0x88)
        f.seek(off); arr = f.read(cnt * 32); out = []
        for i in range(cnt):
            load, = struct.unpack_from("<Q", arr, i * 32 + 16)
            pathoff, = struct.unpack_from("<I", arr, i * 32 + 28)
            f.seek(pathoff); name = b""
            while True:
                c = f.read(1)
                if not c or c == b"\0": break
                name += c
            out.append((load, name.decode()))
        return out
imgs = sorted(images(main))
with open(main, "rb") as f:
    h = f.read(0x200)
    base, = struct.unpack_from("<Q", h, 0xe0)  # sharedRegionStart
with open(symf, "rb") as f:
    h = f.read(0x200)
    lso, lss = struct.unpack_from("<QQ", h, 0x48)
    f.seek(lso); info = f.read(24)
    nlo, nlc, so, ss, eo, ec = struct.unpack("<6I", info)
    f.seek(lso + eo); ents = f.read(ec * 16)
    want = {}
    for va in vas:
        i = bisect.bisect_right([a for a, _ in imgs], va) - 1
        want.setdefault(imgs[i][0], []).append(va)
    entries = {}
    for k in range(ec):
        doff, start, cnt = struct.unpack_from("<QII", ents, k * 16)
        entries[base + doff] = (start, cnt)
    for load, lst in want.items():
        name = [n for a, n in imgs if a == load][0]
        if load not in entries:
            for va in lst: print(hex(va), name, "(no local symbols)"); continue
        start, cnt = entries[load]
        f.seek(lso + nlo + start * 16); raw = f.read(cnt * 16)
        syms = []
        for j in range(cnt):
            strx, typ, sect, desc, val = struct.unpack_from("<IBBHQ", raw, j * 16)
            if val and (typ & 0x0e) == 0x0e:
                syms.append((val, strx))
        syms.sort(); vals = [v for v, _ in syms]
        for va in lst:
            i = bisect.bisect_right(vals, va) - 1
            f.seek(lso + so + syms[i][1]); s = f.read(200).split(b"\0")[0].decode("latin1")
            print(f"{va:#x} {os.path.basename(name)} {s}+{va - vals[i]:#x}")
