#!/bin/bash
# Put tools/lapis_gputest (built by .github/workflows/lapis-gputest.yml) into
# the phone's iOS root, run once at every iOS boot by launchd
# (org.lapis.gputest); its results land in the UFS log as "LapisGPU client:".
#
#   tools/lapis_iosroot_add_gputest.sh path/to/lapis_gputest
#
# Needs the phone in Android (rooted, adb) and sudo here (Linux hfsplus loop
# mount). iOS writes to its own root, so the partition is read from the phone
# first; the program and the launchd job are added to a copy, and only the 4
# KiB blocks that changed are written back (pushed as one file, placed with
# dd on the phone, then read back and compared). Asks before writing.
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=$(realpath "${1:?usage: lapis_iosroot_add_gputest.sh path/to/lapis_gputest}")
PART=${LAPIS_IOSROOT_DEV:-/dev/block/by-name/iosroot}
ORIG=build/iosroot_phone.img
WORK=build/iosroot_phone_gputest.img
PATCH=build/iosroot_gputest_patch

su_sh() { adb shell "su -c '$1'"; }

echo "== phone"
adb get-state >/dev/null
DEV=$(su_sh "readlink -f $PART" | tr -d '\r')
SIZE=$(su_sh "blockdev --getsize64 $DEV" | tr -d '\r')
echo "$PART -> $DEV, $SIZE bytes"
[ "$SIZE" -ge $((8 << 30)) ] || { echo "unexpected size: abort"; exit 1; }

echo "== read the iOS root off the phone (a few minutes)"
adb exec-out "su -c 'dd if=$DEV bs=4194304 2>/dev/null'" > "$ORIG"
[ "$(stat -c %s "$ORIG")" = "$SIZE" ] || { echo "short read: abort"; exit 1; }
cp --sparse=always "$ORIG" "$WORK"

echo "== add the program and the launchd job"
fsck.hfsplus -fy "$WORK" | tail -2 || true
MNT=$(mktemp -d /tmp/iosroot.XXXXXX)
cleanup() { mountpoint -q "$MNT" && sudo umount "$MNT"; rmdir "$MNT" 2>/dev/null || true; }
trap cleanup EXIT
sudo mount -t hfsplus -o loop,force,rw "$WORK" "$MNT"
grep -q " $MNT hfsplus rw" /proc/mounts || { echo "mounted read-only: abort"; exit 1; }
sudo install -d -o 0 -g 0 -m 755 "$MNT/usr/local/bin"
sudo install -o 0 -g 0 -m 755 "$BIN" "$MNT/usr/local/bin/lapis_gputest"
sudo python3 - "$MNT/System/Library/xpc/launchd.plist" <<'PY'
import os, plistlib, sys
path = sys.argv[1]
st = os.stat(path)
with open(path, 'rb') as f:
    plist = plistlib.load(f)
plist['LaunchDaemons']['/System/Library/LaunchDaemons/org.lapis.gputest.plist'] = {
    'Label': 'org.lapis.gputest',
    'ProgramArguments': ['/usr/local/bin/lapis_gputest'],
    'RunAtLoad': True,
    'KeepAlive': False,
    'UserName': 'root',
    'StandardOutPath': '/var/root/lapis_gputest.log',
    'StandardErrorPath': '/var/root/lapis_gputest.log',
}
with open(path, 'wb') as f:
    plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
os.chown(path, st.st_uid, st.st_gid)
os.chmod(path, st.st_mode & 0o7777)
print('launchd.plist: org.lapis.gputest added')
PY
sync
cleanup
trap - EXIT
fsck.hfsplus -fy "$WORK" | tail -2 || true

echo "== changed blocks"
python3 - "$ORIG" "$WORK" "$PATCH" <<'PY'
import sys
orig, work, out = sys.argv[1:4]
B = 4096
runs, data = [], open(out + '.bin', 'wb')
with open(orig, 'rb') as a, open(work, 'rb') as b:
    n, start, pos = 0, None, 0
    while True:
        x, y = a.read(B * 256), b.read(B * 256)
        if not y:
            break
        for i in range(0, len(y), B):
            blk = n + i // B
            if x[i:i + B] != y[i:i + B]:
                if start is None or blk != prev + 1:
                    if start is not None:
                        runs.append((pos0, start, prev - start + 1))
                    start, pos0 = blk, pos
                data.write(y[i:i + B])
                pos += 1
                prev = blk
        n += len(y) // B
    if start is not None:
        runs.append((pos0, start, prev - start + 1))
data.close()
with open(out + '.txt', 'w') as f:
    for r in runs:
        f.write('%d %d %d\n' % r)
print(f'{sum(r[2] for r in runs)} blocks in {len(runs)} runs')
PY
[ -s "$PATCH.txt" ] || { echo "nothing changed"; exit 0; }

read -r -p "Write these blocks to $DEV on the phone? Type YES: " answer
[ "$answer" = YES ] || { echo "not written"; exit 1; }
adb push "$PATCH.bin" /data/local/tmp/gputest_patch.bin >/dev/null
adb push "$PATCH.txt" /data/local/tmp/gputest_patch.txt >/dev/null
su_sh "while read s d c; do dd if=/data/local/tmp/gputest_patch.bin of=$DEV bs=4096 skip=\$s seek=\$d count=\$c conv=notrunc 2>/dev/null || echo FAIL \$d; done < /data/local/tmp/gputest_patch.txt; sync"

echo "== read back"
bad=0
while read -r s d c; do
	want=$(dd if="$WORK" bs=4096 skip="$d" count="$c" status=none | sha256sum | cut -c1-64)
	got=$(su_sh "dd if=$DEV bs=4096 skip=$d count=$c 2>/dev/null | sha256sum" | cut -c1-64)
	[ "$want" = "$got" ] || { echo "run at block $d differs"; bad=1; }
done < "$PATCH.txt"
su_sh "rm /data/local/tmp/gputest_patch.bin /data/local/tmp/gputest_patch.txt"
[ "$bad" = 0 ] && echo "== done: boot iOS (Vol-up); the results are 'LapisGPU client:' lines in the UFS log" || exit 1
