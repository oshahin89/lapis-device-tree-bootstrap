#!/usr/bin/env python3
"""Run a lapis first-stage payload in QEMU virt and check what it drew.

  qemu_check.py PAYLOAD.bin OUTDIR [--secs 3]

QEMU virt RAM starts at 0x40000000; with 4 GiB it covers the lapis LK
framebuffer reservation (0xfd72f000 + 0x28d0000) as plain memory. The payload
is loaded at 0x40200000 (above QEMU's DTB) and started there with
the MMU off, like LK's jump. Afterwards each of the three derived buffers is
saved as bufN.png and its probe pattern is checked. This tests the payload
code only, not LK or the panel.
"""
import argparse, json, os, socket, struct, subprocess, sys, time, zlib

FB, FRAME, STRIDE, W, H = 0xFD72F000, 0xD98000, 5120, 1280, 2772
LOAD = 0x40200000   # above the 1 MiB DTB QEMU places at RAM base


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
        rgb[0::3], rgb[1::3], rgb[2::3] = line[2::4], line[1::4], line[0::4]   # ARGB8888 LE
        rows.append(b"\0" + bytes(rgb))
    c = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + c(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                           + c(b"IDAT", zlib.compress(b"".join(rows), 6)) + c(b"IEND", b""))


def px(raw, x, y):
    return struct.unpack_from("<I", raw, y * STRIDE + x * 4)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("payload"); ap.add_argument("outdir")
    ap.add_argument("--secs", type=float, default=3)
    a = ap.parse_args()
    out = os.path.abspath(a.outdir); os.makedirs(out, exist_ok=True)
    mon = os.path.join(out, "m.sock")
    if len(mon) >= 108:
        sys.exit("OUTDIR path too long for a QMP unix socket")
    if os.path.exists(mon):
        os.unlink(mon)
    p = subprocess.Popen(["qemu-system-aarch64", "-M", "virt,gic-version=3", "-cpu", "cortex-a55",
                          "-m", "4G", "-nographic", "-serial", "null", "-monitor", "none",
                          "-qmp", f"unix:{mon},server,nowait", "-no-reboot",
                          "-device", f"loader,file={os.path.abspath(a.payload)},addr={LOAD:#x},force-raw=on",
                          "-device", f"loader,addr={LOAD:#x},cpu-num=0"], stdin=subprocess.DEVNULL)
    time.sleep(a.secs)
    if p.poll() is not None:
        sys.exit(f"qemu exited early: {p.returncode}")
    calls = [("stop", {})] + [("pmemsave", {"val": FB + i * FRAME, "size": FRAME,
                                           "filename": f"{out}/buf{i}.raw"}) for i in range(3)]
    calls.append(("human-monitor-command", {"command-line": "info registers"}))
    r = qmp(mon, *calls)
    p.kill(); p.wait()
    open(f"{out}/regs.txt", "w").write(r[-1].get("return", ""))

    ok = True
    for i in range(3):
        raw = open(f"{out}/buf{i}.raw", "rb").read()
        png(f"{out}/buf{i}.png", raw)
        bands = sum(px(raw, 640, 256 + 314 * k + 157) == 0xFF00FF00 for k in range(8))
        checks = {
            "top_band_ARGB_red": px(raw, 640, 100) == 0xFFFF0000,
            "white_stripe": all(px(raw, x, y) == 0xFFFFFFFF for x in (0, 63) for y in (0, 1500, H - 1)),
            "no_stripe_past_63": px(raw, 64, 1500) != 0xFFFFFFFF,
            "green_bands": bands,
            "last_row_drawn": px(raw, W - 1, H - 1) in (0xFF00FF00, 0xFF202020),
        }
        good = (checks["top_band_ARGB_red"] and checks["white_stripe"] and checks["no_stripe_past_63"]
                and bands == i + 1 and checks["last_row_drawn"])
        ok &= good
        print(f"buf{i}: {'OK' if good else 'BAD'} {checks}")
        os.unlink(f"{out}/buf{i}.raw")
    pc = [l for l in r[-1].get("return", "").splitlines() if l.strip().startswith("PC=")]
    print(pc[0].strip() if pc else "PC unknown")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
