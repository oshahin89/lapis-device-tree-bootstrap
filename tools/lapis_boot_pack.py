#!/usr/bin/env python3
"""Pack a first-stage payload or a whole kernel Image into a copy of the lapis stock boot partition.

  lapis_boot_pack.py STOCK_BOOT OUT.img --probe PAYLOAD.bin [--expect-stock-sha256 H]
  lapis_boot_pack.py STOCK_BOOT OUT.img --image Image [--append BLOB] [--expect-stock-sha256 H]

This only WRITES A FILE. It never talks to the phone. Flashing the result is
a separate, explicitly authorised step that needs a proven restore path
(device/ios_experiment_readiness.md).

STOCK_BOOT is the full 64 MiB boot partition (e.g. device/slot_b_readback/
boot_b.img): Android boot header v4, LZ4 legacy kernel, the 16 KiB boot
signature blob, and an AVB hash footer at the end of the partition.

--probe follows ruby-ios-bringup's "small payload" recipe, adapted to v4 + LZ4:
  1. decompress the stock kernel (Linux arm64 Image, 'ARMd' at 0x38);
  2. replace code0 (raw[0:4]) with `b 0x1000` and put the payload at
     raw+0x1000; keep the Image header and every other byte;
  3. recompress only LZ4 block 0 (blocks are independent; blocks 1..n are
     copied byte for byte) and update the header's kernel_size;
  4. lay out header | kernel | stock boot-signature blob | stock vbmeta, and
     rewrite the AVB footer's original_image_size / vbmeta_offset.

--image replaces the whole decompressed kernel with IMAGE (an arm64 Image,
e.g. tools/lapis_shim), optionally followed at the next 4 KiB boundary by
BLOB (a SWEETXNU payload); the Image header's image_size is patched to cover
both, the result is LZ4-compressed from scratch, and the same header /
signature / vbmeta / footer handling applies.

The AVB hash no longer matches the image. LK on this unlocked (orange)
device is expected to continue, as it does for unsigned root-patched boot
images, but that is unverified on lapis.
"""
import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lapis_lz4 as lz4  # noqa: E402

PAGE = 4096
PAYLOAD_OFF = 0x1000
BRANCH_TO_PAYLOAD = struct.pack("<I", 0x14000000 | (PAYLOAD_OFF >> 2))   # b +0x1000
FOOTER = 64


def pad(n):
    return (n + PAGE - 1) // PAGE * PAGE


def parse(img):
    if img[:8] != b"ANDROID!":
        sys.exit("not an Android boot image")
    ks, rs, osv, hs = struct.unpack_from("<IIII", img, 8)
    hv, = struct.unpack_from("<I", img, 40)
    sig, = struct.unpack_from("<I", img, 1580)
    if hv != 4 or rs != 0 or sig != 0:
        sys.exit(f"unexpected header: version {hv}, ramdisk {rs}, signature_size {sig}")
    f = img[-FOOTER:]
    if f[:4] != b"AVBf":
        sys.exit("no AVB footer at the end of the partition image")
    major, minor, orig, vbo, vbs = struct.unpack_from(">IIQQQ", f, 4)
    if img[vbo:vbo + 4] != b"AVB0":
        sys.exit("AVB footer does not point at a vbmeta blob")
    kend = PAGE + pad(ks)
    return dict(kernel_size=ks, kernel=img[PAGE:PAGE + ks], trailer=img[kend:orig],
                orig=orig, vbmeta=img[vbo:vbo + vbs], footer=f)


CMDLINE_OFF, CMDLINE_LEN = 44, 1536      # boot header v4 cmdline (LK appends it)


def assemble(stock, kernel, cmdline=b""):
    s = parse(stock)
    hdr = bytearray(stock[:PAGE])
    struct.pack_into("<I", hdr, 8, len(kernel))
    if cmdline:
        if len(cmdline) >= CMDLINE_LEN or any(hdr[CMDLINE_OFF:CMDLINE_OFF + CMDLINE_LEN]):
            sys.exit("--cmdline: too long, or the stock header already has one")
        hdr[CMDLINE_OFF:CMDLINE_OFF + len(cmdline)] = cmdline
    body = bytes(hdr) + bytes(kernel) + b"\0" * (pad(len(kernel)) - len(kernel)) + s["trailer"]
    orig = len(body)
    if orig + len(s["vbmeta"]) > len(stock) - FOOTER:
        sys.exit("kernel too large for the partition")
    img = bytearray(len(stock))
    img[:orig] = body
    img[orig:orig + len(s["vbmeta"])] = s["vbmeta"]
    footer = bytearray(s["footer"])
    struct.pack_into(">QQ", footer, 12, orig, orig)        # original_image_size, vbmeta_offset
    img[-FOOTER:] = footer
    return bytes(img)


