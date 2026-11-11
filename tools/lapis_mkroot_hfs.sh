#!/bin/bash
# Build a small HFS+ root volume for an XNU RAM-disk root (rd=md0).
#
#   sudo tools/lapis_mkroot_hfs.sh OUT.img INIT [SIZE_MB]
#
# INIT is a static arm64 Mach-O (e.g. ios-sweet's sweet-init built with
# tools/ios-sweet/mkinit.py); it becomes /sbin/launchd, root:wheel 0755.
# The volume is labelled SweetRoot and holds only /sbin, /dev and /private.
# Needs hfsprogs (mkfs.hfsplus, fsck.hfsplus) and root for the loop mount.
# Nothing outside OUT.img and a temporary mount point is touched.
set -euo pipefail

OUT=${1:?usage: lapis_mkroot_hfs.sh OUT.img INIT [SIZE_MB]}
INIT=${2:?usage: lapis_mkroot_hfs.sh OUT.img INIT [SIZE_MB]}
SIZE_MB=${3:-8}

[ "$(id -u)" = 0 ] || { echo "run with sudo (loop mount)" >&2; exit 1; }
command -v mkfs.hfsplus >/dev/null || { echo "install hfsprogs first" >&2; exit 1; }
[ -f "$INIT" ] || { echo "no such init: $INIT" >&2; exit 1; }
[ ! -e "$OUT" ] || { echo "$OUT already exists" >&2; exit 1; }

MNT=$(mktemp -d /tmp/lapis-hfsroot.XXXXXX)
cleanup() { mountpoint -q "$MNT" && umount "$MNT"; rmdir "$MNT"; }
trap cleanup EXIT

truncate -s "${SIZE_MB}M" "$OUT"
mkfs.hfsplus -v SweetRoot "$OUT" >/dev/null
mount -t hfsplus -o loop,rw "$OUT" "$MNT"
install -d -m 0755 -o 0 -g 0 "$MNT/sbin" "$MNT/dev" "$MNT/private" "$MNT/private/var"
install -m 0755 -o 0 -g 0 "$INIT" "$MNT/sbin/launchd"
sync
umount "$MNT"
fsck.hfsplus -n "$OUT"
# hand the image back to the invoking user
[ -n "${SUDO_UID:-}" ] && chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$OUT"
sha256sum "$OUT"
