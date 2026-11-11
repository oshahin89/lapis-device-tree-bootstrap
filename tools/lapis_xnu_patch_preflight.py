#!/usr/bin/env python3
"""Offline check of the LAPIS XNU patch queue against the pinned clean XNU.

1. assembles the queue from the local ios-topaz checkout
   (reference/xnu-topaz-baseline) with device/xnu-lapis/ci/prepare_baseline.py;
2. exports the pinned xnu-8796.141.3 source to a temporary directory;
3. applies the queue the way darwin-xnu-build does, but stops on the first
   patch that does not apply (device/xnu-lapis/ci/strict_apply.py).

The stage scripts fetch libpthread, BearSSL and HFS from GitHub. Pass
--no-scripts to stay offline; patches after 0030 then cannot be checked.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
XNU = ROOT / "reference/apple-xnu-8796.141.3"
TOPAZ = ROOT / "reference/xnu-topaz-baseline"
CI = ROOT / "device/xnu-lapis/ci"
EXPECTED_HEAD = "1b191cb58250d0705d8a51287127505aa4bc0789"


def main():
    head = subprocess.check_output(["git", "-C", str(XNU), "rev-parse", "HEAD"], text=True).strip()
    if head != EXPECTED_HEAD:
        sys.exit(f"Pinned XNU HEAD mismatch: expected {EXPECTED_HEAD}, got {head}")

    with tempfile.TemporaryDirectory(prefix="lapis-xnu-preflight-") as tmp:
        queue, src = Path(tmp) / "queue", Path(tmp) / "xnu"
        subprocess.run([sys.executable, str(CI / "prepare_baseline.py"), str(queue),
                        "--base-dir", str(TOPAZ)], check=True)
        src.mkdir()
        archive = subprocess.Popen(["git", "-C", str(XNU), "archive", "HEAD"], stdout=subprocess.PIPE)
        subprocess.run(["tar", "-x", "-C", str(src)], stdin=archive.stdout, check=True)
        if archive.wait() != 0:
            sys.exit("could not export the pinned XNU source")
        subprocess.run(["git", "init", "-q"], cwd=src, check=True)
        subprocess.run([sys.executable, str(CI / "strict_apply.py"), str(queue), str(src),
                        *sys.argv[1:]], check=True)


if __name__ == "__main__":
    main()
