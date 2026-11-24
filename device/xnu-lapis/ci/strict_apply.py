#!/usr/bin/env python3
"""Apply a patch queue to an XNU tree the way darwin-xnu-build does, but strictly.

darwin-xnu-build's patches() step runs every *.sh, then `git apply`s every
*.patch and only *warns* (and skips) when a patch does not apply. A silently
skipped patch produces a kernel that is missing part of the port. This script
mirrors the same order (the harness's own sed edits, then *.sh, then *.patch)
and stops at the first patch that does not apply.

Usage:
  strict_apply.py PATCH_DIR XNU_DIR [--no-scripts]

  --no-scripts  skip the *.sh stage scripts (they fetch libpthread, BearSSL and
                HFS sources from the network); patches that depend on staged
                files are then reported as dependent, not as failures.
"""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
import sys
from pathlib import Path


def harness_seds(xnu):
    """The text edits build.sh's patches() makes before applying patches."""
    edits = [
        ("makedefs/MakeInc.def",
         r"^LDFLAGS_KERNEL_SDK\t= -L\$\(SDKROOT\).*$",
         "LDFLAGS_KERNEL_SDK\t= -L$(FAKEROOT_DIR)/usr/local/lib/kernel -lfirehose_kernel"),
        ("makedefs/MakeInc.def",
         r"^(INCFLAGS_SDK[ \t]*=) -I\$\(SDKROOT\)/usr/local/include/(kernel)?",
         r"\1 -I$(FAKEROOT_DIR)/usr/local/include/kernel"),
        ("makedefs/MakeInc.cmd",
         r"export MIG := \$\(shell \$\(XCRUN\) -sdk \$\(SDKROOT\) -find mig\)",
         'export MIG := $(shell find $(FAKEROOT_DIR) -name "mig")'),
        ("makedefs/MakeInc.cmd",
         r"export MIGCOM := \$\(shell \$\(XCRUN\) -sdk \$\(SDKROOT\) -find migcom\)",
         'export MIGCOM := $(shell find $(FAKEROOT_DIR) -name "migcom")'),
        ("bsd/sys/make_symbol_aliasing.sh",
         r'^AVAILABILITY_PL="\$\{SDKROOT\}/\$\{DRIVERKITROOT\}',
         'AVAILABILITY_PL="${FAKEROOT_DIR}'),
        ("libsyscall/Libsyscall.xcconfig", r"^#include.*BSD.xcconfig.*$", ""),
    ]
    for rel, pat, repl in edits:
        path = Path(xnu) / rel
        text = path.read_text()
        path.write_text(re.sub(pat, repl, text, flags=re.M))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("patches")
    ap.add_argument("xnu")
    ap.add_argument("--no-scripts", action="store_true")
    a = ap.parse_args()

    patches = Path(a.patches).resolve()
    xnu = Path(a.xnu).resolve()
    harness_seds(xnu)

    env = dict(os.environ, MACHINE_CONFIG="SWEET", MACOS_VERSION="13.5")
    env.setdefault("LAPIS_NT38771_FIRMWARE",
                   str(Path(__file__).resolve().parents[3] / "device" / "novatek_ts_fw_tm.bin"))
    env.setdefault("LAPIS_PROJECT_ROOT", str(Path(__file__).resolve().parents[3]))
    if not a.no_scripts and shutil.which("xcrun") is None:
        # Linux preflight only: the stage scripts copy SDK headers that the
        # macOS build needs. Point xcrun at an empty placeholder SDK so the
        # staging and patch order can still be checked; this tree is not
        # buildable and is never uploaded.
        shim = Path(tempfile.mkdtemp(prefix="lapis-xcrun-"))
        sdk = shim / "sdk"
        (sdk / "usr/include").mkdir(parents=True)
        for h in ("TargetConditionals.h", "AvailabilityMacros.h", "Availability.h",
                  "AvailabilityInternal.h", "AvailabilityVersions.h"):
            (sdk / "usr/include" / h).write_text("/* lapis linux preflight placeholder */\n")
        (shim / "xcrun").write_text(f"#!/bin/sh\necho {sdk}\n")
        (shim / "xcrun").chmod(0o755)
        env["PATH"] = f"{shim}:{env['PATH']}"
        print("note: using a placeholder xcrun SDK (Linux preflight, not buildable)")
    if not a.no_scripts:
        for script in sorted(patches.glob("*.sh")):
            print(f"script  {script.name}")
            subprocess.run(["bash", str(script)], cwd=xnu, env=env, check=True)

    applied = 0
    for patch in sorted(patches.glob("*.patch")):
        r = subprocess.run(["git", "apply", "--check", str(patch)], cwd=xnu,
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"FAIL    {patch.name}\n{r.stderr}", file=sys.stderr)
            print(f"{applied} patches applied before the failure", file=sys.stderr)
            sys.exit(1)
        subprocess.run(["git", "apply", str(patch)], cwd=xnu, check=True)
        applied += 1
    print(f"PASS: {applied} patches applied strictly")


if __name__ == "__main__":
    main()
