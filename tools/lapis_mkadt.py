#!/usr/bin/env python3
"""Build the minimal Apple Device Tree (ADT) for the SWEET-config XNU on lapis.

The ios-sweet/ios-topaz kernel (MACHINE_CONFIG=SWEET) takes its hardware
addresses from the ADT, not from compile-time constants. This script writes an
ADT with the values measured on the Redmi 25080RABDG / lapis (MT6878): see
device/lapis_boot_contract.md and device/runtime_iomem.txt.

It is an independent implementation of the ADT layout (no upstream code):
  node = u32 nprops, u32 nchildren, props..., children...
  prop = char name[32], u32 size (bit 31 = placeholder flag), value padded to 4

Usage:
  lapis_mkadt.py OUT.adt [--fb-base ADDR] [--fb-stride BYTES] [--product D22AP.adt]
  lapis_mkadt.py OUT.adt --qemu          (QEMU virt values, single CPU, no UFS)
  lapis_mkadt.py --dump FILE.adt

Framebuffer: measured on the device with the stage 1 probe (2026-11-11):
ARGB8888, LK scans out buffer 0 at 0xfd72f000, stride 5120. These are now
the phone defaults; --fb-base/--fb-stride remain for experiments.
"""
import argparse
import hashlib
import os
import struct
import sys

# --- lapis measurements (runtime FDT / iomem, OS3.0.309.0 slot B) -----------
# Kernel RAM: inside the 0x140000000-0x33d7fffff System RAM bank, below the
# first nested reservation at 0x32df00000 (4 GiB used, same size as topaz).
PHYS_BASE, MEM_SIZE = 0x140000000, 0x100000000
SOC_BASE = 0x01000000                       # /arm-io ranges base; must be non-zero
GICD, GICD_SIZE = 0x0C000000, 0x40000       # arm,gic-v3 distributor
GICR, GICR_SIZE = 0x0C040000, 0x200000      # redistributors, 8 CPUs
TIMEBASE = 13000000                         # arm,armv8-timer clock-frequency
VTIMER_INTID = 27                           # PPI 11 -> INTID 27
UFS, UFS_SIZE = 0x112B0000, 0x2300          # 112b0000.ufshci (iomem)
FB_RESERVED, FB_RESERVED_SIZE = 0xFD72F000, 0x028D0000   # mblock-15-framebuffer
FB_W, FB_H = 1280, 2772                     # p16_36_02_0b_dsc_vdo
FB_STRIDE = 5120                            # measured: ARGB8888, buffer 0 at FB_RESERVED
# Little cluster only: Cortex-A55 (MIDR part 0xD05), MPIDR Aff1 = 0..3. The
# Linux boot CPU is CPU0 (MPIDR 0). The A78 cluster (0x400-0x700) stays out
# until PSCI bring-up of the second cluster is verified.
A55_MPIDRS = (0x000, 0x100, 0x200, 0x300)

QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE, QEMU_VTIMER = 0x08000000, 0x080A0000, 62500000, 27
# QEMU only: the framebuffer the host runner reads back as plain RAM. It is
# the address the sweet 0010/0012 early console still hard-codes, not lapis'.
QEMU_FB, QEMU_FB_W, QEMU_FB_H = 0x9C000000, 1080, 2400

ECID = 0x4C41504953000001                   # 'LAPIS' + 1, synthetic
BOOT_MANIFEST_HASH = hashlib.sha384(b"lapis boot manifest").digest()

u32 = lambda *v: struct.pack("<%dI" % len(v), *v)
u64 = lambda *v: struct.pack("<%dQ" % len(v), *v)
cstr = lambda *s: b"".join(x.encode() + b"\0" for x in s)


class Node:
    def __init__(self, name, props=(), children=()):
        self.props = [("name", 0, cstr(name))] + [(k, 0, v) for k, v in props]
        self.children = list(children)


def serialize(n):
    out = [u32(len(n.props), len(n.children))]
    for key, flag, val in n.props:
        k = key.encode()
        if len(k) >= 32:
            raise ValueError(f"property name too long: {key}")
        out.append(k.ljust(32, b"\0"))
        out.append(u32(len(val) | (flag << 31)))
        out.append(val + b"\0" * (-len(val) % 4))
    out += [serialize(c) for c in n.children]
    return b"".join(out)