STUB_AREA = 0x10000


def stock_image(stock):
    """The stock boot image's own decompressed Linux arm64 Image."""
    raw = lz4.decompress(parse(stock)["kernel"])
    if raw[0x38:0x3C] != b"ARMd":
        sys.exit("stock kernel is not a Linux arm64 Image")
    return raw


def build_image(stock, image, blob, android=None, cmdline=b""):
    if image[0x38:0x3C] != b"ARMd":
        sys.exit("--image is not a Linux arm64 Image")
    raw = bytearray(image)
    if blob:
        raw += b"\0" * (-len(raw) % 0x1000)
        raw += blob
    if android:
        # tools/lapis_shim/dual.c: Android's Image at a 2 MiB boundary, then a
        # free area for chain_stub, which copies the Image down to the shim's
        # load address. The area must lie beyond everything that Image covers
        # there (its image_size, bss included) and beyond its own source.
        android = bytes(android) + b"\0" * (-len(android) % 16)
        a_size, = struct.unpack_from("<Q", android, 16)
        raw += b"\0" * (-len(raw) % 0x200000)
        a_off = len(raw)
        raw += android
        raw += b"\0" * (-len(raw) % 0x1000)
        stub_off = len(raw)
        if stub_off < a_size:
            sys.exit("stub area would be overwritten by Android's kernel")
        raw += b"\0" * STUB_AREA
        struct.pack_into("<QQQ", raw, 0x20, a_off, len(android), stub_off)
    struct.pack_into("<Q", raw, 16, len(raw))             # image_size covers the payload
    return assemble(stock, lz4.compress(bytes(raw)), cmdline), bytes(raw)


def build(stock, payload):
    s = parse(stock)
    blocks = lz4.split_blocks(s["kernel"])
    raw0 = lz4.decompress_block(s["kernel"][blocks[0][0]:blocks[0][0] + blocks[0][1]])
    if raw0[0x38:0x3C] != b"ARMd":
        sys.exit("stock kernel is not a Linux arm64 Image")
    if PAYLOAD_OFF + len(payload) > 0x10000:
        sys.exit("payload larger than 60 KiB")
    raw = bytearray(raw0)
    raw[0:4] = BRANCH_TO_PAYLOAD
    raw[PAYLOAD_OFF:PAYLOAD_OFF + len(payload)] = payload
    new0 = lz4.compress_block(bytes(raw))
    kernel = bytearray(struct.pack("<I", lz4.MAGIC))
    kernel += struct.pack("<I", len(new0)) + new0
    for off, size in blocks[1:]:
        kernel += s["kernel"][off - 4:off + size]

    return assemble(stock, kernel), raw0


def verify(stock, out, payload, raw0_stock):
    """Re-parse OUT and prove it differs from STOCK only where intended."""
    a, b = parse(stock), parse(out)
    if stock[:8] + stock[12:PAGE] != out[:8] + out[12:PAGE]:
        sys.exit("verify: header changed beyond kernel_size")
    if a["trailer"] != b["trailer"] or a["vbmeta"] != b["vbmeta"]:
        sys.exit("verify: boot signature or vbmeta changed")
    if a["footer"][:12] != b["footer"][:12] or a["footer"][28:] != b["footer"][28:]:
        sys.exit("verify: AVB footer changed beyond the two offsets")
    sa, sb = lz4.split_blocks(a["kernel"]), lz4.split_blocks(b["kernel"])
    if len(sa) != len(sb) or any(a["kernel"][o:o + n] != b["kernel"][p:p + m]
                                 for (o, n), (p, m) in zip(sa[1:], sb[1:])):
        sys.exit("verify: kernel blocks 1..n are not byte-identical")
    raw0 = lz4.decompress_block(b["kernel"][sb[0][0]:sb[0][0] + sb[0][1]])
    diff = [i for i in range(len(raw0)) if raw0[i] != raw0_stock[i]] if len(raw0) == len(raw0_stock) else None
    if diff is None:
        sys.exit("verify: block 0 changed size")
    allowed = set(range(4)) | set(range(PAYLOAD_OFF, PAYLOAD_OFF + len(payload)))
    if not set(diff) <= allowed or raw0[PAYLOAD_OFF:PAYLOAD_OFF + len(payload)] != payload:
        sys.exit("verify: decompressed block 0 differs outside the branch and payload")
    return len(diff)


