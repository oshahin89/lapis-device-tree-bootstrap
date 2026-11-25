#!/usr/bin/env python3
"""Write LEN bytes at unslid VA of a split dyld cache to OUT."""
import os, struct, sys
cache, va, ln, out = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0), sys.argv[4]
for name in sorted(os.listdir(cache)):
    if not name.startswith("dyld_shared_cache_arm64"): continue
    path = os.path.join(cache, name)
    with open(path, "rb") as f:
        h = f.read(0x100)
        if not h.startswith(b"dyld_v1"): continue
        moff, mcnt = struct.unpack_from("<II", h, 0x10)
        f.seek(moff)
        for _ in range(mcnt):
            addr, size, foff, _, _ = struct.unpack("<QQQII", f.read(32))
            if addr <= va < addr + size:
                f.seek(foff + va - addr); open(out, "wb").write(f.read(ln)); print(name); sys.exit(0)
print("not found"); sys.exit(1)