def parse(buf, off=0):
    nprops, nchildren = struct.unpack_from("<II", buf, off)
    off += 8
    props = []
    for _ in range(nprops):
        key = buf[off:off + 32].split(b"\0")[0].decode()
        size, = struct.unpack_from("<I", buf, off + 32)
        ln = size & 0x7FFFFFFF
        props.append((key, size >> 31, buf[off + 36:off + 36 + ln]))
        off += 36 + ((ln + 3) & ~3)
    n = Node("?")
    n.props = props
    for _ in range(nchildren):
        child, off = parse(buf, off)
        n.children.append(child)
    return n, off


def dump(n, depth=0):
    pad = "  " * depth
    for key, flag, val in n.props:
        if val.endswith(b"\0") and all(32 <= c < 127 or c == 0 for c in val):
            shown = repr(val.rstrip(b"\0").replace(b"\0", b" | ").decode())
        else:
            shown = val[:32].hex() + ("..." if len(val) > 32 else "")
        print(f"{pad}{key}{' (placeholder)' if flag else ''} = {shown}")
    for c in n.children:
        print(f"{pad}{{")
        dump(c, depth + 1)
        print(f"{pad}}}")


def product_props(path):
    """/product of a user-supplied D22AP ADT (Apple material, never in git)."""
    if not path:
        return None
    root, _ = parse(open(path, "rb").read())
    for c in root.children:
        if dict((k, v) for k, _, v in c.props).get("name") == b"product\0":
            return [(k, v) for k, f, v in c.props if not f and k not in ("name", "AAPL,phandle")]
    sys.exit(f"{path}: no /product node")


