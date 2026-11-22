#!/bin/bash
# The official way to package an iOS build for lapis (device/lapis_dual_boot.md):
# one boot_b image with Android by default and iOS on Volume up.
#   tools/lapis_dual_image.sh PAYLOAD.bin OUT.img
# PAYLOAD is a SWEETXNU payload from tools/ios-sweet/mkpayload.py. Only writes
# files; flash OUT.img from rooted Android with
#   dd if=OUT.img of=/dev/block/by-name/boot_b bs=4M && sync
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PAYLOAD=$(realpath "${1:?usage: lapis_dual_image.sh PAYLOAD.bin OUT.img}")
OUT=$(realpath -m "${2:?usage: lapis_dual_image.sh PAYLOAD.bin OUT.img}")
STOCK=$ROOT/device/hyperos4_readback/boot.img
"$ROOT/tools/lapis_shim/build.sh" "$ROOT/build/dual_build" dual >/dev/null
python3 "$ROOT/tools/lapis_boot_pack.py" "$STOCK" "$OUT" --image "$ROOT/build/dual_build/Image" \
	--append "$PAYLOAD" --android \
	--expect-stock-sha256 a01d0f11fb35c95f7410df5a27c379edaec65bbef01ab994c74b88579f6bd6c5
