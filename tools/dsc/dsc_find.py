#!/usr/bin/env python3
"""Local symbols of a dyld cache whose name contains SUBSTR: address and name."""
import struct, sys, os
cache, sub = sys.argv[1], sys.argv[2].encode()
main = os.path.join(cache, "dyld_shared_cache_arm64")
with open(main, "rb") as f:
    base, = struct.unpack_from("<Q", f.read(0x200), 0xe0)
with open(main + ".symbols", "rb") as f:
    lso, = struct.unpack_from("<Q", f.read(0x200), 0x48)
    f.seek(lso); nlo, nlc, so, ss, eo, ec = struct.unpack("<6I", f.read(24))
    f.seek(lso + so); strs = f.read(ss)
    f.seek(lso + nlo); nl = f.read(nlc * 16)
    for j in range(nlc):
        strx, typ, sect, desc, val = struct.unpack_from("<IBBHQ", nl, j * 16)
        e = strs.find(b"\0", strx)
        name = strs[strx:e]
        if sub in name and val:
            print(hex(val), name.decode("latin1"))
