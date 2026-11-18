#!/bin/bash
# Make the deployable lapis iOS root: a copy of build/iosroot.img with the
# SWEET root tweaks (tools/ios-sweet/sweet-root-tweaks.py: CommCenter jobs
# removed, DataMigration LastSystemVersion, Setup skipped, SpringBoard
# BKNoWatchdogs, VirtualAudio off, containers, activation ark) plus ruby's ICL
# NOP (the lsd/MobileGestalt deadlock that keeps SpringBoard in
# _LSContextInitCommon). mkiosroot.sh never applied the tweaks; without them
# DataMigration reruns on every boot and SpringBoard never checks in.
#
#   tools/lapis_iosroot_tweak.sh [IN.img] [OUT.img]
#
# Needs sudo once (Linux hfsplus loop mount). The input is never modified.
set -euo pipefail
cd "$(dirname "$0")/.."
IN=${1:-build/iosroot.img}
OUT=${2:-build/iosroot_tweaked.img}
BLK=2438140          # iosroot 4K block holding dyld_shared_cache_arm64.15 +0x1619000
ORIG=ae3178046fe7cb9e08291c39dc4773429be26c35472c6fbd9135d5f2a225fd73
NOP=4a4895422dcec766b90b65c067ecb06ef8fd3e346c03ee7e2464a85f62a73b74

blk_sha() { dd if="$1" bs=4096 skip=$BLK count=1 status=none | sha256sum | cut -c1-64; }

echo "== copy $IN -> $OUT"
cp --sparse=always "$IN" "$OUT"

echo "== fsck (Linux mounts an unclean HFS+ read-only)"
fsck.hfsplus -fy "$OUT" | tail -3 || true

MNT=$(mktemp -d /tmp/iosroot.XXXXXX)
cleanup() { mountpoint -q "$MNT" && sudo umount "$MNT"; rmdir "$MNT" 2>/dev/null || true; }
trap cleanup EXIT
echo "== mount (sudo)"
sudo mount -t hfsplus -o loop,force,rw "$OUT" "$MNT"
grep -q " $MNT hfsplus rw" /proc/mounts || { echo "mounted read-only: abort"; exit 1; }

echo "== sweet-root-tweaks"
sudo python3 tools/ios-sweet/sweet-root-tweaks.py "$MNT"
sync
cleanup
trap - EXIT

echo "== ICL NOP at block $BLK"
s=$(blk_sha "$OUT")
if [ "$s" = "$NOP" ]; then
	echo "already patched"
elif [ "$s" = "$ORIG" ]; then
	python3 - "$OUT" $BLK <<'PY'
import sys
f = open(sys.argv[1], 'r+b'); off = int(sys.argv[2]) * 4096 + 0xf0c
f.seek(off); assert f.read(4) == bytes.fromhex('02255e95')
f.seek(off); f.write(bytes.fromhex('1f2003d5')); f.close()
PY
	[ "$(blk_sha "$OUT")" = "$NOP" ] || { echo "ICL NOP verify failed"; exit 1; }
	echo "patched"
else
	echo "block $BLK has unexpected content ($s): cache moved? not patching"; exit 1
fi

echo "== done"
ls -l "$OUT"
sha256sum "$OUT"
