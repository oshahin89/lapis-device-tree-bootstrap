#!/bin/bash
# Build the lapis XNU boot shim as a raw arm64 Image (no payload).
#   tools/lapis_shim/build.sh OUTDIR
# Pack it with a SWEETXNU payload into a boot_b copy with
#   tools/lapis_boot_pack.py STOCK --image OUTDIR/Image --append PAYLOAD OUT.img
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(realpath -m "${1:?usage: build.sh OUTDIR}")
CC=aarch64-linux-gnu-gcc
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -Werror"
mkdir -p "$OUT"
for f in start.S shim.c console.c loader.c; do
	$CC $CFLAGS -c $f -o "$OUT/${f%.*}.o"
done
# LK loads us at 0x40000000 but the code must not depend on it: no absolute
# relocations in C objects (the header's image_size in start.o is the only one).
abs=$(aarch64-linux-gnu-readelf -r "$OUT"/shim.o "$OUT"/console.o "$OUT"/loader.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	"$OUT"/start.o "$OUT"/shim.o "$OUT"/console.o "$OUT"/loader.o -o "$OUT/shim.elf"
aarch64-linux-gnu-objcopy -O binary "$OUT/shim.elf" "$OUT/Image"
size=$((16#$(aarch64-linux-gnu-nm "$OUT/shim.elf" | awk '$3=="_end"{print $1}')))
truncate -s "$size" "$OUT/Image"
ls -l "$OUT/Image"
