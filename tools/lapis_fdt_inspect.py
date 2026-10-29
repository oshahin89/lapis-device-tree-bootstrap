#!/usr/bin/env python3
"""Read selected hardware facts from a flattened device tree via dtc."""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path


SELECT = re.compile(r"(?:^|/)(?:cpu@|interrupt-controller@|timer$|spi3@|novatek@|panel|watchdog@|chosen$)", re.I)
KEYS = re.compile(r"^(?:compatible|device_type|status|reg|interrupts|interrupt-parent|clock-frequency|method|spi-max-frequency|pinctrl-names|pinctrl-\d+|novatek,(?:reset-gpio|irq-gpio)|reset-gpios|.*panel.*|.*gpio.*)$", re.I)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dtb", type=Path)
    args = ap.parse_args()
    dtc = shutil.which("dtc")
    if not dtc:
        ap.error("dtc is required (device-tree-compiler package)")
    if not args.dtb.is_file():
        ap.error(f"not a file: {args.dtb}")
    proc = subprocess.run([dtc, "-I", "dtb", "-O", "dts", str(args.dtb)],
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode:
        sys.stderr.write(proc.stderr)
        return proc.returncode

    stack = []
    rows = []
    for line in proc.stdout.splitlines():
        s = line.strip()
        if not s or s.startswith("/dts-v1;"):
            continue
        if s == "};" or s == "}":
            if stack:
                stack.pop()
            continue
        if s.endswith("{"):
            name = s[:-1].strip()
            stack.append(name)
            continue
        if ":" in s and not s.startswith("/"):
            continue
        if "=" not in s:
            continue
        key = s.split("=", 1)[0].strip()
        path = "/" + "/".join(part for part in stack if part != "/")
        if SELECT.search(path) and KEYS.match(key):
            rows.append((path, s.rstrip(";")))

    prior = None
    for path, prop in rows:
        if path != prior:
            print(path)
            prior = path
        print(f"  {prop}")

    spi3 = "/soc/spi3@11013000"
    clients = {}
    for path, prop in rows:
        if not path.startswith(spi3 + "/") or path.count("/") != spi3.count("/") + 1:
            continue
        match = re.fullmatch(r"reg\s*=\s*<\s*(0x[0-9a-fA-F]+|[0-9]+)\s*>", prop.rstrip(";"))
        if match:
            clients.setdefault(int(match.group(1), 0), []).append(path)
    for cs, nodes in sorted(clients.items()):
        if len(nodes) > 1:
            print(f"WARNING: SPI3 chip-select {cs} is declared by multiple DT children:")
            for path in nodes:
                print(f"  {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
