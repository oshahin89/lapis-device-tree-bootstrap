#!/usr/bin/env python3
"""Inspect lapis Android boot v4 images without modifying them.

This checks the Android boot/vendor_boot v4 component boundaries and hashes
their payloads. It does not validate AVB signatures or make an image bootable.
Header layouts follow AOSP system/tools/mkbootimg/include/bootimg/bootimg.h.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from typing import Any


BOOT_MAGIC = b"ANDROID!"
VENDOR_BOOT_MAGIC = b"VNDRBOOT"
BOOT_PAGE_SIZE = 4096
BOOT_V4_HEADER_SIZE = 1584
VENDOR_BOOT_V4_HEADER_SIZE = 2128
VENDOR_RAMDISK_TABLE_ENTRY_V4_SIZE = 108


class ImageError(ValueError):
    pass


def u32(data: bytes, offset: int, field: str) -> int:
    if offset + 4 > len(data):
        raise ImageError(f"truncated while reading {field}")
    return struct.unpack_from("<I", data, offset)[0]


def align(value: int, page_size: int) -> int:
    return (value + page_size - 1) & ~(page_size - 1)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def component(data: bytes, name: str, offset: int, size: int) -> dict[str, Any]:
    end = offset + size
    if offset < 0 or size < 0 or end > len(data):
        raise ImageError(
            f"{name} range 0x{offset:x}..0x{end:x} exceeds image size 0x{len(data):x}"
        )
    payload = data[offset:end]
    return {"offset": offset, "size": size, "sha256": sha256(payload)}


def read_image(path: Path) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        raise ImageError(f"cannot read {path}: {exc}") from exc


def parse_boot_v4(path: Path) -> dict[str, Any]:
    data = read_image(path)
    if len(data) < BOOT_PAGE_SIZE or data[:8] != BOOT_MAGIC:
        raise ImageError(f"{path}: missing ANDROID! magic or first header page")

    kernel_size = u32(data, 8, "kernel_size")
    ramdisk_size = u32(data, 12, "ramdisk_size")
    header_size = u32(data, 20, "header_size")
    header_version = u32(data, 40, "header_version")
    signature_size = u32(data, 1580, "signature_size")
    if header_version != 4:
        raise ImageError(f"{path}: expected boot header v4, got v{header_version}")
    if header_size != BOOT_V4_HEADER_SIZE:
        raise ImageError(
            f"{path}: expected v4 header size {BOOT_V4_HEADER_SIZE}, got {header_size}"
        )

    kernel_offset = BOOT_PAGE_SIZE
    ramdisk_offset = align(kernel_offset + kernel_size, BOOT_PAGE_SIZE)
    signature_offset = align(ramdisk_offset + ramdisk_size, BOOT_PAGE_SIZE)
    payload_end = align(signature_offset + signature_size, BOOT_PAGE_SIZE)
    if payload_end > len(data):
        raise ImageError(f"{path}: declared components exceed file size")

    return {
        "kind": "boot-v4",
        "path": str(path),
        "file_size": len(data),
        "image_sha256": sha256(data),
        "header_size": header_size,
        "header_version": header_version,
        "page_size": BOOT_PAGE_SIZE,
        "kernel": component(data, "kernel", kernel_offset, kernel_size),
        "ramdisk": component(data, "ramdisk", ramdisk_offset, ramdisk_size),
        "boot_signature": component(
            data, "boot_signature", signature_offset, signature_size
        ),
        "payload_end": payload_end,
        "trailing_bytes": len(data) - payload_end,
    }


def parse_vendor_boot_v4(path: Path) -> dict[str, Any]:
    data = read_image(path)
    if len(data) < VENDOR_BOOT_V4_HEADER_SIZE or data[:8] != VENDOR_BOOT_MAGIC:
        raise ImageError(f"{path}: missing VNDRBOOT magic or v4 header")

    header_version = u32(data, 8, "header_version")
    page_size = u32(data, 12, "page_size")
    ramdisk_size = u32(data, 24, "vendor_ramdisk_size")
    header_size = u32(data, 2096, "header_size")
    dtb_size = u32(data, 2100, "dtb_size")
    table_size = u32(data, 2112, "vendor_ramdisk_table_size")
    table_entries = u32(data, 2116, "vendor_ramdisk_table_entry_num")
    table_entry_size = u32(data, 2120, "vendor_ramdisk_table_entry_size")
    bootconfig_size = u32(data, 2124, "bootconfig_size")

    if header_version != 4:
        raise ImageError(f"{path}: expected vendor_boot header v4, got v{header_version}")
    if page_size < VENDOR_BOOT_V4_HEADER_SIZE or page_size & (page_size - 1):
        raise ImageError(f"{path}: invalid page size {page_size}")
    if header_size != VENDOR_BOOT_V4_HEADER_SIZE:
        raise ImageError(
            f"{path}: expected v4 header size {VENDOR_BOOT_V4_HEADER_SIZE}, got {header_size}"
        )
    if table_entries:
        if table_entry_size < VENDOR_RAMDISK_TABLE_ENTRY_V4_SIZE:
            raise ImageError(f"{path}: ramdisk table entries are too small")
        if table_size != table_entries * table_entry_size:
            raise ImageError(
                f"{path}: table size {table_size} does not match "
                f"{table_entries} entries of {table_entry_size} bytes"
            )
    elif table_size != 0:
        raise ImageError(f"{path}: table has bytes but zero entries")

    ramdisk_offset = page_size
    dtb_offset = align(ramdisk_offset + ramdisk_size, page_size)
    table_offset = align(dtb_offset + dtb_size, page_size)
    bootconfig_offset = align(table_offset + table_size, page_size)
    payload_end = align(bootconfig_offset + bootconfig_size, page_size)
    if payload_end > len(data):
        raise ImageError(f"{path}: declared components exceed file size")

    return {
        "kind": "vendor_boot-v4",
        "path": str(path),
        "file_size": len(data),
        "image_sha256": sha256(data),
        "header_size": header_size,
        "header_version": header_version,
        "page_size": page_size,
        "vendor_ramdisk": component(data, "vendor_ramdisk", ramdisk_offset, ramdisk_size),
        "dtb": component(data, "dtb", dtb_offset, dtb_size),
        "vendor_ramdisk_table": component(data, "vendor_ramdisk_table", table_offset, table_size),
        "vendor_ramdisk_table_entries": table_entries,
        "vendor_ramdisk_table_entry_size": table_entry_size,
        "bootconfig": component(data, "bootconfig", bootconfig_offset, bootconfig_size),
        "payload_end": payload_end,
        "trailing_bytes": len(data) - payload_end,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot", type=Path, required=True, help="boot.img (header v4)")
    parser.add_argument("--init-boot", type=Path, required=True, help="init_boot.img (header v4)")
    parser.add_argument("--vendor-boot", type=Path, required=True, help="vendor_boot.img (header v4)")
    parser.add_argument(
        "--compare-dir",
        type=Path,
        help="also report exact whole-file matches against boot.img, init_boot.img, vendor_boot.img",
    )
    parser.add_argument(
        "--require-identical",
        action="store_true",
        help="exit nonzero if any image differs from --compare-dir",
    )
    args = parser.parse_args()
    if args.require_identical and args.compare_dir is None:
        parser.error("--require-identical requires --compare-dir")

    try:
        reports = [
            parse_boot_v4(args.boot),
            parse_boot_v4(args.init_boot),
            parse_vendor_boot_v4(args.vendor_boot),
        ]
        if args.compare_dir is not None:
            for report, name in zip(reports, ("boot.img", "init_boot.img", "vendor_boot.img")):
                reference_path = args.compare_dir / name
                if not reference_path.exists():
                    stem, suffix = name.rsplit(".", 1)
                    matches = [
                        candidate
                        for slot in ("_a", "_b")
                        if (candidate := args.compare_dir / f"{stem}{slot}.{suffix}").exists()
                    ]
                    if len(matches) != 1:
                        raise ImageError(
                            f"{args.compare_dir}: expected {name} or exactly one slot copy"
                        )
                    reference_path = matches[0]
                reference = read_image(reference_path)
                current = read_image(Path(report["path"]))
                report["reference"] = str(reference_path)
                report["exact_match"] = current == reference
        print(json.dumps(reports, indent=2))
        if args.require_identical and any(not report["exact_match"] for report in reports):
            return 1
    except ImageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
