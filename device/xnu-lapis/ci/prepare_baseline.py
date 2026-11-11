#!/usr/bin/env python3
"""Assemble the LAPIS XNU patch queue on top of the pinned ios-topaz series.

The upstream ios-sweet / ios-topaz trees have no license file, so they are
never copied into this repository. This script fetches (or copies from a local
checkout) the pinned topaz commit at build time, then:

  1. takes topaz's xnu/patches (the ios-sweet 0001-0079 series plus topaz's
     0080-0103 device patches),
  2. normalizes .patch/.sh files to LF,
  3. removes the topaz (Qualcomm SM6225) device patches listed in
     topaz-patches-to-remove.txt (same list ruby-ios-bringup uses),
  4. adds this project's patches from device/xnu-lapis/patches/,
  5. prints the canonical patchset hash (same algorithm as ruby-ios-bringup).

Usage:
  prepare_baseline.py OUT_DIR [--base-dir LOCAL_TOPAZ_CHECKOUT] [--expect-hash H]

OUT_DIR receives only the final patch queue (*.patch, *.sh).
"""
import argparse
import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
LAPIS_PATCHES = HERE.parent / "patches"
REMOVE_LIST = HERE / "topaz-patches-to-remove.txt"

BASE_REPO = "https://github.com/MakrSas/ios-topaz.git"
BASE_SHA = "5095ac19dbdcf37b64b81fe443f4027ad51b84bf"


def git(*args, cwd=None):
    return subprocess.run(["git", *args], cwd=cwd, check=True,
                          capture_output=True, text=True).stdout.strip()


def queue_files(root):
    return sorted(p for p in Path(root).iterdir()
                  if p.is_file() and p.suffix in (".patch", ".sh"))


def patchset_hash(root):
    lines = []
    for p in queue_files(root):
        digest = hashlib.sha256(p.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
        lines.append(f"{digest}  {p.name}\n")
    return hashlib.sha256("".join(lines).encode()).hexdigest(), len(lines)


def fetch_base(work, base_dir):
    if base_dir:
        head = git("-C", str(base_dir), "rev-parse", "HEAD")
        if head != BASE_SHA:
            sys.exit(f"base checkout is at {head}, expected {BASE_SHA}")
        return Path(base_dir)
    repo = Path(work) / "topaz"
    git("init", "-q", str(repo))
    git("-C", str(repo), "fetch", "-q", "--depth", "1", BASE_REPO, BASE_SHA)
    git("-C", str(repo), "checkout", "-q", "FETCH_HEAD")
    head = git("-C", str(repo), "rev-parse", "HEAD")
    if head != BASE_SHA:
        sys.exit(f"fetched {head}, expected {BASE_SHA}")
    return repo


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--base-dir", help="local ios-topaz checkout at the pinned commit")
    ap.add_argument("--expect-hash")
    a = ap.parse_args()

    out = Path(a.out)
    if out.exists():
        sys.exit(f"{out} already exists")

    with tempfile.TemporaryDirectory(prefix="lapis-baseline-") as work:
        base = fetch_base(work, a.base_dir)
        src = base / "xnu" / "patches"
        out.mkdir(parents=True)
        for p in queue_files(src):
            (out / p.name).write_bytes(p.read_bytes().replace(b"\r\n", b"\n"))

    for name in REMOVE_LIST.read_text().split():
        target = out / name
        if not target.is_file():
            sys.exit(f"topaz patch listed for removal is missing: {name}")
        target.unlink()

    for p in queue_files(LAPIS_PATCHES):
        dest = out / p.name
        if dest.exists():
            sys.exit(f"LAPIS patch name collides with baseline patch: {p.name}")
        dest.write_bytes(p.read_bytes().replace(b"\r\n", b"\n"))

    digest, count = patchset_hash(out)
    print(f"patchset: {digest} ({count} files)")
    if a.expect_hash and digest != a.expect_hash:
        sys.exit(f"patchset mismatch: expected {a.expect_hash}")
    print(f"baseline ready: {out}")


if __name__ == "__main__":
    main()
