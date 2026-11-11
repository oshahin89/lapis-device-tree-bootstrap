#!/usr/bin/env python3
"""Run the lapis shim (+ appended SWEETXNU payload) in QEMU virt like LK would.

  qemu_shim.py IMAGE OUTDIR [--secs 60] [--kernel KERNEL]

IMAGE is the decompressed kernel the packer puts into boot_b (shim Image,
payload at the next 4 KiB boundary). LK places it at 0x40000000; QEMU virt
keeps its own DTB there, so it is loaded at 0x40200000 instead (the shim is
position independent) and entered with x0 = 0 (no DTB). RAM is 12 GiB so the
lapis kernel bank (0x140000000 + 4 GiB) and the LK framebuffer (0xfd72f000)
exist as plain memory. Use a payload built with `lapis_mkadt.py --qemu` so the
kernel finds QEMU's GIC; the shim itself still uses the lapis framebuffer.

Afterwards OUTDIR has screen.png (the lapis framebuffer), screen.txt (text
decoded with the shim's 3x font and, with --kernel, xnu's 2x iso_font),
regs.txt and qemu.log. This tests the shim and loader logic only: no LK, no
GenieZone stage 2, no real hardware.
"""
import argparse, json, os, socket, struct, subprocess, sys, time, zlib

FB, W, H, STRIDE = 0xFD72F000, 1280, 2772, 5120
LOAD = 0x40200000
HERE = os.path.dirname(os.path.abspath(__file__))


def qmp(path, *calls):
    s = socket.socket(socket.AF_UNIX); s.connect(path)
    f = s.makefile("rw"); f.readline()
    out = []
    for ex, args in (("qmp_capabilities", {}),) + calls:
        f.write(json.dumps({"execute": ex, "arguments": args}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r:
                out.append(r); break
    s.close()
    return out[1:]


def png(path, raw):
    rows = []
    for y in range(H):
        line = raw[y * STRIDE:y * STRIDE + W * 4]
        rgb = bytearray(W * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = line[2::4], line[1::4], line[0::4]
        rows.append(b"\0" + bytes(rgb))
    c = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + c(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                           + c(b"IDAT", zlib.compress(b"".join(rows), 6)) + c(b"IEND", b""))


def shim_font():
    """font8x16[95][16] from font.h (rows MSB = leftmost pixel)."""
    import re
    rows = re.findall(r"\{([0-9a-fx,]+)\}", open(os.path.join(HERE, "font.h")).read())
    return [bytes(int(v, 16) for v in r.split(",")) for r in rows]


def decode(raw, glyphs, scale, cw, ch, x0, y0, msb_left, fg_test):
    """Exact glyph matching on a fixed text grid; '?' for unknown cells."""
    table = {g: chr(32 + i) for i, g in enumerate(glyphs)}
    lines = []
    for row in range((H - y0) // ch):
        chars = []
        for col in range((W - x0) // cw):
            cell = bytearray(16)
            for gy in range(16):
                for gx in range(8):
                    o = (y0 + row * ch + gy * scale) * STRIDE + (x0 + col * cw + gx * scale) * 4
                    if fg_test(struct.unpack_from("<I", raw, o)[0]):
                        cell[gy] |= (0x80 >> gx) if msb_left else (1 << gx)
            chars.append(table.get(bytes(cell), "?"))
        lines.append("".join(chars).rstrip("? ").rstrip())
    return "\n".join(l for l in lines if l.strip("? ")) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image"); ap.add_argument("outdir")
    ap.add_argument("--secs", type=float, default=60)
    ap.add_argument("--kernel", help="unstripped xnu (for its _iso_font)")
    a = ap.parse_args()
    out = os.path.abspath(a.outdir); os.makedirs(out, exist_ok=True)
    mon = os.path.join(out, "m.sock")
    if len(mon) >= 108:
        sys.exit("OUTDIR path too long for a QMP unix socket")
    if os.path.exists(mon):
        os.unlink(mon)
    cmd = ["qemu-system-aarch64", "-M", "virt,gic-version=3", "-cpu", "cortex-a76", "-smp", "1",
           "-m", "12G", "-nographic", "-serial", f"file:{out}/serial.txt", "-monitor", "none",
           "-qmp", f"unix:{mon},server,nowait", "-no-reboot",
           "-d", "int,guest_errors,unimp", "-D", f"{out}/qemu.log",
           "-device", f"loader,file={os.path.abspath(a.image)},addr={LOAD:#x},force-raw=on",
           "-device", f"loader,addr={LOAD:#x},cpu-num=0"]
    p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL)
    t0 = time.time()
    while p.poll() is None and time.time() - t0 < a.secs:
        time.sleep(0.2)
    if p.poll() is not None:
        sys.exit(f"qemu exited early: {p.returncode}")
    r = qmp(mon, ("stop", {}), ("pmemsave", {"val": FB, "size": STRIDE * H, "filename": f"{out}/fb.raw"}),
            ("human-monitor-command", {"command-line": "info registers"}))
    p.kill(); p.wait()
    open(f"{out}/regs.txt", "w").write(r[-1].get("return", ""))
    raw = open(f"{out}/fb.raw", "rb").read()
    png(f"{out}/screen.png", raw)
    text = ["== shim console (3x font) ==",
            decode(raw, shim_font(), 3, 24, 48, 24, 24, True, lambda v: v in (0xFFFFFFFF, 0xFFFFFF00))]
    if a.kernel:
        sys.path.insert(0, os.path.dirname(HERE))
        syms = subprocess.check_output(["llvm-nm", "--defined-only", a.kernel], text=True)
        va = [int(l.split()[0], 16) for l in syms.splitlines() if l.split()[-1:] == ["_iso_font"]][0]
        kern = open(a.kernel, "rb").read()
        # find the segment holding _iso_font in the Mach-O
        ncmds, = struct.unpack_from("<I", kern, 16); off = 32; font = None
        for _ in range(ncmds):
            c, sz = struct.unpack_from("<II", kern, off)
            if c == 0x19:
                vm, vs, fo, fs = struct.unpack_from("<4Q", kern, off + 24)
                if vm <= va < vm + fs:
                    font = kern[fo + va - vm:fo + va - vm + 256 * 16]
            off += sz
        if font:
            glyphs = [font[(32 + i) * 16:(33 + i) * 16] for i in range(95)]
            text += ["== xnu console (2x iso_font) ==",
                     decode(raw, glyphs, 2, 16, 32, 0, 0, False, lambda v: v == 0xFFFFFFFF)]
    open(f"{out}/screen.txt", "w").write("\n".join(text))
    os.unlink(f"{out}/fb.raw")
    pc = [l.strip() for l in r[-1].get("return", "").splitlines() if l.strip().startswith("PC=")]
    print(pc[0] if pc else "PC unknown")
    print(f"{out}/screen.png {out}/screen.txt")


if __name__ == "__main__":
    main()
