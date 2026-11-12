#!/bin/bash
# Build the lapis XNU boot shim (default) or the register-dump probe as a raw
# arm64 Image (no payload).
#   tools/lapis_shim/build.sh OUTDIR [shim|regdump|pstest|tchip]
# Pack the shim with a SWEETXNU payload into a boot_b copy with
#   tools/lapis_boot_pack.py STOCK OUT.img --image OUTDIR/Image --append PAYLOAD
# and the probe with
#   tools/lapis_boot_pack.py STOCK OUT.img --image OUTDIR/Image
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(realpath -m "${1:?usage: build.sh OUTDIR [shim|regdump|pstest|tchip]}")
WHAT=${2:-shim}
case "$WHAT" in
	shim)    MAIN=shim.c; EXTRA="loader.c" ;;
	regdump) MAIN=regdump.c; EXTRA="pstore.c" ;;
	tchip)   MAIN=tchip.c; EXTRA="reboot.c" ;;
	pstest)  MAIN=pstest.c; EXTRA="pstore.c reboot.c" ;;
	*) echo "unknown target $WHAT" >&2; exit 2 ;;
esac
CC=aarch64-linux-gnu-gcc
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -Werror"
mkdir -p "$OUT"
objs=("$OUT/start.o")
$CC $CFLAGS -c start.S -o "$OUT/start.o"
for f in $MAIN console.c $EXTRA; do
	$CC $CFLAGS -c $f -o "$OUT/${f%.*}.o"
	objs+=("$OUT/${f%.*}.o")
done
# LK loads us at 0x40000000 but the code must not depend on it: no absolute
# relocations in C objects (the header's image_size in start.o is the only one).
abs=$(aarch64-linux-gnu-readelf -r "${objs[@]:1}" | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	"${objs[@]}" -o "$OUT/$WHAT.elf"
aarch64-linux-gnu-objcopy -O binary "$OUT/$WHAT.elf" "$OUT/Image"
size=$((16#$(aarch64-linux-gnu-nm "$OUT/$WHAT.elf" | awk '$3=="_end"{print $1}')))
truncate -s "$size" "$OUT/Image"
ls -l "$OUT/Image"
