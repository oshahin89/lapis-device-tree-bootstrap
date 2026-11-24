#!/usr/bin/env python3
"""AGXMetalA11.bundle's Info.plist with CFBundleExecutable = ds.g10p (the
LapisMetal plug-in sits over that file, tools/lapis_rootslot.sh slot agxds),
as the one 4 KiB block of slot agxinfo. The catalog keeps the file's size
(852 bytes), so the new binary plist is exactly that long: written by
plistlib, then padded between its objects and its offset table (unreferenced
bytes a binary plist reader skips), the trailer's table offset moved.

    tools/lapis_agx_infoplist.py ORIGINAL_INFO_PLIST OUT_BLOCK
"""
import plistlib, struct, sys

src, out = sys.argv[1:3]
orig = open(src, 'rb').read()
p = plistlib.loads(orig)
p['CFBundleExecutable'] = 'ds.g10p'
new = plistlib.dumps(p, fmt=plistlib.FMT_BINARY, sort_keys=False)
size = len(orig)
if len(new) > size:
    for key in ('BuildMachineOSBuild', 'DTXcodeBuild', 'DTXcode'):   # build metadata, unused at run time
        if len(new) <= size:
            break
        p.pop(key, None)
        new = plistlib.dumps(p, fmt=plistlib.FMT_BINARY, sort_keys=False)
if len(new) > size:
    raise SystemExit(f'{len(new)} bytes, more than {size}')
pad = size - len(new)
off_size, ref_size, nobj, top, table = struct.unpack('>6xBBQQQ', new[-32:])
padded = new[:table] + b'\0' * pad + new[table:-32] + struct.pack('>6xBBQQQ', off_size, ref_size, nobj, top, table + pad)
assert len(padded) == size
back = plistlib.loads(padded)
assert back['CFBundleExecutable'] == 'ds.g10p' and back['NSPrincipalClass'] == p['NSPrincipalClass']
open(out, 'wb').write(padded + b'\0' * (4096 - size))
print(f'{out}: {size} bytes ({pad} padding) + zeros, CFBundleExecutable ds.g10p')
