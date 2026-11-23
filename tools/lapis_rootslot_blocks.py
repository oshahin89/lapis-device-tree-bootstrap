#!/usr/bin/env python3
"""A file as a slot's blocks for tools/lapis_rootslot.sh: zero-padded to the
slot's whole size.   tools/lapis_rootslot_blocks.py SLOT IN OUT"""
import sys
COUNT = {'fdr': 36, 'bbfw': 4912}
slot, src, out = sys.argv[1:4]
data = open(src, 'rb').read()
n = COUNT[slot] * 4096
if len(data) > n:
    raise SystemExit(f'{src}: {len(data)} bytes, the {slot} slot holds {n}')
open(out, 'wb').write(data + b'\0' * (n - len(data)))
print(f'{out}: {len(data)} bytes + {n - len(data)} zeros for slot {slot}')