def build(qemu, fb_base, fb_stride, product):
    ids = iter(range(1, 1000))
    ph = lambda: ("AAPL,phandle", u32(next(ids)))
    gicd, gicr, timebase, vtimer = ((QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE, QEMU_VTIMER)
                                    if qemu else (GICD, GICR, TIMEBASE, VTIMER_INTID))

    chosen = Node("chosen", [
        ph(),
        ("dram-base", u64(PHYS_BASE)),
        ("dram-size", u64(MEM_SIZE)),
        ("random-seed", os.urandom(64)),
        ("debug-enabled", u32(1)),
        ("firmware-version", cstr("lapis-shim")),
        ("system-firmware-version", cstr("lapis-shim")),
        ("boot-manifest-hash", BOOT_MANIFEST_HASH),
        ("crypto-hash-method", cstr("sha2-384")),
        # 1280x2772 at 3x; the presented canvas is decided by disp0 below.
        ("display-scale", u32(3)),
        ("unique-chip-id", u64(ECID)),
        ("chip-id", u32(0x8015)),               # A11, what the iPhone X userland expects
        ("certificate-production-status", u32(0)),
        ("effective-production-status-ap", u32(0)),
        ("certificate-security-mode", u32(1)),
        ("effective-security-mode-sep", u32(1)),
    ], [Node("memory-map", [ph()]),
        Node("asmb", [ph(), ("lp-sip0", u64(0x6F)), ("lp-sip1", u32(1))])])

    mpidrs = (0,) if qemu else A55_MPIDRS
    cpus = Node("cpus", [ph(), ("#address-cells", u32(1)), ("#size-cells", u32(0))], [
        Node(f"cpu{i}", [
            ph(),
            ("device_type", cstr("cpu")),
            # qcom,kryo470 is the IONameMatch of the SWEET CPU driver; it is a
            # match key, not a claim about the hardware.
            ("compatible", cstr("arm,cortex-a55", "qcom,kryo470", "ARM,v8")),
            ("reg", u32(mpidr)),                 # MPIDR, not the logical CPU ID
            ("cpu-id", u32(i)),
            ("cluster-id", u32(0)),
            ("cluster-type", u32(0)),
            ("die-cluster-id", u32(0)),
            ("cluster-core-id", u32(i)),
            ("l2-cache-id", u32(0)),
            ("l3-cache-id", u32(0)),
            ("state", cstr("running" if i == 0 else "stopped")),
            ("timebase-frequency", u32(timebase)),
            ("clock-frequency", u32(2000000000)),
            ("bus-frequency", u32(100000000)),
            ("memory-frequency", u32(1866000000)),
            ("peripheral-frequency", u32(timebase)),
            ("fixed-frequency", u32(timebase)),
        ]) for i, mpidr in enumerate(mpidrs)])

    arm_io = Node("arm-io", [
        ph(),
        ("device_type", cstr("sweet-io")),
        ("compatible", cstr("sweet-io", "arm-io")),
        ("#address-cells", u32(2)),
        ("#size-cells", u32(2)),
        ("ranges", u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [
        Node("gic", [
            ph(),
            ("compatible", cstr("arm,gic-v3")),
            ("interrupt-controller", b""),
            ("virtual-timer-intid", u32(vtimer)),
            ("reg", u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
        ]),
        Node("wdt", [ph(), ("no-pmu", u32(1))]),
        Node("disp0", [
            ph(),
            ("device_type", cstr("display-subsystem")),
            ("compatible", cstr("sweet,display")),
            ("canvas-width", u32(1125)),         # iPhone X canvas
            ("canvas-height", u32(2436)),
        ]),
    ])

    children = [chosen, Node("defaults", [ph(), ("no-effaceable-storage", u32(1))]), cpus, arm_io]
    if qemu:
        children.append(Node("sweet-fbcon", [
            ph(),
            ("reg", u64(QEMU_FB, QEMU_FB_W * 4 * QEMU_FB_H)),
            ("width", u32(QEMU_FB_W)),
            ("height", u32(QEMU_FB_H)),
            ("stride", u32(QEMU_FB_W * 4)),
        ]))
    else:
        children.append(Node("sweet-fbcon", [
            ph(),
            ("reg", u64(fb_base, FB_RESERVED + FB_RESERVED_SIZE - fb_base)),
            ("width", u32(FB_W)),
            ("height", u32(FB_H)),
            ("stride", u32(fb_stride)),
        ]))
        children.append(Node("sweet-ufs", [
            ph(),
            ("compatible", cstr("mediatek,mt8183-ufshci")),
            ("reg", u64(UFS, UFS_SIZE)),
        ]))
    children.append(Node("buttons", [
        ph(),
        ("button-names", cstr("volup", "voldown", "hold")),
        ("home-button-type", u32(2)),
        ("opposed-power-vol-buttons", u32(1)),
    ]))
    if product:
        children.append(Node("product", [ph()] + product))

    # xiaomi,sweet is the SweetPlatformExpert IONameMatch; keep it.
    return Node("device-tree", [
        ph(),
        ("compatible", cstr("D22AP", "iPhone10,3", "xiaomi,lapis", "xiaomi,sweet",
                            "mediatek,mt6878", "AppleARM")),
        ("model", cstr("iPhone10,3")),
        ("target-type", cstr("D22")),
        ("target-sub-type", cstr("D22AP")),
        ("manufacturer", cstr("Apple Inc.")),
        ("#address-cells", u32(2)),
        ("#size-cells", u32(2)),
    ], children)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out", nargs="?")
    ap.add_argument("--qemu", action="store_true")
    ap.add_argument("--fb-base", type=lambda s: int(s, 0), default=FB_RESERVED)
    ap.add_argument("--fb-stride", type=lambda s: int(s, 0), default=FB_STRIDE)
    ap.add_argument("--product", help="D22AP ADT supplying /product (not in git)")
    ap.add_argument("--dump", metavar="ADT")
    a = ap.parse_args()

    if a.dump:
        dump(parse(open(a.dump, "rb").read())[0])
        return
    if not a.out:
        ap.error("OUT.adt is required")
    if not a.qemu:
        if a.fb_stride < FB_W * 4 or a.fb_stride % 4:
            ap.error(f"--fb-stride {a.fb_stride} is below 1280*4 or not 4-byte aligned")
        end = a.fb_base + a.fb_stride * FB_H
        if not (FB_RESERVED <= a.fb_base and end <= FB_RESERVED + FB_RESERVED_SIZE):
            ap.error("framebuffer does not fit inside the LK reservation "
                     f"0x{FB_RESERVED:x}+0x{FB_RESERVED_SIZE:x}")
    if not a.product:
        print("warning: no --product; MobileGestalt will lack /product", file=sys.stderr)

    blob = serialize(build(a.qemu, a.fb_base, a.fb_stride, product_props(a.product)))
    with open(a.out, "wb") as f:
        f.write(blob)
    print(f"{a.out}: {len(blob)} bytes", file=sys.stderr)


if __name__ == "__main__":
    main()
