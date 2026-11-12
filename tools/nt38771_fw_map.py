#!/usr/bin/env python3
"""Parse a Novatek NT387xx host-download firmware image (read-only, host side).

  nt38771_fw_map.py device/novatek_ts_fw_tm.bin

Re-implements the partition-table walk of Novatek's GPL driver
(`nvt_bin_header_parser` in nt387xx_fw_update.c, `ilm_dlm_num = 2`) so the
download plan for an XNU driver can be checked against the real image:

  ILM/DLM:    BIN_addr, SRAM_addr, size at 0x00 + 12*n; crc at 0x18 + 4*n
  info/header sections: 16-byte records from 0x30 (SRAM, size, BIN, crc);
              the list ends at the first record with size != 0 and BIN == 0
  overlays:   if bit 4 of byte 0x28, (byte 0x28 & 0x0f) records at DLM BIN
  cascade:    bit 1 of byte 0x20 (second header), reported only

The driver writes each partition (size + 1 bytes, as in nvt_write_firmware)
to SRAM in NVT_TRANSFER_LEN chunks, then programs the ILM/DLM destination,
length and golden CRC into the bootloader CRC bank. Nothing is sent anywhere.
"""
import struct
import sys

NVT_TRANSFER_LEN = 15 * 1024


def w(b, o):
    return struct.unpack_from("<I", b, o)[0]


def parse(fw):
    end = w(fw, 0)
    pos = 0x30
    while pos < end:
        if w(fw, pos + 4) != 0 and w(fw, pos + 8) == 0:
            end = pos + 0x10
            break
        pos += 0x10
    info = 0
    pos = 0x30
    while pos < end:
        info += 1
        pos += 0x10
    cascade = (fw[0x20] & 0x02) >> 1
    if cascade:
        info += 1
        end *= 2
    ovly_info = (fw[0x28] & 0x10) >> 4
    ovly = (fw[0x28] & 0x0F) if ovly_info else 0

    parts = []
    for n in range(2):
        parts.append(dict(name=("ILM", "DLM")[n], BIN=w(fw, 0 + 12 * n), SRAM=w(fw, 4 + 12 * n),
                          size=w(fw, 8 + 12 * n), crc=w(fw, 0x18 + 4 * n)))
    found_header = False
    for i in range(info):
        p = 0x30 + 0x10 * i if not found_header else end - 0x10
        rec = dict(SRAM=w(fw, p), size=w(fw, p + 4), BIN=w(fw, p + 8), crc=w(fw, p + 12))
        if rec["BIN"] < end and rec["size"] != 0:
            rec["name"] = "Header"
            found_header = True
        else:
            rec["name"] = f"Info-{i}"
        parts.append(rec)
    for i in range(ovly):
        p = parts[1]["BIN"] + 0x10 * i
        parts.append(dict(name=f"Overlay-{i}", SRAM=w(fw, p), size=w(fw, p + 4),
                          BIN=w(fw, p + 8), crc=w(fw, p + 12)))
    return dict(end=end, info=info, cascade=cascade, ovly=ovly, parts=parts)


def main():
    fw = open(sys.argv[1], "rb").read()
    m = parse(fw)
    print(f"image {len(fw)} bytes; header end 0x{m['end']:x}; info sections {m['info']}; "
          f"overlays {m['ovly']}; cascade {m['cascade']}")
    total = 0
    bad = False
    for p in m["parts"]:
        ok = p["BIN"] + p["size"] <= len(fw)
        bad |= not ok
        n = p["size"] + 1 if p["size"] else 0
        total += n
        chunks = -(-n // NVT_TRANSFER_LEN) if n else 0
        print(f"  {p['name']:10} SRAM 0x{p['SRAM']:06x}  BIN 0x{p['BIN']:06x}  size 0x{p['size']:06x}  "
              f"crc 0x{p['crc']:08x}  chunks {chunks}{'' if ok else '  OUT OF RANGE'}")
    print(f"bytes to write {total} (+ page/CRC commands)")
    fwv = fw[0x2f000:0x2f002] if len(fw) > 0x2f002 else b""
    if fwv:
        print(f"fw version byte at 0x2f000: 0x{fwv[0]:02x} (complement 0x{fwv[1]:02x})")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
