#!/usr/bin/env python3
"""Verify the LAPIS patch queue against the pinned clean XNU source tag."""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "reference/apple-xnu-8796.141.3"
PATCH_DIR = ROOT / "device/xnu-lapis/patches"
EXPECTED_HEAD = "1b191cb58250d0705d8a51287127505aa4bc0789"


def run(args, *, cwd=None, stdout=None):
    subprocess.run(args, cwd=cwd, check=True, stdout=stdout)


def main():
    head = subprocess.check_output(
        ["git", "-C", str(SOURCE), "rev-parse", "HEAD"], text=True
    ).strip()
    if head != EXPECTED_HEAD:
        raise SystemExit(f"Pinned XNU HEAD mismatch: expected {EXPECTED_HEAD}, got {head}")

    patches = sorted(PATCH_DIR.glob("*.patch"))
    if not patches:
        raise SystemExit(f"No patches found in {PATCH_DIR}")

    with tempfile.TemporaryDirectory(prefix="lapis-xnu-patch-preflight-") as tmp:
        with subprocess.Popen(
            ["git", "-C", str(SOURCE), "archive", "HEAD"], stdout=subprocess.PIPE
        ) as archive:
            assert archive.stdout is not None
            extract = subprocess.run(["tar", "-x", "-C", tmp], stdin=archive.stdout)
            archive.stdout.close()
            if archive.wait() != 0 or extract.returncode != 0:
                raise SystemExit("Could not export the pinned XNU source")

        for patch in patches:
            args = ["git", "apply", "--check", "--whitespace=error", str(patch)]
            run(args, cwd=tmp)
            run(["git", "apply", str(patch)], cwd=tmp)

        for patch in reversed(patches):
            run(["git", "apply", "--reverse", "--check", str(patch)], cwd=tmp)

    print(f"PASS: {len(patches)} patches apply and reverse cleanly on XNU {EXPECTED_HEAD}")


if __name__ == "__main__":
    main()