def verify_layout(stock, out, cmdline=b""):
    a, b = parse(stock), parse(out)
    o = bytearray(out[:PAGE])
    if cmdline and o[CMDLINE_OFF:CMDLINE_OFF + len(cmdline)] == cmdline:
        o[CMDLINE_OFF:CMDLINE_OFF + len(cmdline)] = bytes(len(cmdline))
    if stock[:8] + stock[12:PAGE] != bytes(o[:8]) + bytes(o[12:PAGE]):
        sys.exit("verify: header changed beyond kernel_size")
    if a["trailer"] != b["trailer"] or a["vbmeta"] != b["vbmeta"]:
        sys.exit("verify: boot signature or vbmeta changed")
    if a["footer"][:12] != b["footer"][:12] or a["footer"][28:] != b["footer"][28:]:
        sys.exit("verify: AVB footer changed beyond the two offsets")
    return b


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("stock"); ap.add_argument("out")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--probe", help="small payload placed at Image+0x1000 of the stock kernel")
    g.add_argument("--image", help="replacement arm64 Image (e.g. the lapis shim)")
    ap.add_argument("--append", help="with --image: blob appended at the next 4 KiB boundary")
    ap.add_argument("--android", action="store_true",
                    help="with --image: also carry STOCK's own Android kernel (dual boot, lapis_shim dual)")
    ap.add_argument("--cmdline", default="",
                    help="with --image: text for the boot header cmdline (LK appends it to bootargs)")
    ap.add_argument("--expect-stock-sha256")
    a = ap.parse_args()
    if a.append and not a.image:
        ap.error("--append needs --image")
    stock = open(a.stock, "rb").read()
    h = hashlib.sha256(stock).hexdigest()
    if a.expect_stock_sha256 and h != a.expect_stock_sha256:
        sys.exit(f"stock image sha256 {h} does not match")
    s = parse(stock)
    if a.probe:
        payload = open(a.probe, "rb").read()
        out, raw0 = build(stock, payload)
        changed = verify(stock, out, payload, raw0)
        what = (f"{changed} bytes of the decompressed Image differ, all inside code0 "
                f"and the {len(payload)}-byte payload")
        how = "LZ4 block 0 recompressed, others copied"
    else:
        image = open(a.image, "rb").read()
        blob = open(a.append, "rb").read() if a.append else b""
        android = stock_image(stock) if a.android else None
        out, raw = build_image(stock, image, blob, android, a.cmdline.encode())
        if android and raw[struct.unpack_from("<Q", raw, 0x20)[0]:][:len(android)] != android:
            sys.exit("verify: Android Image not where the header says")
        o = verify_layout(stock, out, a.cmdline.encode())
        if lz4.decompress(o["kernel"]) != raw:
            sys.exit("verify: decompressed kernel differs from Image + payload")
        what = (f"decompressed kernel == Image ({len(image)} B) + payload ({len(blob)} B)"
                + (f" + Android Image ({len(android)} B at {struct.unpack_from('<Q', raw, 0x20)[0]:#x})" if android else "")
                + ", "
                f"image_size {len(raw):#x}")
        how = "whole Image compressed"
    with open(a.out, "wb") as f:
        f.write(out)
    o = parse(out)
    print(f"stock   sha256 {h}")
    print(f"out     sha256 {hashlib.sha256(out).hexdigest()}  ({len(out)} bytes, same as partition)")
    print(f"kernel  {s['kernel_size']} -> {o['kernel_size']} bytes ({how})")
    print(f"AVB     original_image_size {s['orig']:#x} -> {o['orig']:#x} (hash now stale)")
    print(f"verify  OK: {what}")


if __name__ == "__main__":
    main()
