#!/bin/sh
# Read-only SPI client/driver and probe-log snapshot from the attached lapis.
set -eu

ADB_BIN=${ADB_BIN:-adb}
if ! command -v "$ADB_BIN" >/dev/null 2>&1; then
    echo "adb not found" >&2
    exit 127
fi
if [ "$("$ADB_BIN" get-state 2>/dev/null || true)" != device ]; then
    echo "No authorized adb device is connected; no device command was run." >&2
    exit 2
fi

"$ADB_BIN" shell su -c sh <<'REMOTE'
echo '=== SPI bus clients ==='
for d in /sys/bus/spi/devices/*; do
    [ -e "$d" ] || continue
    echo "--- $d"
    for f in modalias uevent; do
        if [ -r "$d/$f" ]; then
            echo "[$f]"
            cat "$d/$f"
        fi
    done
    for f in driver of_node; do
        if [ -L "$d/$f" ]; then
            echo "[$f] $(readlink -f "$d/$f")"
        fi
    done
    if [ -r "$d/of_node/compatible" ]; then
        echo '[of_node/compatible: raw NUL-separated bytes]'
        od -An -tx1 "$d/of_node/compatible"
    fi
done
echo '=== Goodix DT child status ==='
gd=/sys/firmware/devicetree/base/soc/spi3@11013000/goodix@0
if [ -d "$gd" ]; then
    echo "DT child exists: $gd"
    if [ -r "$gd/status" ]; then
        printf 'status bytes: '
        od -An -tx1 "$gd/status"
    else
        echo 'status property: absent'
    fi
    if [ -r "$gd/compatible" ]; then
        printf 'compatible bytes: '
        od -An -tx1 "$gd/compatible"
    fi
    if [ -r "$gd/reg" ]; then
        printf 'reg cells: '
        od -An -tx4 "$gd/reg"
    fi
else
    echo 'Goodix DT child absent'
fi
echo '=== SPI host devices ==='
for d in /sys/bus/platform/devices/*spi3* /sys/devices/platform/soc/*spi3*; do
    [ -e "$d" ] || continue
    echo "--- $d"
    [ ! -L "$d/driver" ] || echo "driver: $(readlink -f "$d/driver")"
    [ ! -L "$d/of_node" ] || echo "of_node: $(readlink -f "$d/of_node")"
done
echo '=== Pinctrl visibility ==='
for p in /sys/devices/platform/soc/10005000.pinctrl/consumer:*spi3*; do
    [ -L "$p" ] || continue
    echo "$p -> $(readlink -f "$p")"
done
for p in /sys/class/pinctrl /sys/kernel/debug/pinctrl; do
    if [ -d "$p" ]; then echo "$p: present"; else echo "$p: absent"; fi
done
echo '=== Touch/SPI probe messages ==='
dmesg 2>/dev/null | grep -iE 'spi3|NVT-ts|nt38771|novatek|goodix.{0,24}(touch|probe|spi)|(touch|probe|spi).{0,24}goodix|gt9896' | tail -n 200 || true
echo '=== Interrupt counters ==='
grep -iE 'NVT-ts|goodix|11013000\.spi3|mtk-eint' /proc/interrupts || true
REMOTE
