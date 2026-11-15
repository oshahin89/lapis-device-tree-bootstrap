#!/bin/bash
# Boot the lapis kernel to PID 1 in QEMU and check the milestones. Offline
# regression: no device, no IPSW. Run after a new CI kernel or a patch change.
#
#   tools/lapis_qemu_regress.sh KERNEL [ROOT_HFS]
#
# KERNEL     an unstripped kernel.development.sweet (from CI).
# ROOT_HFS   an HFS+ RAM-disk root (tools/lapis_mkroot_hfs.sh); if omitted a
#            fresh 8 MiB one with ios-sweet's init is built (needs sudo+hfsprogs).
# Builds the shim, an 8-CPU --debug ADT, the payload (rd=md0), boots in QEMU
# (--smp 113 for the lapis MPIDR layout, 8 cores) and greps the console mirror
# for: 8/8 CPUs, SweetRoot mounted, PID 1, a heartbeat. Exit 0 if all pass.
set -euo pipefail
cd "$(dirname "$0")/.."
K=${1:?usage: lapis_qemu_regress.sh KERNEL [ROOT_HFS]}
ROOT=${2:-}
W=$(mktemp -d /tmp/lapis-regress.XXXXXX)
trap 'rm -rf "$W"' EXIT

[ -n "$ROOT" ] || {
  [ -f build/sweet-init ] || { echo "need build/sweet-init (clang -target arm64-apple-ios16.0 -c tools/ios-sweet/sweet-init.S + mkinit.py)"; exit 2; }
  ROOT="$W/root.img"; sudo tools/lapis_mkroot_hfs.sh "$ROOT" build/sweet-init >/dev/null
}

tools/lapis_shim/build.sh "$W/shim" shim >/dev/null
# QEMU mode lists CPUs from --qemu-smp (MPIDR 0,0x100,..); run QEMU with --smp
# 16*(N-1)+1 so those MPIDRs exist (QEMU GICv3 clusters are 16 CPUs).
python3 tools/lapis_mkadt.py "$W/q.adt" --qemu --qemu-lapis-fb --qemu-smp 8 --debug 2>/dev/null
python3 tools/ios-sweet/mkpayload.py "$K" "$W/q.adt" "$W/p.bin" --ramdisk "$ROOT" \
  --cmdline "debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress sweethw=0 wdt=-1 rd=md0" >/dev/null 2>&1
python3 - "$W/shim/Image" "$W/p.bin" "$W/img" <<'PY'
import struct, sys
img, pl, out = sys.argv[1:4]
b = bytearray(open(img,"rb").read()); b += b"\0"*(-len(b)%0x1000); b += open(pl,"rb").read()
struct.pack_into("<Q", b, 16, len(b)); open(out,"wb").write(bytes(b))
PY
python3 tools/lapis_shim/qemu_shim.py "$W/img" "$W/run" --secs 150 --kernel "$K" --smp 113 --pstore >/dev/null 2>&1

log="$W/run/pstore.txt"; [ -s "$log" ] || log="$W/run/screen.txt"
pass=0; fail=0
check() { if grep -aq "$2" "$log"; then echo "  ok   $1"; pass=$((pass+1)); else echo "  FAIL $1"; fail=$((fail+1)); fi; }
echo "regression against $(basename "$K"):"
check "8/8 CPUs init"     "8/8 CPUs initialized"
check "SweetRoot mounted" "mounted SweetRoot"
check "PID 1 running"     "init running as PID 1"
check "heartbeat"         "still alive"
echo "$pass passed, $fail failed"
exit $((fail>0))
