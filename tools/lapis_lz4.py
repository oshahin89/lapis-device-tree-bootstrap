#!/usr/bin/env python3
"""Minimal LZ4 *legacy* frame codec (the format of the lapis stock kernel).

Legacy frame: u32 magic 0x184C2102, then blocks of {u32 compressed size, LZ4
block}; every block decompresses to 8 MiB except the last, and blocks are
independent. The Linux/LK decoders accept any valid LZ4 block, so the
encoder here is a plain greedy hash-chain-free compressor: worse ratio than
`lz4 -l -12`, but exact and dependency free.

  lapis_lz4.py d IN OUT      decompress a legacy stream
  lapis_lz4.py c IN OUT      compress to a legacy stream
"""
import struct
import sys

MAGIC = 0x184C2102
BLOCK = 8 << 20
MINMATCH, LAST_LITERALS, MFLIMIT = 4, 5, 12


def split_blocks(stream):
    """Return [(offset_of_block_data, compressed_size)] of a legacy stream."""
    if struct.unpack_from("<I", stream, 0)[0] != MAGIC:
        raise ValueError("not an LZ4 legacy stream")
    p, out = 4, []
    while p < len(stream):
        n, = struct.unpack_from("<I", stream, p)
        if n == MAGIC:
            p += 4
            continue
        out.append((p + 4, n))
        p += 4 + n
    if p != len(stream):
        raise ValueError("truncated LZ4 legacy stream")
    return out


def decompress_block(src):
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]; i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255:
                    break
        dst += src[i:i + lit]; i += lit
        if i >= n:
            break
        off = src[i] | src[i + 1] << 8; i += 2
        if off == 0 or off > len(dst):
            raise ValueError("bad LZ4 match offset")
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255:
                    break
        ml += MINMATCH
        start = len(dst) - off
        if off >= ml:
            dst += dst[start:start + ml]
        else:
            for k in range(ml):
                dst.append(dst[start + k])
    return bytes(dst)


def _length(out, n):
    while n >= 255:
        out.append(255); n -= 255
    out.append(n)


def compress_block(src):
    n = len(src)
    out = bytearray()
    table = {}
    anchor = i = 0
    limit = n - MFLIMIT
    while i < limit:
        key = src[i:i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is None or i - cand > 0xFFFF:
            i += 1
            continue
        ml = 4
        maxml = n - LAST_LITERALS - i
        while ml < maxml and src[cand + ml] == src[i + ml]:
            ml += 1
        lit = i - anchor
        out.append((min(lit, 15) << 4) | min(ml - 4, 15))
        if lit >= 15:
            _length(out, lit - 15)
        out += src[anchor:i]
        out += struct.pack("<H", i - cand)
        if ml - 4 >= 15:
            _length(out, ml - 4 - 15)
        i += ml
        anchor = i
    lit = n - anchor
    out.append(min(lit, 15) << 4)
    if lit >= 15:
        _length(out, lit - 15)
    out += src[anchor:]
    return bytes(out)


def decompress(stream):
    return b"".join(decompress_block(stream[o:o + s]) for o, s in split_blocks(stream))


def compress(data):
    out = [struct.pack("<I", MAGIC)]
    for o in range(0, len(data), BLOCK):
        blk = compress_block(data[o:o + BLOCK])
        out += [struct.pack("<I", len(blk)), blk]
    return b"".join(out)


if __name__ == "__main__":
    mode, src, dst = sys.argv[1:4]
    data = open(src, "rb").read()
    open(dst, "wb").write(decompress(data) if mode == "d" else compress(data))
