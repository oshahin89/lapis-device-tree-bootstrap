#!/bin/bash
# Build the lapis XNU boot shim (default) or the register-dump probe as a raw
# arm64 Image (no payload).
#   tools/lapis_shim/build.sh OUTDIR dual [PROBE]   (PROBE: tvib, tdsi: behind Volume down)
#   tools/lapis_shim/build.sh OUTDIR [shim|dual|regdump|pstest|tchip|tufs|tnvt|tspi|tkeys|tpmic|tgauge|tchg|tvib|tdsi|tbl|tamp|taud|ttone|tcpu|tgpu|tcsf]
# Pack the shim with a SWEETXNU payload into a boot_b copy with
#   tools/lapis_boot_pack.py STOCK OUT.img --image OUTDIR/Image --append PAYLOAD
# and the probe with
#   tools/lapis_boot_pack.py STOCK OUT.img --image OUTDIR/Image
set -euo pipefail
cd "$(dirname "$0")"
OUT=$(realpath -m "${1:?usage: build.sh OUTDIR [shim|dual|regdump|pstest|tchip|tufs|tnvt|tspi|tkeys|tpmic|tgauge|tchg|tvib|tdsi|tbl|tamp|taud|ttone|tcpu|tgpu|tcsf]}")
WHAT=${2:-shim}
case "$WHAT" in
	shim)    MAIN=shim.c; EXTRA="loader.c ufsinit.c" ;;
	dual)    MAIN=shim.c; EXTRA="loader.c ufsinit.c dual.c pstore.c"; DEFS="-DLAPIS_DUAL ${DUAL_TEST:+-DLAPIS_DUAL_TEST=$DUAL_TEST}"
		 PROBE=${3:-}
		 [ -n "$PROBE" ] && DEFS="$DEFS -DLAPIS_DUAL_PROBE" ;;
	regdump) MAIN=regdump.c; EXTRA="pstore.c" ;;
	tchip)   MAIN=tchip.c; EXTRA="reboot.c" ;;
	tnvt)    MAIN=tnvt.c; EXTRA="" ;;
	tspi)    MAIN=tspi.c; EXTRA="" ;;
	tkeys)   MAIN=tkeys.c; EXTRA="" ;;
	tpmic)   MAIN=tpmic.c; EXTRA="" ;;
	tgauge)  MAIN=tgauge.c; EXTRA="" ;;
	tchg)    MAIN=tchg.c; EXTRA="" ;;
	tvib)    MAIN=tvib.c; EXTRA="" ;;
	tdsi)    MAIN=tdsi.c; EXTRA="" ;;
	tbl)     MAIN=tbl.c; EXTRA="" ;;
	tamp)    MAIN=tamp.c; EXTRA="" ;;
	taud)    MAIN=taud.c; EXTRA="" ;;
	ttone)   MAIN=ttone.c; EXTRA="" ;;
	tcpu)    MAIN=tcpu.c; EXTRA="" ;;
	tgpu)    MAIN=tgpu.c; EXTRA="" ;;
	tcsf)    MAIN=tcsf.c; EXTRA="" ;;
	tufs)    MAIN=tufs.c; EXTRA="reboot.c ufsinit.c" ;;
	pstest)  MAIN=pstest.c; EXTRA="pstore.c reboot.c" ;;
	*) echo "unknown target $WHAT" >&2; exit 2 ;;
esac
CC=aarch64-linux-gnu-gcc
CFLAGS="${DEFS:-} -I$OUT -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -Werror"
mkdir -p "$OUT"
if [ "$WHAT" = tnvt ] || [ "$WHAT" = tspi ]; then
	# the NT38771 firmware Android runs (HyperOS 4 TianMa 0x2B), as a C array
	python3 - ../../device/novatek_ts_fw_tm_hyperos4.bin "$OUT/nvt_fw.h" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
assert len(d) == 196608 and d[0x2f000] + d[0x2f001] == 0xff
with open(sys.argv[2], "w") as o:
    o.write("static const uint8_t nvt_fw[196608] = {\n")
    for i in range(0, len(d), 16):
        o.write(",".join("0x%02x" % b for b in d[i:i + 16]) + ",\n")
    o.write("};\n")
PY
fi
if [ "$WHAT" = tcsf ] || [ "${PROBE:-}" = tcsf ]; then
	# the Mali CSF firmware pulled from this phone (not in git: Arm binary)
	FW=../../build/gpu/mali_csffw.bin
	[ -f "$FW" ] || { echo "missing $FW (adb pull /vendor/firmware/mali_csffw.bin)" >&2; exit 1; }
	python3 - "$FW" "$OUT/mali_fw.h" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w") as o:
    o.write("static const uint8_t mali_fw[%d] __attribute__((aligned(8))) = {\n" % len(d))
    for i in range(0, len(d), 16):
        o.write(",".join("0x%02x" % b for b in d[i:i + 16]) + ",\n")
    o.write("};\n")
PY
fi
objs=("$OUT/start.o")
$CC $CFLAGS -c start.S -o "$OUT/start.o"
for f in $MAIN console.c $EXTRA; do
	$CC $CFLAGS -c $f -o "$OUT/${f%.*}.o"
	objs+=("$OUT/${f%.*}.o")
done
if [ -n "${PROBE:-}" ]; then
	# a probe behind Volume down in the dual shim: its shim_main / exc_report renamed
	[ -f "$PROBE.c" ] || { echo "unknown probe $PROBE" >&2; exit 2; }
	$CC $CFLAGS -Dshim_main=probe_main -Dexc_report=probe_exc_report -c $PROBE.c -o "$OUT/probe_$PROBE.o"
	objs+=("$OUT/probe_$PROBE.o")
fi
# LK loads us at 0x40000000 but the code must not depend on it: no absolute
# relocations in C objects (the header's image_size in start.o is the only one).
abs=$(aarch64-linux-gnu-readelf -r "${objs[@]:1}" | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
[ "$WHAT" = dual ] && WHAT=shim_dual
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	"${objs[@]}" -o "$OUT/$WHAT.elf"
aarch64-linux-gnu-objcopy -O binary "$OUT/$WHAT.elf" "$OUT/Image"
size=$((16#$(aarch64-linux-gnu-nm "$OUT/$WHAT.elf" | awk '$3=="_end"{print $1}')))
truncate -s "$size" "$OUT/Image"
ls -l "$OUT/Image"
